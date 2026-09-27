"""Check the Fantasia Map Generator stand-ins against the original.

Requires lupa.lua52 and the Fantasia workshop mod (2916150031), which is only
read. The mod is installed into a temporary mods folder; each stand-in is loaded
the way the game loads it (its own gen file, package.path over the mod folders
and the game's res/scripts), and its pipeline is compared with the original
generator's: untouched up to 32 x 32 km, and above that under the symbolic
replay of test_generation_memory.verify.
"""
from pathlib import Path
import tempfile
from lupa.lua52 import LuaRuntime
import test_generation_memory as t
from install_fantasia_low_memory import install, FOLDER

FANTASIA = Path(r'C:\Program Files (x86)\Steam\steamapps\workshop\content\1066780\2916150031\res')
GENERATORS = (('fantasia_map_generator.gen.lua', 'temperate.clima.lua'),
              ('fantasia_map_generator_dry.gen.lua', 'dry.clima.lua'),
              ('fantasia_map_generator_tropical.gen.lua', 'tropical.clima.lua'))


def runtime(script_dirs, lines):
    L = LuaRuntime(unpack_returned_tuples=True)
    # Backslashes, as the game's own package.path on Windows has them.
    L.globals().package.path = ';'.join(str(d / '?.lua') for d in script_dirs) + ';' + L.globals().package.path
    L.execute('_=function(s) return s end; require "mathutil"')
    L.globals().print = lambda *a: lines.append(' '.join(str(x) for x in a))
    L.globals().debugPrint = lambda *a: None
    return L


def generator(L, path):
    L.execute(path.read_text(encoding='utf-8'))
    return L.globals().data()


def generate(L, info, water, seed, km):
    p = L.table_from({row['key']: row['defaultIndex'] for _, row in info.params.items()})
    p.water, p.mapSizeX, p.mapSizeY = water, km * 1024, km * 1024
    p.bounds = L.table_from(dict(min=L.table_from(dict(x=-p.mapSizeX / 2, y=-p.mapSizeY / 2)),
                                 max=L.table_from(dict(x=p.mapSizeX / 2, y=p.mapSizeY / 2))))
    L.globals().math.randomseed(seed)
    return info.updateFn(p)


def pair(mod, file, lines):
    L = runtime((mod / 'res/scripts', FANTASIA / 'scripts', t.RES / 'scripts'), lines)
    standin = generator(L, mod / 'res/config/terrain_generators' / file)
    R = runtime((FANTASIA / 'scripts', t.RES / 'scripts'), [])
    stock = generator(R, FANTASIA / 'config/terrain_generators' / file)
    return L, standin, R, stock


def main():
    if not FANTASIA.is_dir():
        print(f'SKIP: Fantasia Map Generator not found at {FANTASIA}')
        return
    with tempfile.TemporaryDirectory() as td:
        mods = Path(td)
        assert install(mods) > 0 and install(mods) == 0
        mod = mods / FOLDER
        armed = '[tpf2_bigmap] {}: buffer reuse armed for maps over 1024 km2'
        for file, climate in GENERATORS:
            lines = []
            L, standin, R, stock = pair(mod, file, lines)
            assert standin.name == stock.name and standin.climate == stock.climate == climate
            assert standin.order == stock.order
            assert t.native(standin.params) == t.native(stock.params)
            assert lines == [armed.format(file)], lines
            before = t.native(generate(R, stock, 2, 35924, 40))
            after = t.native(generate(L, standin, 2, 35924, 40))
            a, b = t.verify(before, after)
            assert lines[1:] == [f'[tpf2_bigmap] terrain memory: {a} -> {b} named buffers'], lines
            assert b <= 12, b
            print(f'{file:<42} 40 km  {len(t.indices(before["layers"])):>6} layers  {a} -> {b} named buffers')

        # 32 x 32 km and below: Fantasia's pipeline, untouched.
        file = GENERATORS[0][0]
        lines = []
        L, standin, R, stock = pair(mod, file, lines)
        before = t.native(generate(R, stock, 2, 35924, 32))
        after = t.native(generate(L, standin, 2, 35924, 32))
        assert before == after and lines == [armed.format(file)], lines
        print(f'{file:<42} 32 km  unchanged ({len(t.indices(before["layers"]))} layers)')

        # Fantasia not active: the stand-in is listed, but refuses to generate.
        lines = []
        L = runtime((mod / 'res/scripts', t.RES / 'scripts'), lines)
        missing = generator(L, mod / 'res/config/terrain_generators' / file)
        assert 'not active' in missing.name and missing.climate == 'temperate.clima.lua', missing.name
        assert lines and 'cannot run' in lines[0], lines
        try:
            missing.updateFn(L.table())
        except Exception as e:
            assert 'cannot run' in str(e), e
        else:
            raise AssertionError('stand-in without Fantasia generated a map')

        other = mods / 'someone_else_1'
        other.mkdir()
        (other / 'mod.lua').write_text('function data() return {} end')
        (mods / FOLDER).rename(mods / 'moved')
        other.rename(mods / FOLDER)
        try:
            install(mods)
        except ValueError:
            pass
        else:
            raise AssertionError('foreign folder overwritten')
        (mods / FOLDER).rename(other)
        (mods / 'moved').rename(mods / FOLDER)
        assert install(mods, remove=True) == 1 and not (mods / FOLDER).exists()
    print('PASS: Fantasia stand-ins (temperate, dry, tropical): reuse at 40 km, untouched at 32 km, '
          'missing-Fantasia refusal, install/remove')


if __name__ == '__main__':
    main()
