#!/usr/bin/env python3
"""Rendering tests: each folder of tests/render draws a scene (scene.txt) with Cg programs (v*.cg vertex, f*.cg
fragment) compiled with the SDK's shader compiler, through the reference renderer (the USSE reference interpreter,
rasterized on the CPU) and through the Vulkan backend, and compares the two images pixel by pixel (tools/psvrender).

    render_check.py --sdk <SDK root> [--preset windows-x64-release] [--gpu-index N] [--no-validate] [tests...]

The compiled programs and the images stay in build/render/<test> (the SDK's compiler made them: they are not
committed). Exit status: 0 when every test matches.
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sdk', required=True, help='the SDK root (host_tools/bin/psp2cgc)')
    ap.add_argument('--preset', default='windows-x64-release')
    ap.add_argument('--gpu-index', type=int, default=None)
    ap.add_argument('--no-validate', action='store_true', help='without the Vulkan validation layer')
    ap.add_argument('tests', nargs='*', help='test folders under tests/render (default: all)')
    args = ap.parse_args()

    exe = '.exe' if os.name == 'nt' else ''
    cgc = os.path.join(args.sdk, 'host_tools', 'bin', 'psp2cgc' + exe)
    tool = os.path.join(ROOT, 'build', args.preset, 'tools', 'psvrender', 'psvrender' + exe)
    for path, what in ((cgc, 'the SDK shader compiler'), (tool, 'psvrender (build the tools)')):
        if not os.path.exists(path):
            sys.exit('%s not found: %s' % (what, path))
    src = os.path.join(ROOT, 'tests', 'render')
    tests = args.tests or sorted(n for n in os.listdir(src) if os.path.isdir(os.path.join(src, n)))

    failed = []
    for name in tests:
        tdir = os.path.join(src, name)
        out = os.path.join(ROOT, 'build', 'render', name)
        os.makedirs(out, exist_ok=True)
        ok = True
        for cg in sorted(f for f in os.listdir(tdir) if f.endswith('.cg')):
            profile = 'sce_vp_psp2' if cg.startswith('v') else 'sce_fp_psp2'
            gxp = os.path.join(out, cg[:-3] + '.gxp')
            r = subprocess.run([cgc, '-profile', profile, '-o', gxp, os.path.join(tdir, cg)], capture_output=True, text=True)
            if r.returncode != 0:
                print('%s: %s did not compile:\n%s%s' % (name, cg, r.stdout, r.stderr))
                ok = False
        if not ok:
            failed.append(name)
            continue
        shutil.copy(os.path.join(tdir, 'scene.txt'), os.path.join(out, 'scene.txt'))
        cmd = [tool, os.path.join(out, 'scene.txt'), '--out', out]
        if not args.no_validate:
            cmd.append('--validate')
        if args.gpu_index is not None:
            cmd += ['--gpu-index', str(args.gpu_index)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        lines = [l for l in (r.stdout + r.stderr).splitlines() if ' INFO ' not in l]
        status = 'ok' if r.returncode == 0 else 'FAILED'
        print('%-16s %s  %s' % (name, status, ' | '.join(lines)))
        if r.returncode != 0:
            failed.append(name)

    print('%d of %d tests match' % (len(tests) - len(failed), len(tests)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
