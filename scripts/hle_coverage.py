#!/usr/bin/env python3
"""System library coverage: what psvrecore implements against the SDK's public API, and against what games import.

    hle_coverage.py --sdk <SDK root> [--out coverage.md] [--imports-out imports.md] [--psvinfo <psvinfo>]
                    [game ...]

Implemented functions are read from the sources: every `Function` / `Variable` table a `register_library` call in
libs/*/src registers (HLE_FUNCTION entries and the helper macros built on Function). The reference for each library
is the SDK's stub libraries (`psvinfo stubs <sdk>/target/lib`), matched by library name and NID; a library the SDK
does not have is measured against vita-headers' firmware database (third_party/vita-headers/db/360), which also
gives each library's module and names the NIDs the SDK does not.

Each game argument is `[Name=]path`, the path a game folder or one executable or module. A folder is searched for
eboot.bin, *.self, *.suprx, *.sprx and *.prx (folders whose names start with a dot, sce_sys and addcont are
skipped; under app/<ID>/ and patch/<ID>/ the patch's copy of a file replaces the app's). Each file is read with `psvinfo imports` and
`psvinfo exports`; encrypted ones are reported and left out. Imports from a library one of the game's own modules
exports (libc, libult, libfios2, ...) are the game's: they are left out. The name defaults to the folder's name.

Outputs (Markdown):
  --out          per library: implemented of the reference, the percentage, the implemented and missing functions,
                 and for each missing one the given games that import it (default: standard output)
  --imports-out  with games: per game, the system functions it imports and how many are implemented; per library,
                 the union; every missing function with the games that import it

Nothing from a game is written anywhere but these outputs: keep them outside the repository.
"""
import argparse
import collections
import concurrent.futures
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MODULE_EXTS = ('.self', '.suprx', '.sprx', '.prx')
SKIP_DIRS = ('addcont', 'sce_sys')  # (downloadable content; the system's own files: sce_sys/about/right.suprx)


# --- implemented: the HLE tables --------------------------------------------------------------------------------

def strip_comments(text):
    # (keeps string literals: a // inside one is left alone)
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


TABLE_RE = re.compile(r'\bconst\s+(?:[\w:]*::)?(Function|Variable)\s+(\w+)\s*\[\s*\]\s*=\s*\{')
ENTRY_RE = re.compile(r'(?:\b\w+\s*\(|\{)\s*(0x[0-9A-Fa-f]+)\s*,\s*(?:"([^"]+)"|([A-Za-z_]\w*))')
REGISTER_RE = re.compile(r'register_library\s*\(\s*\{\s*"(\w+)"\s*,\s*([\w:]+)\s*,\s*(\w+|\{\s*\})\s*(?:,\s*(\w+)\s*)?\}')
CONSTANT_RE = re.compile(r'\b(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)')
LIBRARY_NIDS = {}  # (filled by read_hle: library -> the NIDs it is registered with)


def table_body(text, start):
    depth, i = 1, start
    while i < len(text) and depth:
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
        i += 1
    return text[start:i - 1]


def read_hle(root):
    """{library: {nid: (name, kind)}} from every source file that registers a library."""
    libs = collections.defaultdict(dict)
    files = 0
    for path in sorted(glob.glob(os.path.join(root, 'libs', '*', 'src', '**', '*.cpp'), recursive=True)):
        with open(path, encoding='utf-8') as f:
            text = f.read()
        if 'register_library' not in text:
            continue
        text = strip_comments(text)
        tables = {}
        for m in TABLE_RE.finditer(text):
            entries = {}
            for e in ENTRY_RE.finditer(table_body(text, m.end())):
                entries[int(e.group(1), 16)] = e.group(2) or e.group(3)
            tables[m.group(2)] = (m.group(1).lower(), entries)
        constants = {c.group(1): int(c.group(2), 16) for c in CONSTANT_RE.finditer(text)}
        for m in REGISTER_RE.finditer(text):
            files += 1
            nid = constants.get(m.group(2).split('::')[-1])
            if nid is not None:
                LIBRARY_NIDS.setdefault(m.group(1), set()).add(nid)
            for table in (m.group(3), m.group(4)):
                if not table or table.startswith('{'):  # (no functions, only variables)
                    continue
                if table not in tables:
                    sys.exit('%s: register_library uses %s, which is not a table in that file' % (path, table))
                kind, entries = tables[table]
                for nid, name in entries.items():
                    libs[m.group(1)][nid] = (name, kind)
    if not files:
        sys.exit('no register_library calls found under %s/libs' % root)
    return libs


