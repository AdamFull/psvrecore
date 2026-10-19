#!/usr/bin/env python3
"""Same-source check of the USSE reference interpreter.

Generates random Cg vertex programs (expressions over attributes, uniforms and literals, with branches and loops),
evaluates each expression on the host (numpy, float32), compiles the program with the SDK's shader compiler, runs
it in the interpreter (psvusse --run) and compares the outputs:

    usse_exec_check.py --psvusse build/windows-x64-release/tools/psvusse/psvusse.exe --sdk <SDK root>
                       [--count 500] [--seed 1] [--keep DIR] [--show 10]

Values are compared with a tolerance that grows with the magnitudes an expression goes through, and with the
approximate operations it uses (division, square roots, exponentials, trigonometry). Functions that are not
continuous (floor, frac, step, comparisons) are only applied to values computed exactly (sums, products and
moves of short binary fractions), so an approximation cannot move them across a step.

Local only: it needs an SDK (not part of this repository). Everything it writes goes to a temporary folder (or
--keep): compiled shaders are derived from the SDK, never commit them.
"""

import argparse
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

import numpy as np

F = np.float32
SWIZ = 'xyzw'
SPECIAL = ['mul', 'index', 'half', 'int', 'int', 'sincos', 'pow']  # (--special)
FLOW = ['branch', 'loop', 'uloop']  # (--flow)


def f4(v):
    return np.array(v, dtype=np.float32)


class Node:
    """A float4 expression: its Cg text, its value, whether it is exact, its error bound, whether it depends on
    literals only (the compiler folds those itself), and what made it (floor, ceil: see Gen.expr)."""

    def __init__(self, text, value, exact, err, const=False, kind=''):
        self.text, self.value, self.exact, self.err = text, f4(value), exact, err
        self.const, self.kind = const, kind


