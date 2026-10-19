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
FRAGMENT = [False]  # (--fragment)
TEXTURE_PROGRAMS = [0.0]  # (--textures: the share of fragment programs that sample textures)
MAG = [0.0]  # the largest magnitude of the expression being generated (fragment programs compute in f16)


def f4(v):
    return np.array(v, dtype=np.float32)


class Node:
    """A float4 expression: its Cg text, its value, whether it is exact, its error bound, whether it depends on
    literals only (the compiler folds those itself), and what made it (floor, ceil: see Gen.expr)."""

    def __init__(self, text, value, exact, err, const=False, kind=''):
        self.text, self.value, self.exact, self.err = text, f4(value), exact, err
        self.const, self.kind = const, kind
        with np.errstate(all='ignore'):
            m = float(np.max(np.abs(self.value)))
        if np.isfinite(m):
            MAG[0] = max(MAG[0], m)
        if FRAGMENT[0] and exact and not f16_exact(self.value):
            self.exact = False


def f16_exact(v):
    with np.errstate(all='ignore'):
        return bool(np.all(np.abs(v) < 1024) and np.all(v.astype(np.float16).astype(np.float32) == v))


class Gen:
    def __init__(self, rnd, attrs, uniforms):
        self.rnd = rnd
        self.attrs = attrs        # name -> value
        self.uniforms = uniforms  # name -> value
        self.texts = {}           # name -> the float4 expression reading it (fragment inputs of other types)
        self.skip = set()         # special operations not to generate
        self.tex_leaf = None      # a texture sample (fragment programs with textures)
        self.mats = {}            # name -> 4x4 (rows)
        self.arrays = {}          # name -> n x 4

    def special(self, depth, a):
        """Matrices, indexed arrays, half precision, integers, trigonometry, powers (or None)."""
        rnd = self.rnd
        fn = rnd.choice([f for f in SPECIAL if f not in self.skip] or ['none'])
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
        if self.tex_leaf and self.rnd.random() < 0.3:
            return self.tex_leaf()
        k = self.rnd.random()
        if k < 0.45:
            name = self.rnd.choice(list(self.attrs))
            val = self.attrs[name]
        elif k < 0.8:
            name = self.rnd.choice(list(self.uniforms))
            val = self.uniforms[name]
        else:
            return self.literal()
        text = self.texts.get(name, name)
        node = Node(text, val, True, 0.0)
        if self.rnd.random() < 0.4:
            sw = ''.join(self.rnd.choice(SWIZ) for _ in range(4))
            node = Node('(%s).%s' % (text, sw) if text != name else '%s.%s' % (name, sw),
                        [val[SWIZ.index(c)] for c in sw], True, 0.0)
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
    return source(attrs, uniforms, body, len(outs)), attrs, uniforms, outs, body, {}


VARYING_TYPES = {'float4': (4, 'f32'), 'float2': (2, 'f32'), 'float': (1, 'f32'), 'half4': (4, 'f16'),
                 'half2': (2, 'f16')}
VARYING_TYPES_FIXED = {'fixed4': (4, 'fx10')}
SEMANTIC_BITS = dict([('TEXCOORD%d' % k, 16 << k) for k in range(10)] + [('COLOR0', 4), ('COLOR1', 8)])


def texels(tex):
    """RGBA8 words, row by row."""
    t = tex.astype(np.uint32)
    return (t[..., 0] | t[..., 1] << 8 | t[..., 2] << 16 | t[..., 3] << 24).reshape(-1)


def sample(tex, u, v):
    """The nearest texel (wrapping), as psvusse samples, in [0, 1]."""
    h, w = tex.shape[:2]
    return tex[int(np.floor(v * h)) % h, int(np.floor(u * w)) % w].astype(np.float32) / F(255)