# --- the reference: SDK stubs and vita-headers' database --------------------------------------------------------

def read_stubs(psvinfo, sdk):
    """{library: {nid: (name, kind)}}, {library: library NID} from the SDK's stub libraries."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 'stubs.tsv')
        r = subprocess.run([psvinfo, 'stubs', os.path.join(sdk, 'target', 'lib'), out], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            sys.exit('psvinfo stubs failed:\n%s%s' % (r.stdout, r.stderr))
        with open(out, encoding='utf-8') as f:
            lines = f.read().splitlines()
    names, libs, lib_nids = {}, collections.defaultdict(dict), {}
    for line in lines:
        p = line.split('\t')
        if p[0] == 'L' and len(p) >= 3:
            names[p[1]] = p[2]
            lib_nids[p[2]] = int(p[1], 16)
    for line in lines:
        p = line.split('\t')
        if p[0] in ('F', 'V') and len(p) >= 4 and p[1] in names:
            libs[names[p[1]]][int(p[2], 16)] = (p[3], 'function' if p[0] == 'F' else 'variable')
    return libs, lib_nids


def read_db(db_dir):
    """{library: {nid: (name, kind)}}, {library: (module, kernel)} from vita-headers' yml files (a small reader for
    their fixed layout: modules > module > libraries > library > functions/variables > name: nid)."""
    libs, modules = collections.defaultdict(dict), {}
    for path in sorted(glob.glob(os.path.join(db_dir, '*.yml'))):
        module = lib = section = None
        kernel = False
        with open(path, encoding='utf-8') as f:
            for raw in f:
                line = raw.rstrip()
                if not line.strip() or line.lstrip().startswith('#'):
                    continue
                indent = len(line) - len(line.lstrip())
                key, _, value = line.strip().partition(':')
                value = value.strip()
                if indent == 2:
                    module, lib, section = key, None, None
                elif indent == 6:
                    lib, section, kernel = key, None, False
                elif indent == 8 and lib:
                    if key == 'kernel':
                        kernel = value == 'true'
                        known = modules.get(lib)
                        if known is None or (known[1] and not kernel):
                            modules[lib] = (module, kernel)
                    section = key if key in ('functions', 'variables') else None
                elif indent == 10 and lib and section and value.startswith('0x'):
                    libs[lib][int(value, 16)] = (key, 'function' if section == 'functions' else 'variable')
                if indent == 6 and lib and lib not in modules:
                    modules[lib] = (module, False)
    return libs, modules


# --- games -------------------------------------------------------------------------------------------------------

def game_files(path):
    if os.path.isfile(path):
        return [path]
    found = {}
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith('.') and d.lower() not in SKIP_DIRS)
        for fn in filenames:
            low = fn.lower()
            if low != 'eboot.bin' and not low.endswith(MODULE_EXTS):
                continue
            full = os.path.join(dirpath, fn)
            parts = os.path.relpath(full, path).replace('\\', '/').split('/')
            key, rank = '/'.join(parts), 0
            for i, part in enumerate(parts[:-2]):
                if part.lower() in ('app', 'patch'):
                    key, rank = '/'.join(parts[i + 2:]).lower(), 1 if part.lower() == 'patch' else 0
                    break
            if key not in found or found[key][0] < rank:
                found[key] = (rank, full)
    return sorted(v[1] for v in found.values())


def game_name(arg):
    if '=' in arg and not os.path.exists(arg):
        name, path = arg.split('=', 1)
        return name, path
    base = os.path.basename(os.path.normpath(arg))
    if base.lower() == 'extracted':
        base = os.path.basename(os.path.dirname(os.path.normpath(arg)))
    name = re.sub(r'\s*[\[(][^\])]*[\])]', '', base).replace('_ ', ': ').strip()
    return name or base, arg


def psvinfo_rows(psvinfo, command, path):
    r = subprocess.run([psvinfo, command, path], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode != 0:
        return None, (r.stderr or r.stdout).strip().splitlines()[-1:] or ['failed']
    rows = []
    for line in r.stdout.splitlines()[1:]:
        p = line.split('\t')
        if len(p) >= 6:
            rows.append((p[0], p[2], int(p[3], 16), p[5]))
    if not rows and 'encrypted' in (r.stderr or ''):
        return None, ['encrypted']
    return rows, None


def read_game(psvinfo, path, pool):
    files = game_files(path)
    imports = pool.map(lambda f: psvinfo_rows(psvinfo, 'imports', f), files)
    exports = pool.map(lambda f: psvinfo_rows(psvinfo, 'exports', f), files)
    used, own, unreadable, read = {}, set(), [], 0
    for f, (imp, err), (exp, _) in zip(files, imports, exports):
        if imp is None:
            unreadable.append((os.path.relpath(f, path) if os.path.isdir(path) else f, err[0]))
            continue
        read += 1
        for lib, kind, nid, name in imp:
            used.setdefault((lib, nid), (name, kind))
        for lib, kind, nid, name in exp or []:
            if lib != '(module)':
                own.add(lib)
    return {k: v for k, v in used.items() if k[0] not in own}, sorted(own), read, unreadable


# --- the report --------------------------------------------------------------------------------------------------

def find_psvinfo():
    exe = '.exe' if os.name == 'nt' else ''
    hits = glob.glob(os.path.join(ROOT, 'build', '*', 'tools', 'psvinfo', 'psvinfo' + exe))
    return max(hits, key=os.path.getmtime) if hits else None


def pct(a, b):
    return '%d%%' % (100 * a // b) if b else '—'


def code_list(names):
    return ', '.join('`%s`' % n for n in names) if names else 'none'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sdk', required=True, help='the SDK root (target/lib holds the stub libraries)')
    ap.add_argument('--psvinfo', help='psvinfo executable (default: the newest build/*/tools/psvinfo)')
    ap.add_argument('--db', default=os.path.join(ROOT, 'third_party', 'vita-headers', 'db', '360'),
                    help="vita-headers' NID database folder")
    ap.add_argument('--out', help='the coverage report (default: standard output)')
    ap.add_argument('--imports-out', help='the per-game import survey (needs games)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--check', action='store_true',
                    help='only check the tables: every library registered with another NID than the SDK has, every entry '
                         'whose name the reference gives another NID, or whose NID the reference names otherwise '
                         '(exit status 1 if any)')
    ap.add_argument('games', nargs='*', help='[Name=]path: a game folder, executable or module')
    args = ap.parse_args()

    psvinfo = args.psvinfo or find_psvinfo()
    if not psvinfo or not os.path.exists(psvinfo):
        sys.exit('psvinfo not found (build psvrecore\'s tools or pass --psvinfo)')

    hle = read_hle(ROOT)
    sdk, lib_nids = read_stubs(psvinfo, args.sdk)
    db, modules = read_db(args.db)

    if args.check:
        sys.exit(check_names(hle, sdk, db, lib_nids))

    games = []  # (name, {(lib, nid): (name, kind)}, own libs, files read, unreadable)
    if args.games:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            for g in args.games:
                name, path = game_name(g)
                if not os.path.exists(path):
                    sys.exit('not found: %s' % path)
                used, own, read, bad = read_game(psvinfo, path, pool)
                print('%s: %d files read, %d unreadable, %d system imports' % (name, read, len(bad), len(used)),
                      file=sys.stderr)
                games.append((name, used, own, read, bad))

    def implemented(lib, nid):
        return nid in hle.get(lib, {})

    def name_of(lib, nid, fallback=None):
        for table in (hle, sdk, db):
            if nid in table.get(lib, {}):
                return table[lib][nid][0]
        if fallback and fallback not in ('?', '') and not fallback.startswith(('0x', 'NID')):
            return fallback
        for table in (sdk, db):  # (another library's name for the NID: an alias)
            for entries in table.values():
                if nid in entries:
                    return entries[nid][0]
        return 'NID 0x%08X' % nid

    imported_by = collections.defaultdict(list)  # (lib, nid) -> [game]
    import_names = {}
    for gname, used, _, _, _ in games:
        for (lib, nid), (n, _) in used.items():
            imported_by[(lib, nid)].append(gname)
            import_names[(lib, nid)] = n

    libraries = set(hle) | {lib for lib, _ in imported_by}
    rows = []
    for lib in libraries:
        if lib in sdk:
            ref, ref_from = sdk[lib], 'SDK stubs'
        elif lib in db:
            ref, ref_from = db[lib], "vita-headers' firmware database (not in the SDK stubs)"
        else:
            ref = {nid: (import_names[(l, nid)], 'function') for (l, nid) in imported_by if l == lib}
            ref_from = 'only what the games import (in neither the SDK nor the database)'
        impl = sorted(n for n in ref if implemented(lib, n))
        extra = sorted(n for n in hle.get(lib, {}) if n not in ref)
        missing = sorted(n for n in ref if not implemented(lib, n))
        users = {g for (l, _), gs in imported_by.items() if l == lib for g in gs}
        rows.append((lib, ref, ref_from, impl, extra, missing, users))
    rows.sort(key=lambda r: (-(len(r[3]) / len(r[1]) if r[1] else 0), r[0].lower()))

    out = []
    w = out.append
    w('# psvrecore: system library coverage\n')
    w('Implemented functions (psvrecore\'s `register_library` tables) against the functions and variables each '
      'library exports in the SDK\'s stub libraries (`psvinfo stubs <sdk>/target/lib`): the public API a game can '
      'import, matched by library name and NID. Libraries the SDK does not have are measured against vita-headers\' '
      'firmware database. The module is the one that database puts the library in. Made by '
      '`scripts/hle_coverage.py`.\n')
    if games:
        w('Games given: %s.\n' % ', '.join(g[0] for g in games))
    total_ref = sum(len(r[1]) for r in rows if r[2] == 'SDK stubs')
    total_impl = sum(len(r[3]) for r in rows if r[2] == 'SDK stubs')
    w('Of the SDK\'s functions in the libraries below: %d of %d implemented (%s).\n' % (total_impl, total_ref,
                                                                                       pct(total_impl, total_ref)))
    head = '| Library | Module | Implemented | Of | % |' + (' Games importing it |' if games else '')
    w(head)
    w('|---|---|---|---|---|' + ('---|' if games else ''))
    for lib, ref, ref_from, impl, extra, missing, users in rows:
        module = modules.get(lib, ('?', False))[0]
        mark = '' if ref_from == 'SDK stubs' else ' *'
        cell = '%d (+%d)' % (len(impl), len(extra)) if extra else '%d' % len(impl)
        line = '| %s%s | %s | %s | %d | %s |' % (lib, mark, module, cell, len(ref), pct(len(impl), len(ref)))
        if games:
            line += ' %d |' % len(users)
        w(line)
    w('\n(+N: implemented functions the reference does not list, e.g. internal ones a game imports. *: the reference '
      'is not the SDK; see the library\'s section.)\n')

    for lib, ref, ref_from, impl, extra, missing, users in rows:
        module = modules.get(lib, ('?', False))[0]
        w('## %s (%s): %s  (%d of %d; reference: %s)\n' % (lib, module, pct(len(impl), len(ref)), len(impl), len(ref),
                                                          ref_from))
        w('**Implemented (%d):** %s\n' % (len(impl), code_list(sorted(name_of(lib, n) for n in impl))))
        if extra:
            w('**Also implemented, not in the reference (%d):** %s\n' % (len(extra),
                                                                         code_list(sorted(name_of(lib, n) for n in extra))))
        if not games or not missing:
            w('**Not implemented (%d):** %s\n' % (len(missing), code_list(sorted(name_of(lib, n) for n in missing))))
            continue
        used = sorted((n for n in missing if imported_by.get((lib, n))),
                      key=lambda n: (-len(imported_by[(lib, n)]), name_of(lib, n).lower()))
        unused = sorted(name_of(lib, n) for n in missing if not imported_by.get((lib, n)))
        w('**Not implemented (%d), imported by the given games (%d):**%s\n' % (len(missing), len(used),
                                                                             '' if used else ' none'))
        for n in used:
            gs = imported_by[(lib, n)]
            w('- `%s` — %d: %s' % (name_of(lib, n, import_names.get((lib, n))), len(gs), ', '.join(gs)))
        if used:
            w('')
        w('**Not implemented, not imported by the given games (%d):** %s\n' % (len(unused), code_list(unused)))

    text = '\n'.join(out).rstrip() + '\n'
    if args.out:
        with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)

    if args.imports_out:
        if not games:
            sys.exit('--imports-out needs games')
        with open(args.imports_out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(imports_report(games, hle, name_of, imported_by, import_names))


def check_names(hle, sdk, db, lib_nids):
    """Entries whose name and NID disagree with the reference (the SDK's, else the database's): a NID mistyped, or a
    function registered under another's name. Names are compared without a leading underscore; an entry whose
    function is also registered at the reference's NID is an alias (an older NID of it) and is not reported."""
    problems = 0
    for lib, nids in sorted(LIBRARY_NIDS.items()):
        if lib in lib_nids and lib_nids[lib] not in nids:
            print('%s: the library is 0x%08X in the reference, registered as %s' % (
                lib, lib_nids[lib], ', '.join('0x%08X' % n for n in sorted(nids))))
            problems += 1
    for lib in sorted(hle):
        ref = sdk.get(lib) or db.get(lib) or {}
        by_name = {}
        for nid, (name, _) in ref.items():
            by_name.setdefault(name.lstrip('_'), set()).add(nid)
        for nid, (name, _) in sorted(hle[lib].items()):
            key = name.lstrip('_')
            if key in by_name and by_name[key] & set(hle[lib]):
                continue  # (also registered at the reference's NID: this one is another NID for it, e.g. an older one)
            if nid in ref and ref[nid][0].lstrip('_') != key and key in by_name:
                print('%s: 0x%08X is %s in the reference, registered as %s' % (lib, nid, ref[nid][0], name))
                problems += 1
            elif nid not in ref and key in by_name:
                print('%s: %s is 0x%s in the reference, registered as 0x%08X' % (
                    lib, name, '/0x'.join('%08X' % n for n in sorted(by_name[key])), nid))
                problems += 1
    print('%d problems' % problems)
    return 1 if problems else 0