class Gen:
    def __init__(self, rnd, attrs, uniforms):
        self.rnd = rnd
        self.attrs = attrs        # name -> value
        self.uniforms = uniforms  # name -> value
        self.mats = {}            # name -> 4x4 (rows)
        self.arrays = {}          # name -> n x 4

    def special(self, depth, a):
        """Matrices, indexed arrays, half precision, integers, trigonometry, powers (or None)."""
        rnd = self.rnd
        fn = rnd.choice(SPECIAL)
        with np.errstate(all='ignore'):
            if fn == 'mul' and self.mats:
                name = rnd.choice(list(self.mats))
                m = self.mats[name]
                v = (m.astype(np.float64) @ a.value.astype(np.float64)).astype(np.float32)
                mag = float(np.max(np.abs(m))) * float(np.max(np.abs(a.value))) * 4
                return Node('mul(%s, %s)' % (name, a.text), v, False, a.err * float(np.max(np.sum(np.abs(m), 1))) +
                            mag * 1e-6)
            if fn == 'index' and self.arrays and a.exact and not a.const:
                name = rnd.choice(list(self.arrays))
                arr = self.arrays[name]
                c = rnd.randint(0, 3)
                k = int(np.clip(np.floor(a.value[c]), 0, len(arr) - 1))
                return Node('%s[(int)clamp(floor(%s.%s), 0, %d)]' % (name, a.text, SWIZ[c], len(arr) - 1), arr[k],
                            True, 0.0)
            if fn == 'half':
                b = self.expr(depth - 1)
                op = rnd.choice(['+', '*', '-'])
                ha, hb = a.value.astype(np.float16), b.value.astype(np.float16)
                v = {'+': ha + hb, '*': ha * hb, '-': ha - hb}[op].astype(np.float32)
                if not np.all(np.isfinite(v)) or float(np.max(np.abs(v))) > 1000:
                    return None
                mag = float(np.max(np.abs(np.concatenate([a.value, b.value, v]))))
                # (the compiler may compute halves at full precision: allow either)
                return Node('(float4)((half4)%s %s (half4)%s)' % (a.text, op, b.text), v, False,
                            mag * 2e-3 + (a.err + b.err) * (1 + mag))
            if fn == 'int' and a.exact and not a.const:
                b = self.expr(depth - 1)
                if not b.exact or b.const:
                    b = self.leaf()
                    if b.const:
                        return None
                # (psp2cgc converts float to int by flooring, not truncating)
                ia = np.floor(np.clip(a.value, -1000, 1000)).astype(np.int64)
                ib = np.floor(np.clip(b.value, -1000, 1000)).astype(np.int64)
                # (no / and %: the vertex profile divides integers by constants only)
                op = rnd.choice(['+', '-', '*', '&', '|', '^', '<<', '>>', 'min', 'max'])
                if op in ('<<', '>>'):
                    ib = ib & 3
                    bt = '((int4)%s & 3)' % b.text
                else:
                    bt = '(int4)%s' % b.text
                at = '(int4)%s' % a.text
                if op in ('/', '%'):
                    ib = np.where(ib == 0, 1, ib)
                    bt = '(((int4)%s == 0) ? 1 : (int4)%s)' % (b.text, b.text)
                    q = np.trunc(ia / ib).astype(np.int64)
                    r = q if op == '/' else ia - q * ib
                elif op in ('min', 'max'):
                    r = np.minimum(ia, ib) if op == 'min' else np.maximum(ia, ib)
                    cmp = '<' if op == 'min' else '>'
                    return Node('(float4)((%s %s %s) ? %s : %s)' % (at, cmp, bt, at, bt), r.astype(np.float32), True,
                                0.0)
                else:
                    r = {'+': ia + ib, '-': ia - ib, '*': ia * ib, '&': ia & ib, '|': ia | ib, '^': ia ^ ib,
                         '<<': ia << ib, '>>': ia >> ib}[op]
                return Node('(float4)(%s %s %s)' % (at, op, bt), r.astype(np.float32), True, 0.0)
            if fn == 'sincos':
                x = np.clip(a.value, -8, 8)
                f = rnd.choice(['sin', 'cos'])
                v = np.sin(x) if f == 'sin' else np.cos(x)
                return Node('%s(clamp(%s, -8, 8))' % (f, a.text), v, False, a.err + 2e-3)
            if fn == 'pow':
                b = self.leaf()
                x, y = np.abs(a.value) + F(1), np.clip(b.value, -2, 2)
                v = np.power(x.astype(np.float64), y.astype(np.float64)).astype(np.float32)
                return Node('pow(abs(%s) + 1, clamp(%s, -2, 2))' % (a.text, b.text), v, False,
                            (a.err * 2 + 1e-5) * float(np.max(np.abs(v))) * 4)
        return None

    def nice(self):
        return F(self.rnd.randint(-32, 32) / 8.0)

    def literal(self):
        v = [self.nice() for _ in range(4)]
        if self.rnd.random() < 0.3:
            v = [v[0]] * 4
        return Node('float4(%s)' % ', '.join(repr(float(x)) for x in v), v, True, 0.0, const=True)

    def leaf(self):
        k = self.rnd.random()
        if k < 0.45:
            name = self.rnd.choice(list(self.attrs))
            val = self.attrs[name]
        elif k < 0.8:
            name = self.rnd.choice(list(self.uniforms))
            val = self.uniforms[name]
        else:
            return self.literal()
        node = Node(name, val, True, 0.0)
        if self.rnd.random() < 0.4:
            sw = ''.join(self.rnd.choice(SWIZ) for _ in range(4))
            node = Node('%s.%s' % (name, sw), [val[SWIZ.index(c)] for c in sw], True, 0.0)
        return node

    def expr(self, depth):
        if depth <= 0 or self.rnd.random() < 0.2:
            return self.leaf()
        r = self.rnd.random()
        a = self.expr(depth - 1)
        if SPECIAL and self.rnd.random() < 0.15 and a.kind not in ('floor', 'ceil'):
            s = self.special(depth, a)
            if s is not None:
                return s
        if r < 0.45:
            b = self.expr(depth - 1)
            op = self.rnd.choice(['+', '-', '*', 'min', 'max', '*'])
            with np.errstate(all='ignore'):
                if op in '+-*':
                    v = {'+': a.value + b.value, '-': a.value - b.value, '*': a.value * b.value}[op]
                    text = '(%s %s %s)' % (a.text, op, b.text)
                else:
                    v = np.minimum(a.value, b.value) if op == 'min' else np.maximum(a.value, b.value)
                    text = '%s(%s, %s)' % (op, a.text, b.text)
            mag = float(np.max(np.abs(np.concatenate([a.value, b.value, v]))))
            exact = a.exact and b.exact and mag < 2 ** 12
            err = a.err + b.err + (mag * 2e-7 if not exact else 0.0)
            if op == '*':
                err = a.err * float(np.max(np.abs(b.value))) + b.err * float(np.max(np.abs(a.value))) + err
            return Node(text, v, exact, err, a.const and b.const, 'mul' if op == '*' else '')
        if r < 0.55:
            return Node('(-%s)' % a.text, -a.value, a.exact, a.err, a.const, a.kind)
        # (psp2cgc folds abs(floor(x)) + c into a "frc" that drops the abs, and moves abs(x * y) onto one factor:
        # avoid abs right on floor, ceil and products)
        if r < 0.62 and a.kind not in ('floor', 'ceil', 'mul'):
            return Node('abs(%s)' % a.text, np.abs(a.value), a.exact, a.err, a.const)
        # (psp2cgc folds frac of constants as x - trunc(x), but compiles frac of variables as x - floor(x): apply
        # the steps to variables only)
        if r < 0.75 and a.exact and not a.const:
            fn = self.rnd.choice(['floor', 'frac', 'step', 'sel', 'saturate', 'ceil'])
            if fn == 'floor':
                return Node('floor(%s)' % a.text, np.floor(a.value), True, 0.0, kind='floor')
            if fn == 'ceil':
                return Node('ceil(%s)' % a.text, np.ceil(a.value), True, 0.0, kind='ceil')
            if fn == 'frac':
                return Node('frac(%s)' % a.text, a.value - np.floor(a.value), True, 0.0)
            if fn == 'saturate':
                return Node('saturate(%s)' % a.text, np.clip(a.value, 0, 1), True, 0.0)
            b = self.expr(depth - 1)
            if not b.exact:
                b = self.leaf()
            if fn == 'step':
                return Node('step(%s, %s)' % (a.text, b.text), (b.value >= a.value).astype(np.float32), True, 0.0)
            c, d = self.expr(depth - 1), self.expr(depth - 1)
            cmp = self.rnd.choice(['<', '>', '<=', '>=', '==', '!='])
            m = {'<': a.value < b.value, '>': a.value > b.value, '<=': a.value <= b.value,
                 '>=': a.value >= b.value, '==': a.value == b.value, '!=': a.value != b.value}[cmp]
            return Node('((%s %s %s) ? %s : %s)' % (a.text, cmp, b.text, c.text, d.text), np.where(m, c.value, d.value),
                        c.exact and d.exact, max(c.err, d.err))
        # approximate operations (most wrap their operand in abs(x) + 1: not on floor and ceil, see above)
        if a.kind in ('floor', 'ceil'):
            return a
        fn = self.rnd.choice(['rcp', 'rsqrt', 'sqrt', 'exp2', 'log2', 'dot', 'lerp', 'normalize', 'div'])
        with np.errstate(all='ignore'):
            if fn == 'rcp':
                v = F(1) / (np.abs(a.value) + F(1))
                return Node('(1 / (abs(%s) + 1))' % a.text, v, False, a.err + 1e-6 + 2e-6 * float(np.max(v)))
            if fn == 'rsqrt':
                v = F(1) / np.sqrt(np.abs(a.value) + F(1))
                return Node('rsqrt(abs(%s) + 1)' % a.text, v, False, a.err + 2e-6)
            if fn == 'sqrt':
                v = np.sqrt(np.abs(a.value))
                return Node('sqrt(abs(%s))' % a.text, v, False, np.sqrt(a.err) + 2e-6 * (1 + float(np.max(v))))
            if fn == 'exp2':
                x = np.clip(a.value, -8, 8)
                v = np.exp2(x)
                return Node('exp2(clamp(%s, -8, 8))' % a.text, v, False, (a.err + 1e-5) * float(np.max(v)) * 2)
            if fn == 'log2':
                v = np.log2(np.abs(a.value) + F(1))
                return Node('log2(abs(%s) + 1)' % a.text, v, False, a.err + 1e-5)
            if fn == 'div':
                b = self.expr(depth - 1)
                if b.kind in ('floor', 'ceil'):
                    b = self.leaf()
                den = np.abs(b.value) + F(1)
                v = a.value / den
                return Node('(%s / (abs(%s) + 1))' % (a.text, b.text), v, False,
                            a.err + b.err * float(np.max(np.abs(v))) + 2e-6 * (1 + float(np.max(np.abs(v)))))
            b = self.expr(depth - 1)
            if fn == 'dot':
                s = F(np.dot(a.value, b.value))
                mag = float(np.max(np.abs(a.value * b.value))) * 4
                return Node('dot(%s, %s).xxxx' % (a.text, b.text), [s] * 4, False,
                            a.err * float(np.sum(np.abs(b.value))) + b.err * float(np.sum(np.abs(a.value))) + mag * 1e-6)
            if fn == 'lerp':
                t = self.leaf()
                v = a.value + (b.value - a.value) * t.value
                mag = float(np.max(np.abs(np.concatenate([a.value, b.value, v]))))
                return Node('lerp(%s, %s, %s)' % (a.text, b.text, t.text), v, False,
                            (a.err + b.err) * (1 + float(np.max(np.abs(t.value)))) + mag * 4e-6)
            n = a.value + F(0.5)
            ln = np.sqrt(np.dot(n, n))
            if ln < 0.25:
                return a
            v = n / ln
            return Node('normalize(%s + 0.5)' % a.text, v, False, a.err / float(ln) * 2 + 4e-6)


