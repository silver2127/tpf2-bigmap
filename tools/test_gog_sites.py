"""Install the Steam-RVA modules in GOG mode against the real GOG executable.

Each installer runs through RunWithSteamSites (src/gog_rva.h) with a fake
host whose verifyBytes compares against the GOG TransportFever2.exe on disk
and whose installHook / patchBytes only record where they land. Passing means
every byte check of the module succeeds on GOG and every hook and patch lands
on the GOG address measured by tools/gog_align_code.py. Nothing is patched.

    python tools/test_gog_sites.py [--exe <GOG TransportFever2.exe>]
Needs out/tpf2_bigmap.dll (build.bat) and pefile. TPF2_GOG_EXE also sets the exe.
"""
import argparse
import ctypes as C
import os
import sys
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x140000000
GOG_STAMP = 0x675ad7cc

logtype = C.CFUNCTYPE(None, C.c_char_p)
basetype = C.CFUNCTYPE(C.c_size_t)
verifytype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)
hooktype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_int, C.POINTER(C.c_void_p))
patchtype = C.CFUNCTYPE(C.c_int, C.c_size_t, C.c_void_p, C.c_uint32)


class Host(C.Structure):
    _fields_ = [('size', C.c_uint32), ('abi', C.c_uint32), ('log', logtype),
                ('cfgInt', C.c_void_p), ('cfgBool', C.c_void_p), ('cfgStr', C.c_void_p),
                ('base', basetype), ('buildOk', C.c_void_p), ('verify', verifytype),
                ('hook', hooktype), ('patch', patchtype), ('dataDir', C.c_void_p)]


# installer export -> (hooks, patches) it must place, as GOG RVAs
EXPECTED = {
    'BigmapTestInstallMaterialIndex': ({0x316140}, set()),
    'BigmapTestInstallTerrainRefine': ({0x3ac8e0}, set()),
    'BigmapTestInstallTerrainMinMax': ({0x30a760}, {0x33d0e1}),
    'BigmapTestInstallTerrainAlign': ({0x3b3690}, set()),
    'BigmapTestInstallAlignmentBatch': ({0xaad7b0}, set()),
}


PORTED = ['material_index.h', 'terrain_refine.h', 'terrain_minmax.h', 'terrain_align_fast.h', 'alignment_batch.h']


def lint_sources():
    """A Steam RVA added to the module base outside installHook is a GOG crash
    (installHook targets are translated by the host; nothing else is): every
    such sum must go through GameRva. Returns the offending lines."""
    import re
    bad = []
    for name in PORTED:
        for n, line in enumerate((ROOT / 'src' / name).read_text().splitlines(), 1):
            code = line.split('//')[0]
            if 'installHook' in code:
                continue
            for m in re.finditer(r'(?:\bbase|moduleBase\(\))\s*\+\s*(\(?[^;,)]*)', code):
                if 'GameRva' not in m.group(1) and re.search(r'\bk[A-Z]\w*|0x[0-9a-fA-F]{5,}', m.group(1)):
                    bad.append(f'{name}:{n}: {line.strip()}')
    return bad


def main():
    bad = lint_sources()
    if bad:
        print('FAIL: Steam RVA used without GameRva:'); [print('  ' + b) for b in bad]
        sys.exit(1)
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.environ.get('TPF2_GOG_EXE', r'C:\Games\Transport Fever 2\TransportFever2.exe'))
    args = ap.parse_args()
    pe = pefile.PE(args.exe, fast_load=True)
    if pe.FILE_HEADER.TimeDateStamp != GOG_STAMP:
        sys.exit(f'{args.exe} is not the GOG 2024-12-12 build')
    image = pe.get_memory_mapped_image(ImageBase=BASE)
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    gogrva = dll.BigmapTestGogRva
    gogrva.restype = C.c_size_t
    gogrva.argtypes = [C.c_size_t]
    assert gogrva(0x3b3470) == 0x3b3690 and gogrva(0x2f87d20) == 0x2f73d30 and gogrva(0x1000) == 0
    block = C.create_string_buffer(64)
    failures = []
    for name, (want_hooks, want_patches) in EXPECTED.items():
        hooks, patches, verified, bad, logs = set(), set(), 0, [], []

        @logtype
        def log(fmt):
            logs.append(fmt.decode(errors='replace'))

        @basetype
        def getbase():
            return BASE

        @verifytype
        def verify(rva, ptr, size):
            nonlocal verified
            ok = rva != 0 and image[rva:rva + size] == C.string_at(ptr, size)
            verified += 1
            if not ok:
                bad.append(hex(rva))
            return int(ok)

        @hooktype
        def hook(target, detour, steal, out):
            hooks.add(target - BASE)
            out[0] = C.cast(block, C.c_void_p)
            return 1

        @patchtype
        def patch(rva, ptr, size):
            patches.add(rva)
            return 1

        host = Host(C.sizeof(Host), 1, log, None, None, None, getbase, None, verify, hook, patch, None)
        fn = getattr(dll, name)
        fn.argtypes = [C.POINTER(Host), C.c_int, C.c_int]
        ok = bool(fn(C.byref(host), 1, 512 if 'AlignmentBatch' in name else 1))
        line = (f'{name}: install={ok} verified={verified} mismatches={bad} '
                f'hooks={sorted(hex(h) for h in hooks)} patches={sorted(hex(p) for p in patches)}')
        print(line)
        if not ok or bad or hooks != want_hooks or patches != want_patches:
            failures.append(line + ' | log: ' + ' / '.join(logs))
    if failures:
        print('FAIL:'); [print('  ' + f) for f in failures]
        sys.exit(1)
    print('PASS: material index, terrain refine, terrain minmax, terrain align and alignment batch install on the GOG '
          'executable through the Steam->GOG table; every byte check matches and every hook/patch '
          'lands on its measured GOG address')


if __name__ == '__main__':
    main()