def fragment_program(rnd):
    """A fragment program: its source, its varyings ({name: (type, semantic, value)}), uniforms and the expected
    colour (one output, f16x4 in the primary attributes)."""
    varyings, values, uniforms = {}, {}, {}
    semantics = rnd.sample(['TEXCOORD%d' % k for k in range(8)] + ['COLOR0', 'COLOR1'], rnd.randint(1, 5))
    texts = {}
    # textures (4x4 RGBA8) and coordinates: float4 texture coordinates at texel centres ((2m + 1) / 8) with w = 1,
    # so that nearest sampling never falls on a texel boundary
    textures = {}
    if TEXTURE_PROGRAMS[0] and rnd.random() < TEXTURE_PROGRAMS[0]:
        for unit in range(rnd.randint(1, 2)):
            textures[unit] = np.array([[[rnd.randint(0, 255) for _ in range(4)] for _ in range(4)] for _ in range(4)])
        semantics = ['TEXCOORD0'] + [x for x in semantics if x != 'TEXCOORD0']
    for i, sem in enumerate(semantics):
        t = rnd.choice(list(VARYING_TYPES))
        if textures and i == 0:
            t = 'float4'
        n, _ = VARYING_TYPES[t]
        lo, hi = (0, 8) if sem.startswith('COLOR') else (-32, 32)
        v = f4([rnd.randint(lo, hi) / 8.0 for _ in range(n)])
        if textures and i == 0:
            v = f4([(2 * rnd.randint(-8, 7) + 1) / 8.0, (2 * rnd.randint(-8, 7) + 1) / 8.0, rnd.randint(-8, 8) / 8.0,
                    1.0])
        name = 'i%d' % i
        varyings[name] = (t, sem, v)
        rep = {1: 'xxxx', 2: 'xyxy', 4: 'xyzw'}[n]
        values[name] = f4([v['xyzw'.index(c)] for c in rep])
        texts[name] = ('(float4)%s' % name if t.startswith('half') else name) + ('.%s' % rep if n < 4 else '')
    for i in range(rnd.randint(0, 3)):
        uniforms['u%d' % i] = f4([rnd.randint(-32, 32) / 8.0 for _ in range(4)])
    if not uniforms:
        uniforms['u0'] = f4([1, 2, 3, 4])
    g = Gen(rnd, values, uniforms)
    g.texts = texts
    if textures:
        coord = varyings['i0'][2]
        names = {unit: 's%d' % unit for unit in textures}

        def tex_leaf():
            unit = rnd.choice(list(textures))
            # (u, v): the coordinate, or a transform of it that keeps it off texel boundaries
            form = rnd.choice(['xy', 'yx', 'xy3', 'xyq', 'neg', 'proj'])
            u, v = float(coord[0]), float(coord[1])
            text = {'xy': 'i0.xy', 'yx': 'i0.yx', 'xy3': 'i0.xy * 3', 'xyq': 'i0.xy + 0.25', 'neg': '-i0.xy',
                    'proj': 'i0.xyw'}[form]
            u, v = {'xy': (u, v), 'yx': (v, u), 'xy3': (u * 3, v * 3), 'xyq': (u + 0.25, v + 0.25), 'neg': (-u, -v),
                    'proj': (u, v)}[form]
            value = sample(textures[unit], u, v)
            kind = rnd.choice(['half', 'half', 'float', 'u8'])
            if form == 'proj':
                call = 'tex2Dproj(%s, %s)' % (names[unit], text)
            elif kind == 'float':
                call = 'tex2D<float4>(%s, %s)' % (names[unit], text)
            elif kind == 'u8':
                call = 'float4(tex2D<unsigned char4>(u%s, %s)) / 255' % (names[unit], text)
            else:
                call = 'tex2D(%s, %s)' % (names[unit], text)
            return Node(call, value, False, 1e-3)

        g.tex_leaf = tex_leaf
    # (psp2cgc converts only one lane of a half vector cast to int, then reads it for every lane: no integer
    # operations in programs with half inputs)
    if any(t.startswith('half') for t, _, _ in varyings.values()):
        g.skip.add('int')
    MAG[0] = 0.0
    e = g.expr(rnd.randint(1, 5))
    e.mag = MAG[0]
    params = ['%s %s : %s' % (t, name, sem) for name, (t, sem, _) in varyings.items()]
    src = '\n'.join('uniform float4 %s;' % u for u in uniforms)
    for unit in textures:  # (a texture's unsigned sampler: the same texture on unit + 4)
        src += '\nuniform sampler2D s%d : TEXUNIT%d;\nuniform usampler2D us%d : TEXUNIT%d;' % (unit, unit, unit, unit + 4)
    src += '\nfloat4 main(%s) : COLOR\n{\n    return %s;\n}\n' % (',\n           '.join(params), e.text)
    return src, varyings, uniforms, [e], [], textures