def program(rnd):
    """A vertex program: its source, its inputs (pa and uniform words) and its expected outputs (o0-o15)."""
    attrs, uniforms = {}, {}
    for i in range(rnd.randint(1, 4)):
        attrs['a%d' % i] = f4([rnd.randint(-32, 32) / 8.0 for _ in range(4)])
    for i in range(rnd.randint(0, 4)):
        uniforms['u%d' % i] = f4([rnd.randint(-32, 32) / 8.0 for _ in range(4)])
    if not uniforms:
        uniforms['u0'] = f4([1, 2, 3, 4])
    g = Gen(rnd, attrs, uniforms)
    # matrices (float4x4, rows in order) and arrays of float4
    for i in range(rnd.choice([0, 0, 1, 2])):
        g.mats['m%d' % i] = np.array([[rnd.randint(-16, 16) / 8.0 for _ in range(4)] for _ in range(4)], np.float32)
    for i in range(rnd.choice([0, 0, 1, 2])):
        g.arrays['v%d' % i] = np.array([[rnd.randint(-32, 32) / 8.0 for _ in range(4)]
                                        for _ in range(rnd.randint(2, 9))], np.float32)
    outs = []
    body = []
    for j in range(rnd.randint(1, 4)):
        e = g.expr(rnd.randint(1, 5))
        kind = rnd.random()
        if kind < 0.15 and e.exact and 'branch' in FLOW:
            # a branch on an exact condition
            c = g.leaf()
            k = rnd.randint(0, 3)
            o = g.expr(2)
            taken = c.value[k] > F(0)
            body.append('if (%s.%s > 0) r%d = %s; else r%d = %s;' % (c.text, SWIZ[k], j, e.text, j, o.text))
            e = e if taken else o
            body[-1] = 'float4 r%d;\n    ' % j + body[-1]
        elif kind < 0.25 and e.exact and 'loop' in FLOW:
            # a loop with a constant count: r = r * 0.5 + e
            n = rnd.randint(2, 6)
            body.append('float4 r%d = 0;\n    for (int i%d = 0; i%d < %d; i%d++) r%d = r%d * 0.5 + %s;' % (
                j, j, j, n, j, j, j, e.text))
            v = f4([0, 0, 0, 0])
            for _ in range(n):
                v = v * F(0.5) + e.value
            e = Node('', v, False, e.err * 2 + 1e-6 * float(np.max(np.abs(v)) + 1))
        elif kind < 0.32 and e.exact and 'uloop' in FLOW:
            # a loop whose count is a uniform's: r = r * 0.5 + e, (int)min(abs(u.c), 5) times
            u = rnd.choice(list(uniforms))
            c = rnd.randint(0, 3)
            n = int(min(abs(float(uniforms[u][c])), 5.0))
            body.append('float4 r%d = 1;\n    for (int i%d = 0; i%d < (int)min(abs(%s.%s), 5); i%d++) '
                        'r%d = r%d * 0.5 + %s;' % (j, j, j, u, SWIZ[c], j, j, j, e.text))
            v = f4([1, 1, 1, 1])
            for _ in range(n):
                v = v * F(0.5) + e.value
            e = Node('', v, False, e.err * 2 + 1e-6 * float(np.max(np.abs(v)) + 1))
        else:
            body.append('float4 r%d = %s;' % (j, e.text))
        outs.append(e)
    # the matrices and arrays become uniforms (their values flattened in declaration order)
    for name, m in g.mats.items():
        uniforms[name] = ('float4x4', m.reshape(-1))
    for name, a in g.arrays.items():
        uniforms[name] = ('float4[%d]' % len(a), a.reshape(-1))
    return source(attrs, uniforms, body, len(outs)), attrs, uniforms, outs, body


