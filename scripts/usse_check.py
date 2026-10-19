#!/usr/bin/env python3
"""Differential check of psvrecore's USSE disassembler against the reference one (the SDK's psp2shaderperf).

Disassembles GXP files (compiled Vita shaders) with `psp2shaderperf -disasm` and with psvusse, and compares their
listings program by program: line numbers and text, whitespace-insensitive.

    usse_check.py --psvusse build/windows-x64-release/tools/psvusse/psvusse.exe --sdk <SDK root>
                  shaders/ more.gxp [--show 20]

Local only: it needs an SDK (not part of this repository) and the GXP files are whatever the caller has; nothing is
written anywhere.
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

LINE = re.compile(r'^\s*#?(\d+)\s*:\s*(.*?)\s*$')


def norm(text):
    return re.sub(r'\s+', ' ', text.strip())


def sections(text):
    """{'Primary program': [(number, text)], 'Secondary program': [...]} from a listing."""
    out, cur = {}, None
    for line in text.splitlines():
        if line.startswith('Primary program') or line.startswith('Secondary program'):
            cur = line.strip().rstrip(':')
            out[cur] = []
            continue
        if cur is None:
            continue
        m = LINE.match(line)
        if m:
            out[cur].append((int(m.group(1)), norm(m.group(2))))
        elif not line.strip():
            cur = None if out.get(cur) else cur
    return out


def reference(shaderperf, path):
    r = subprocess.run([shaderperf, '-disasm', path], capture_output=True, text=True, errors='replace')
    return sections(r.stdout)


def ours(psvusse, paths):
    """psvusse on many files at once: its output has a '== file' line before each."""
    r = subprocess.run([psvusse] + paths, capture_output=True, text=True, errors='replace')
    out, cur, buf = {}, None, []
    lines = r.stdout.splitlines()
    if len(paths) == 1:
        return {paths[0]: sections(r.stdout)}
    for line in lines + ['== ']:
        if line.startswith('== '):
            if cur is not None:
                out[cur] = sections('\n'.join(buf))
            cur, buf = line[3:], []
        else:
            buf.append(line)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', help='GXP files or folders of them')
    ap.add_argument('--psvusse', required=True)
    ap.add_argument('--sdk', help='SDK root (host_tools/bin/psp2shaderperf.exe)')
    ap.add_argument('--shaderperf', help='the reference disassembler (instead of --sdk)')
    ap.add_argument('--show', type=int, default=20, help='differences to print')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    args.psvusse = os.path.abspath(args.psvusse)
    shaderperf = args.shaderperf or (args.sdk and os.path.join(args.sdk, 'host_tools', 'bin', 'psp2shaderperf.exe'))
    if not shaderperf or not os.path.exists(shaderperf):
        sys.exit('psp2shaderperf not found (give --sdk or --shaderperf)')
    files = []
    for p in args.inputs:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, n) for n in sorted(names) if n.lower().endswith('.gxp')]
        else:
            files.append(p)
    if not files:
        sys.exit('no GXP files')
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        refs = dict(zip(files, ex.map(lambda f: reference(shaderperf, f), files)))
        batches = [files[i:i + 64] for i in range(0, len(files), 64)]
        got = {}
        for part in ex.map(lambda b: ours(args.psvusse, b), batches):
            got.update(part)
    programs = same = 0
    shown = 0
    for f in files:
        ref, mine = refs[f], got.get(f, {})
        for name in ('Secondary program', 'Primary program'):
            if name not in ref:
                continue
            programs += 1
            a, b = ref[name], mine.get(name, [])
            if a == b:
                same += 1
                continue
            if shown < args.show:
                shown += 1
                j = next((k for k in range(max(len(a), len(b))) if (a[k] if k < len(a) else None) != (b[k] if k < len(b) else None)), 0)
                print('%s (%s) line %d\n   reference: %s\n   psvusse:   %s' % (
                    f, name, j, a[j] if j < len(a) else None, b[j] if j < len(b) else None))
    print('%d of %d programs identical (%d files)' % (same, programs, len(files)))
    return 0 if same == programs else 1


if __name__ == '__main__':
    sys.exit(main())