def fixed_program(rnd):
    """A fragment program computing in fixed point: fixed4 varyings (fx10 in the primary attributes), fixed
    expressions (sums of products, lerps, clamps), the colour returned as f16x4. Values stay inside fx10's range
    (about [-2, 2]); each operation may round by 1/255."""
    varyings = {}
    semantics = rnd.sample(['COLOR0', 'COLOR1', 'TEXCOORD0', 'TEXCOORD1', 'TEXCOORD2'], rnd.randint(1, 3))
    for i, sem in enumerate(semantics):
        varyings['c%d' % i] = ('fixed4', sem, f4([rnd.randint(0, 255) / 255.0 for _ in range(4)]))

    def leaf():
        k = rnd.random()
        if k < 0.75:
            name = rnd.choice(list(varyings))
            v = varyings[name][2]
            if rnd.random() < 0.3:
                c = rnd.choice(SWIZ)
                return '%s.%s' % (name, c * 4), f4([v[SWIZ.index(c)]] * 4), 0.0, True
            return name, v, 0.0, False
        q = rnd.choice([0.25, 0.5, 0.75, 1.0, 2.0])
        return 'fixed4(%r, %r, %r, %r)' % (q, q, q, q), f4([q] * 4), 0.0, False

    def expr(depth):
        if depth <= 0 or rnd.random() < 0.25:
            return leaf()
        a = expr(depth - 1)
        op = rnd.choice(['*', '+', '-', 'lerp', 'sat', 'min', 'max', '1-', 'alpha'])
        # (psp2cgc compiles x - y.wwww, a scalar broadcast subtracted, as a negation followed by a subtraction,
        # which the hardware cannot both mean: avoid subtracting broadcasts)
        if op == '1-' and a[3]:
            op = 'sat'
        if op == '1-':
            r = ('(1 - %s)' % a[0], F(1) - a[1], a[2] + 1 / 255, False)
        elif op == 'sat':
            r = ('saturate(%s)' % a[0], np.clip(a[1], 0, 1), a[2], a[3])
        elif op == 'alpha':
            r = ('%s.wwww' % ('(%s)' % a[0]), f4([a[1][3]] * 4), a[2], True)
        else:
            b = expr(depth - 1)
            if op == '-' and b[3]:
                op = '+'
            if op == 'lerp':
                # (weights in [0, 1]: psp2cgc folds lerp(x, constant, t) with a constant t > 1 with the wrong sign)
                c = expr(depth - 1)
                if float(np.max(np.abs(c[1]))) > 1:
                    c = ('saturate(%s)' % c[0], np.clip(c[1], 0, 1), c[2], c[3])
                v = a[1] + (b[1] - a[1]) * c[1][3]
                r = ('lerp(%s, %s, (%s).w)' % (a[0], b[0], c[0]), v,
                     (a[2] + b[2]) * 2 + c[2] * float(np.max(np.abs(b[1] - a[1]))) + 3 / 255, False)
            elif op in ('min', 'max'):
                r = ('%s(%s, %s)' % (op, a[0], b[0]), np.minimum(a[1], b[1]) if op == 'min' else np.maximum(a[1], b[1]),
                     max(a[2], b[2]), a[3] and b[3])
            else:
                v = {'*': a[1] * b[1], '+': a[1] + b[1], '-': a[1] - b[1]}[op]
                err = a[2] * float(np.max(np.abs(b[1]))) + b[2] * float(np.max(np.abs(a[1]))) if op == '*' else a[2] + b[2]
                r = ('(%s %s %s)' % (a[0], op, b[0]), v, err + 1 / 255, a[3] and b[3])
        if float(np.max(np.abs(r[1]))) > 1.9:  # (keep inside fx10's range: saturate instead)
            r = ('saturate(%s)' % r[0], np.clip(r[1], 0, 1), r[2], r[3])
        return r

    text, value, err, _ = expr(rnd.randint(1, 5))
    e = Node(text, value, False, err + 2 / 255)
    e.mag = float(np.max(np.abs(value)))
    params = ['fixed4 %s : %s' % (name, sem) for name, (_, sem, _) in varyings.items()]
    src = 'fixed4 main(%s) : COLOR\n{\n    return %s;\n}\n' % (', '.join(params), text)
    return src, varyings, {}, [e], [], {}


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