def declaration(name, u):
    if isinstance(u, tuple):
        t = u[0]
        return 'uniform %s %s%s;' % (t.split('[')[0], name, '[' + t.split('[')[1] if '[' in t else '')
    return 'uniform float4 %s;' % name


def flat(u):
    return u[1] if isinstance(u, tuple) else u


def source(attrs, uniforms, body, outputs):
    names = ['POSITION'] + ['TEXCOORD%d' % i for i in range(outputs - 1)]
    params = ['float4 %s : ATTR%d' % (n, i) for i, n in enumerate(attrs)]
    params += ['out float4 o%d : %s' % (j, names[j]) for j in range(outputs)]
    src = '\n'.join(declaration(n, u) for n, u in uniforms.items())
    src += '\nvoid main(%s)\n{\n    %s\n    %s\n}\n' % (
        ',\n          '.join(params), '\n    '.join(body), ' '.join('o%d = r%d;' % (j, j) for j in range(outputs)))
    return src


def words(v):
    return ' '.join('%08x' % struct.unpack('<I', struct.pack('<f', float(x)))[0] for x in v)


def from_word(w):
    return struct.unpack('<f', struct.pack('<I', int(w, 16)))[0]


def check(job):
    """None if the program agrees; else (kind, index, message). A program that disagrees only while it has
    uniforms (compiled again with them as attributes, it agrees) is reported as 'uniforms': the compiler moves
    uniform-only work into the secondary program, and some of its folds there are wrong (see KNOWN_ISSUES)."""
    i, src, attrs, uniforms, outs, body, cgc, psvusse, folder = job
    r = run_one(os.path.join(folder, 'e%05d' % i), src, attrs, uniforms, outs, cgc, psvusse, i)
    plain = not any(isinstance(u, tuple) for u in uniforms.values())
    if r and r[0] == 'value' and uniforms and plain:
        merged = dict(attrs)
        merged.update(uniforms)
        if not run_one(os.path.join(folder, 'e%05d_attr' % i), source(merged, {}, body, len(outs)), merged, {}, outs,
                       cgc, psvusse, i):
            return ('uniforms', i, r[2])
    return r


