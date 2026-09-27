"""Check src/generator_memory.h: the patched Fantasia Map Generator files it serves.

Requires out/tpf2_bigmap.dll (build.bat), lupa.lua52 and the Fantasia workshop
mod (2916150031), which is only read. For each Fantasia generator the DLL's
patched text is run like the game runs it and compared with the original:
identical pipeline up to 32 x 32 km, and at 40 km the symbolic replay of
test_generation_memory.verify with the buffer count down to the lower bound.
Also checks which paths are redirected, that every original line keeps its
number, refusal of a missing or repeated anchor, and the %TEMP% copy.
"""
import ctypes as C
from pathlib import Path
from lupa.lua52 import LuaRuntime
import test_generation_memory as t

ROOT = Path(__file__).resolve().parents[1]
FANTASIA = Path(r'C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\2916150031\res')
FILES = ('fantasia_map_generator.gen.lua', 'fantasia_map_generator_dry.gen.lua',
         'fantasia_map_generator_tropical.gen.lua')


def load_dll():
    dll = C.CDLL(str(ROOT / 'out/tpf2_bigmap.dll'))
    dll.BigmapTestGeneratorIndex.argtypes = [C.c_wchar_p]
    dll.BigmapTestGeneratorPatch.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t]
    dll.BigmapTestGeneratorPatch.restype = C.c_longlong
    dll.BigmapTestGeneratorRedirect.argtypes = [C.c_wchar_p, C.c_wchar_p, C.c_size_t]
    return dll


def patch(dll, src):
    n = dll.BigmapTestGeneratorPatch(src, len(src), None, 0)
    if n < 0:
        return None
    buf = C.create_string_buffer(n)
    assert dll.BigmapTestGeneratorPatch(src, len(src), buf, n) == n
    return buf.raw[:n]


def runtime(lines):
    L = LuaRuntime(unpack_returned_tuples=True)
    L.globals().package.path = ';'.join(str(d / 'scripts' / '?.lua') for d in (FANTASIA, t.RES)) + ';' + L.globals().package.path
    L.execute('_=function(s) return s end; require "mathutil"')
    L.globals().print = lambda *a: lines.append(' '.join(str(x) for x in a))
    L.globals().debugPrint = lambda *a: None
    return L


def generate(text, km):
    lines = []
    L = runtime(lines)
    L.execute(text.decode('utf-8'))
    info = L.globals().data()
    p = L.table_from({row['key']: row['defaultIndex'] for _, row in info.params.items()})
    p.water, p.mapSizeX, p.mapSizeY = 2, km * 1024, km * 1024
    p.bounds = L.table_from(dict(min=L.table_from(dict(x=-p.mapSizeX / 2, y=-p.mapSizeY / 2)),
                                 max=L.table_from(dict(x=p.mapSizeX / 2, y=p.mapSizeY / 2))))
    L.globals().math.randomseed(35924)
    return t.native(info.updateFn(p)), [x for x in lines if 'tpf2_bigmap' in x]


def main():
    dll = load_dll()
    gens = r'\res\config\terrain_generators' + '\\'
    ws = r'C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\2916150031'
    for path, want in ((ws + gens + FILES[0], 0), (ws + gens + FILES[1], 1), (ws + gens + FILES[2], 2),
                       (ws.replace('\\', '/') + gens.replace('\\', '/') + FILES[0].upper(), 0),
                       (r'D:\Games\TF2\mods\fantasia_1' + gens + FILES[2], 2),
                       (ws + gens + 'temperate.gen.lua', -1),
                       (ws + r'\res\scripts\terrain' + '\\' + FILES[0], -1),
                       (r'C:\Temp\tpf2_bigmap' + '\\' + FILES[0], -1),
                       (ws + gens + 'x' + FILES[0], -1)):
        assert dll.BigmapTestGeneratorIndex(path) == want, (path, want)
    assert patch(dll, b'local x = 1\n') is None
    assert patch(dll, b'\t\treturn result\n\t\treturn result\n') is None
    assert patch(dll, b'\t\treturn result -- no\n') is None
    assert patch(dll, b'x\t\treturn result\n') is None
    print('PASS: path matching and anchor refusal')

    if not FANTASIA.is_dir():
        print(f'SKIP: Fantasia Map Generator not found at {FANTASIA}')
        return
    for name in FILES:
        src = (FANTASIA / 'config/terrain_generators' / name).read_bytes()
        out = patch(dll, src)
        assert out is not None, name
        a, b = src.splitlines(), out.splitlines()
        assert len(b) > len(a) and sum(x != y for x, y in zip(a, b)) == 1, name
        before, _ = generate(src, 40)
        after, lines = generate(out, 40)
        x, y = t.verify(before, after)
        n = len(t.indices(before['layers']))
        assert lines == [f'[tpf2_bigmap] generator memory: 40960 x 40960 m, {n} layers over {x} buffer names',
                         f'[tpf2_bigmap] terrain memory: {x} -> {y} named buffers'], lines
        assert y == t.lower_bound(before) and y <= 12, (y, t.lower_bound(before))
        small, _ = generate(src, 32)
        same, lines = generate(out, 32)
        assert same == small and len(lines) == 1 and lines[0].endswith('(32 x 32 km or less: unchanged)'), (name, lines)
        print(f'{name:<42} 40 km: {x} -> {y} named buffers; 32 km: unchanged')

    alt = C.create_unicode_buffer(260)
    path = str(FANTASIA / 'config/terrain_generators' / FILES[0])
    assert dll.BigmapTestGeneratorRedirect(path, alt, 260)
    served = Path(alt.value)
    assert served.name == FILES[0] and served.parent.name == 'tpf2_bigmap'
    assert served.read_bytes() == patch(dll, Path(path).read_bytes())
    assert dll.BigmapTestGeneratorRedirect(path, alt, 260)   # unchanged copy: served again
    assert not dll.BigmapTestGeneratorRedirect(str(t.RES / 'config/terrain_generators/temperate.gen.lua'), alt, 260)
    print(f'PASS: generator memory (redirect copy {served})')


if __name__ == '__main__':
    main()