def imports_report(games, hle, name_of, imported_by, import_names):
    out = []
    w = out.append
    w('# System functions the games import, against psvrecore\n')
    w('Imports of each game\'s executable and its own modules, read with `psvinfo imports` by '
      '`scripts/hle_coverage.py`. Imports that the game\'s own modules provide (libc, libult, libfios2, ...: '
      'recompiled with the game) are left out: what remains is what psvrecore must implement. "Implemented" means an '
      'entry in psvrecore\'s system library tables (by library and NID); it says nothing about how complete each '
      'implementation is. Imported is not called.\n')
    w('## Per game\n')
    w('| Game | Files read | System functions imported | Implemented | % | Libraries with something missing |')
    w('|---|---|---|---|---|---|')
    order = sorted(games, key=lambda g: -sum(1 for k in g[1] if k[1] in hle.get(k[0], {})) / max(1, len(g[1])))
    for gname, used, own, read, bad in order:
        impl = sum(1 for (lib, nid) in used if nid in hle.get(lib, {}))
        libs = sorted({lib for (lib, nid) in used if nid not in hle.get(lib, {})}, key=str.lower)
        files = '%d' % read + (' (%d encrypted or unreadable)' % len(bad) if bad else '')
        w('| %s | %s | %d | %d | %s | %s |' % (gname, files, len(used), impl, pct(impl, len(used)), ', '.join(libs) or '—'))
    allk = set(imported_by)
    missing = {k for k in allk if k[1] not in hle.get(k[0], {})}
    w('\n%d distinct system functions imported in all, %d implemented, %d missing.\n' % (len(allk), len(allk) - len(missing),
                                                                                         len(missing)))
    w('## Per library (union of all games)\n')
    w('| Library | Imported by any game | Implemented | Missing | Games using the library |')
    w('|---|---|---|---|---|')
    per = collections.defaultdict(list)
    for k in allk:
        per[k[0]].append(k)
    lib_users = {lib: {g for k in ks for g in imported_by[k]} for lib, ks in per.items()}
    for lib in sorted(per, key=lambda l: (-len(lib_users[l]), l.lower())):
        ks = per[lib]
        impl = sum(1 for k in ks if k not in missing)
        new = '' if lib in hle else ' (new)'
        w('| %s%s | %d | %d | %d | %d |' % (lib, new, len(ks), impl, len(ks) - impl, len(lib_users[lib])))
    w('\n## Missing functions, by how many games import them\n')
    libs = sorted({k[0] for k in missing}, key=lambda l: (-max(len(imported_by[k]) for k in missing if k[0] == l),
                                                            l.lower()))
    for lib in libs:
        ks = sorted((k for k in missing if k[0] == lib),
                    key=lambda k: (-len(imported_by[k]), name_of(lib, k[1], import_names.get(k)).lower()))
        none = '' if lib in hle else ' (not implemented at all)'
        w('### %s%s: %d missing\n' % (lib, none, len(ks)))
        for k in ks:
            gs = imported_by[k]
            w('- `%s` — %d: %s' % (name_of(lib, k[1], import_names.get(k)), len(gs), ', '.join(gs)))
        w('')
    bad_all = [(g[0], f, e) for g in games for f, e in g[4]]
    if bad_all:
        w('## Files not read\n')
        for gname, f, e in bad_all:
            w('- %s: `%s` (%s)' % (gname, f, e))
    return '\n'.join(out).rstrip() + '\n'


if __name__ == '__main__':
    main()