def run_one(base, src, attrs, uniforms, outs, cgc, psvusse, i):
    open(base + '.cg', 'w').write(src)
    r = subprocess.run([cgc, '-profile', 'sce_vp_psp2', '-o', base + '.gxp', base + '.cg'], capture_output=True,
                       text=True)
    if r.returncode:
        return ('compile', i, (r.stderr or r.stdout)[:400])
    # the attributes and uniforms in the order the compiler placed them (psvusse --params)
    p = subprocess.run([psvusse, '--params', base + '.gxp'], capture_output=True, text=True)
    lines = ['# %s' % os.path.basename(base)]
    for line in p.stdout.splitlines():
        if not line.startswith('param '):
            continue
        f = dict(kv.split('=', 1) for kv in line.split()[2:] if '=' in kv)
        name, res = f['name'], int(f['resource'])
        if f['category'] == '0' and name in attrs:
            lines.append('pa %d %s' % (res, words(attrs[name])))
        elif f['category'] == '1' and name in uniforms:
            lines.append('uniform %d %s' % (res, words(flat(uniforms[name]))))
    open(base + '.in', 'w').write('\n'.join(lines) + '\n')
    r = subprocess.run([psvusse, '--run', base + '.in', base + '.gxp'], capture_output=True, text=True)
    if r.returncode or not r.stdout.startswith('out'):
        return ('run', i, (r.stderr or r.stdout)[:400])
    got = [from_word(w) for w in r.stdout.split()[1:65]]
    for j, e in enumerate(outs):
        for k in range(4):
            want, have = float(e.value[k]), got[4 * j + k]
            tol = e.err * 4 + 1e-6 * (1 + abs(want))
            if not (abs(want - have) <= tol or (np.isnan(want) and np.isnan(have))):
                return ('value', i, 'o%d.%s: want %r, got %r (tolerance %.3g)' % (j, SWIZ[k], want, have, tol))
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--psvusse', required=True)
    ap.add_argument('--sdk', required=True)
    ap.add_argument('--count', type=int, default=500)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--keep', help='folder for the programs (kept); default: a temporary one')
    ap.add_argument('--show', type=int, default=10)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--special', help='comma-separated: the special operations to generate (%s; "none")' %
                    ', '.join(sorted(set(SPECIAL))))
    ap.add_argument('--flow', help='comma-separated: the flow control to generate (%s; "none")' % ', '.join(FLOW))
    args = ap.parse_args()
    if args.special:
        SPECIAL[:] = [] if args.special == 'none' else args.special.split(',')
    if args.flow:
        FLOW[:] = [] if args.flow == 'none' else args.flow.split(',')
    cgc = os.path.join(args.sdk, 'host_tools', 'bin', 'psp2cgc.exe')
    psvusse = os.path.abspath(args.psvusse)
    folder = args.keep or tempfile.mkdtemp(prefix='usse_exec_')
    os.makedirs(folder, exist_ok=True)
    rnd = random.Random(args.seed)
    jobs = [(i,) + program(rnd) + (cgc, psvusse, folder) for i in range(args.count)]
    with ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(check, jobs))
    bad = [r for r in results if r]
    by = {k: [r for r in bad if r[0] == k] for k in ('compile', 'uniforms')}
    wrong = [r for r in bad if r[0] not in by]
    for r in wrong[:args.show]:
        print('%s e%05d: %s' % r)
    for r in by['uniforms'][:args.show]:
        print('uniforms e%05d: %s (agrees with the uniforms as attributes)' % (r[1], r[2]))
    for r in by['compile'][:3]:
        print('compile e%05d: %s' % (r[1], r[2]))
    ran = args.count - len(by['compile'])
    print('%d of %d programs agree; %d disagree only with uniforms; %d did not compile' % (
        ran - len(wrong) - len(by['uniforms']), ran, len(by['uniforms']), len(by['compile'])))
    if not args.keep:
        shutil.rmtree(folder, ignore_errors=True)
    return 0 if not wrong else 1


if __name__ == '__main__':
    sys.exit(main())