def varying_words(v, fmt, n):
    """The primary-attribute words of an interpolated value in the record's format (0 f32, 2 f16)."""
    if fmt == 0:
        return words(list(v[:n]) + [0.0] * (n - len(v)))
    if fmt == 1:  # fx10: x, y, z in bits 0-9, 10-19, 20-29, w in the next register (1.0 = 255)
        q = [int(round(float(x) * 255)) & 0x3FF for x in list(v[:n]) + [0.0] * (4 - min(n, 4))]
        return '%08x %08x' % (q[0] | q[1] << 10 | q[2] << 20, q[3])
    h = np.array(list(v[:n]) + [0.0] * (n - len(v)) + [0.0], np.float32).astype(np.float16).view(np.uint16)
    return ' '.join('%08x' % (int(h[2 * k]) | int(h[2 * k + 1]) << 16) for k in range((n + 1) // 2))


def words(v):
    return ' '.join('%08x' % struct.unpack('<I', struct.pack('<f', float(x)))[0] for x in v)


def from_word(w):
    return struct.unpack('<f', struct.pack('<I', int(w, 16)))[0]


def negated_subtrahend(listing):
    """Whether a fixed-point program negates a value (sub x, 0, y) and later subtracts that value (or a copy of
    it): psp2cgc compiles subtractions of scalars that way, which only works if the first sub does not negate,
    while it compiles negations with the same instruction (see KNOWN_ISSUES)."""
    import re

    def register(text):
        m = re.match(r'-?[(]?([a-z]+[0-9]+)', text)
        return m.group(1) if m else None

    negated = set()
    for line in listing.splitlines():
        m = re.search(r'(sub|mov)[.]fx(?:10|8) +([a-z]+[0-9]+)[-.xyzw]*, (.*)$', line)
        if not m:
            continue
        op, dest, rest = m.group(1), m.group(2), [x.strip() for x in m.group(3).split(',')]
        regs = [register(x) for x in rest if register(x)]
        if op == 'sub' and len(rest) >= 2 and register(rest[1]) in negated:
            return True
        if op == 'sub' and rest and rest[0] == '0':
            negated.add(dest)
        elif op == 'mov' and regs and regs[0] in negated:
            negated.add(dest)
        else:
            negated.discard(dest)
    return False


def check(job):
    """None if the program agrees; else (kind, index, message). A program that disagrees only while it has
    uniforms (compiled again with them as attributes, it agrees) is reported as 'uniforms': the compiler moves
    uniform-only work into the secondary program, and some of its folds there are wrong (see KNOWN_ISSUES)."""
    i, src, attrs, uniforms, outs, body, textures, cgc, psvusse, folder = job
    r = run_one(os.path.join(folder, 'e%05d' % i), src, attrs, uniforms, outs, cgc, psvusse, i, textures)
    if r and r[0] == 'value' and FRAGMENT[0]:
        listing = subprocess.run([psvusse, os.path.join(folder, 'e%05d.gxp' % i)], capture_output=True, text=True).stdout
        if negated_subtrahend(listing):
            return ('compiler', i, r[2])
    plain = not any(isinstance(u, tuple) for u in uniforms.values())
    if r and r[0] == 'value' and uniforms and plain and not FRAGMENT[0]:
        merged = dict(attrs)
        merged.update(uniforms)
        if not run_one(os.path.join(folder, 'e%05d_attr' % i), source(merged, {}, body, len(outs)), merged, {}, outs,
                       cgc, psvusse, i):
            return ('uniforms', i, r[2])
    return r


def run_one(base, src, attrs, uniforms, outs, cgc, psvusse, i, textures=None):
    open(base + '.cg', 'w').write(src)
    profile = 'sce_fp_psp2' if FRAGMENT[0] else 'sce_vp_psp2'
    r = subprocess.run([cgc, '-profile', profile, '-o', base + '.gxp', base + '.cg'], capture_output=True, text=True)
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
    if FRAGMENT[0]:
        # the varyings by semantic (psvusse places them in the primary attributes) and the textures by unit
        for name, (t, sem, v) in attrs.items():
            lines.append('varying %d %s' % (SEMANTIC_BITS[sem], words(list(v) + [0.0] * (4 - len(v)))))
        for unit, tex in [(u + d, t) for u, t in (textures or {}).items() for d in (0, 4)]:
            lines.append('texture %d %x %x %s' % (unit, tex.shape[1], tex.shape[0],
                                                   ' '.join('%08x' % int(t) for t in texels(tex))))
    open(base + '.in', 'w').write('\n'.join(lines) + '\n')
    r = subprocess.run([psvusse, '--run', base + '.in', base + '.gxp'], capture_output=True, text=True)
    if r.returncode or not r.stdout.startswith('out'):
        return ('run', i, (r.stderr or r.stdout)[:400])
    got = [from_word(w) for w in r.stdout.split()[1:65]]
    if FRAGMENT[0]:  # (the colour: f16x4 in pa0-pa1)
        hw = [int(w, 16) for w in r.stdout.split()[1:3]]
        got = [float(np.array([(w >> (16 * h)) & 0xFFFF], np.uint16).view(np.float16)[0]) for w in hw for h in (0, 1)]
    for j, e in enumerate(outs):
        for k in range(4):
            want, have = float(e.value[k]), got[4 * j + k]
            tol = e.err * 4 + 1e-6 * (1 + abs(want))
            if FRAGMENT[0]:  # (computed in f16, at worst: a few rounding steps of the largest magnitude)
                tol = e.err * 4 + 4e-3 * (1 + getattr(e, 'mag', abs(want)))
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
    ap.add_argument('--fragment', action='store_true', help='generate fragment programs instead')
    ap.add_argument('--fixed', action='store_true', help='generate fixed-point fragment programs instead')
    ap.add_argument('--textures', type=float, default=0.5, help='fragment programs: the share that sample textures')
    ap.add_argument('--flow', help='comma-separated: the flow control to generate (%s; "none")' % ', '.join(FLOW))
    args = ap.parse_args()
    if args.special:
        SPECIAL[:] = [] if args.special == 'none' else args.special.split(',')
    if args.flow:
        FLOW[:] = [] if args.flow == 'none' else args.flow.split(',')
    FRAGMENT[0] = args.fragment or args.fixed
    TEXTURE_PROGRAMS[0] = args.textures
    cgc = os.path.join(args.sdk, 'host_tools', 'bin', 'psp2cgc.exe')
    psvusse = os.path.abspath(args.psvusse)
    folder = args.keep or tempfile.mkdtemp(prefix='usse_exec_')
    os.makedirs(folder, exist_ok=True)
    rnd = random.Random(args.seed)
    make = fixed_program if args.fixed else fragment_program if args.fragment else program
    jobs = [(i,) + make(rnd) + (cgc, psvusse, folder) for i in range(args.count)]
    with ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(check, jobs))
    bad = [r for r in results if r]
    by = {k: [r for r in bad if r[0] == k] for k in ('compile', 'uniforms', 'compiler')}
    wrong = [r for r in bad if r[0] not in by]
    for r in wrong[:args.show]:
        print('%s e%05d: %s' % r)
    for r in by['uniforms'][:args.show]:
        print('uniforms e%05d: %s (agrees with the uniforms as attributes)' % (r[1], r[2]))
    for r in by['compiler'][:3]:
        print('compiler e%05d: %s (it subtracts a value it negated)' % (r[1], r[2]))
    for r in by['compile'][:3]:
        print('compile e%05d: %s' % (r[1], r[2]))
    ran = args.count - len(by['compile'])
    print('%d of %d programs agree; %d disagree only with uniforms; %d subtract a negated value; %d did not compile'
          % (ran - len(wrong) - len(by['uniforms']) - len(by['compiler']), ran, len(by['uniforms']), len(by['compiler']),
             len(by['compile'])))
    if not args.keep:
        shutil.rmtree(folder, ignore_errors=True)
    return 0 if not wrong else 1


if __name__ == '__main__':
    sys.exit(main())
