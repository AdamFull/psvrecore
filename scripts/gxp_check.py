#!/usr/bin/env python3
"""Differential check of psvrecore's GXP reader against libgxm's program functions.

The SDK has a host build of libgxm's program functions (host_tools/lib.x64/gxm_tool.dll: sceGxmProgramGetType,
sceGxmProgramGetParameter, sceGxmProgramParameterGetCategory, ...). This calls them on GXP files and compares what
they report with `psvusse --params`, field by field:

    gxp_check.py --psvusse build/windows-x64-release/tools/psvusse/psvusse.exe --sdk <SDK root>
                 shaders/ more.gxp [--show 20]

Windows only (the DLL), with a 64-bit Python. Local only: it needs an SDK (not part of this repository), and the GXP
files are whatever the caller has; nothing is written anywhere.
"""

import argparse
import ctypes
import os
import subprocess
import sys


class Gxm:
    def __init__(self, sdk):
        dll = ctypes.CDLL(os.path.join(sdk, 'host_tools', 'lib.x64', 'gxm_tool.dll'))
        P, U, I, B = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int32, ctypes.c_ubyte

        def fn(name, res, *args):
            f = getattr(dll, name)
            f.restype, f.argtypes = res, list(args)
            return f

        self.check = fn('sceGxmProgramCheck', I, P)
        self.program = {
            'type': fn('sceGxmProgramGetType', U, P),
            'size': fn('sceGxmProgramGetSize', U, P),
            'discard': fn('sceGxmProgramIsDiscardUsed', B, P),
            'depth_replace': fn('sceGxmProgramIsDepthReplaceUsed', B, P),
            'sprite_coord': fn('sceGxmProgramIsSpriteCoordUsed', B, P),
            'native_color': fn('sceGxmProgramIsNativeColorUsed', B, P),
            'frag_color': fn('sceGxmProgramIsFragColorUsed', B, P),
            'default_uniform_buffer': fn('sceGxmProgramGetDefaultUniformBufferSize', U, P),
            'vertex_outputs': fn('sceGxmProgramGetVertexProgramOutputs', U, P),
            'fragment_inputs': fn('sceGxmProgramGetFragmentProgramInputs', U, P),
        }
        self.output = fn('sceGxmProgramGetOutputRegisterFormat', I, P, ctypes.POINTER(U), ctypes.POINTER(U))
        self.count = fn('sceGxmProgramGetParameterCount', U, P)
        self.param = fn('sceGxmProgramGetParameter', P, P, U)
        self.name = fn('sceGxmProgramParameterGetName', ctypes.c_char_p, P)
        self.fields = [
            ('category', fn('sceGxmProgramParameterGetCategory', U, P)),
            ('semantic', fn('sceGxmProgramParameterGetSemantic', U, P)),
            ('semantic_index', fn('sceGxmProgramParameterGetSemanticIndex', U, P)),
            ('type', fn('sceGxmProgramParameterGetType', U, P)),
            ('components', fn('sceGxmProgramParameterGetComponentCount', U, P)),
            ('array', fn('sceGxmProgramParameterGetArraySize', U, P)),
            ('resource', fn('sceGxmProgramParameterGetResourceIndex', U, P)),
            ('container', fn('sceGxmProgramParameterGetContainerIndex', U, P)),
            ('cube', fn('sceGxmProgramParameterIsSamplerCube', B, P)),
        ]
        self.reg_format = fn('sceGxmProgramParameterIsRegFormat', B, P, P)

    def report(self, data):
        """The lines psvusse --params prints, from libgxm's answers."""
        buf = ctypes.create_string_buffer(data, len(data))
        p = ctypes.addressof(buf)
        if self.check(p) != 0:
            return None
        v = {k: f(p) for k, f in self.program.items()}
        line = 'program ' + ' '.join('%s=%d' % (k, v[k]) for k in (
            'type', 'size', 'discard', 'depth_replace', 'sprite_coord', 'native_color', 'frag_color',
            'default_uniform_buffer', 'vertex_outputs', 'fragment_inputs'))
        t, n = ctypes.c_uint32(), ctypes.c_uint32()
        if self.output(p, ctypes.byref(t), ctypes.byref(n)) == 0:
            line += ' output=%d,%d' % (t.value, n.value)
        lines = [line]
        for i in range(self.count(p)):
            q = self.param(p, i)
            name = self.name(q)
            fields = {k: f(q) for k, f in self.fields}
            lines.append('param %d offset=%d category=%d name=%s %s reg_format=%d' % (
                i, q - p, fields.pop('category'), name.decode('latin-1') if name else '-',
                ' '.join('%s=%d' % kv for kv in fields.items()), self.reg_format(p, q)))
        return lines


def ours(psvusse, paths):
    r = subprocess.run([psvusse, '--params'] + paths, capture_output=True, text=True, errors='replace')
    if len(paths) == 1:
        return {paths[0]: r.stdout.splitlines()}
    out, cur = {}, None
    for line in r.stdout.splitlines():
        if line.startswith('== '):
            cur = line[3:]
            out[cur] = []
        elif cur is not None:
            out[cur].append(line)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', help='GXP files or folders of them')
    ap.add_argument('--psvusse', required=True)
    ap.add_argument('--sdk', required=True, help='SDK root (host_tools/lib.x64/gxm_tool.dll)')
    ap.add_argument('--show', type=int, default=20, help='differences to print')
    args = ap.parse_args()
    psvusse = os.path.abspath(args.psvusse)
    gxm = Gxm(args.sdk)
    files = []
    for p in args.inputs:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, n) for n in sorted(names) if n.lower().endswith('.gxp')]
        else:
            files.append(p)
    if not files:
        sys.exit('no GXP files')
    got = {}
    for i in range(0, len(files), 64):
        got.update(ours(psvusse, files[i:i + 64]))
    same = rejected = shown = 0
    for f in files:
        ref = gxm.report(open(f, 'rb').read())
        if ref is None:
            rejected += 1
            continue
        # (libgxm has no function for input records and output layouts)
        mine = [l for l in got.get(f, []) if not l.startswith('input ') and not l.startswith('outputs ')]
        if ref == mine:
            same += 1
        elif shown < args.show:
            shown += 1
            j = next(k for k in range(max(len(ref), len(mine)))
                     if (ref[k] if k < len(ref) else None) != (mine[k] if k < len(mine) else None))
            print('%s\n   libgxm:  %s\n   psvusse: %s' % (f, ref[j] if j < len(ref) else None,
                                                         mine[j] if j < len(mine) else None))
    checked = len(files) - rejected
    print('%d of %d programs identical (%d rejected by sceGxmProgramCheck)' % (same, checked, rejected))
    return 0 if same == checked else 1


if __name__ == '__main__':
    sys.exit(main())
