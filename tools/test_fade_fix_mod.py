"""Offline checks of mod/bigmap_fade_fix_1 (the street fade fix shaders).

Without a game install:
  * each of the five shaders holds the patched distance exactly once and no
    longer holds the stock statement;
  * undoing the patch (the block back to the one stock line) leaves a shader
    whose only difference from itself is that statement, with balanced braces;
  * mod.lua and strings.lua load (with `lua` on PATH, or LUA=; skipped without
    one), mod.lua's description key exists, and the 16 languages each hold it.
With the game installed (TPF2_GAME, or C:\\Games\\Transport Fever 2, or the Steam
default): the builder's output, regenerated into a temp folder, equals the
committed shaders byte for byte, and the stock shaders are recovered exactly by
undoing the patch. Without it, those two checks are skipped, not failed.

    python tools/test_fade_fix_mod.py
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT / 'mod' / 'bigmap_fade_fix_1'
spec = importlib.util.spec_from_file_location('build_fade_fix_mod', ROOT / 'tools' / 'build_fade_fix_mod.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

SHADER_DIR = MOD / 'res' / 'shaders2'


def undo(patched, rel):
    """The stock text: the inserted block (comment + ternary) back to one line."""
    dist = builder.FILES[rel]
    start = patched.index(builder.COMMENT)
    line_start = patched.index('float distToCam', start)
    indent = patched[patched.rfind('\n', 0, line_start) + 1:line_start]
    new =(builder.COMMENT + f'{indent}float distToCam = {builder.NO_BOX}\n'
           f'{indent}\t? {dist}\n'
           f'{indent}\t: calcDistToBox(u_view.viewInverse[3].xyz, instAttrBbox);')
    assert patched.count(new) == 1, f'{rel}: the patched block is not the expected one'
    return patched.replace(new, indent + builder.ANCHOR, 1)


def game_dir():
    for c in (os.environ.get('TPF2_GAME'), r'C:\Games\Transport Fever 2',
              r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2'):
        if c and (Path(c) / 'res' / 'shaders2').is_dir():
            return Path(c)
    return None


def main():
    assert set(builder.FILES) == {
        'mat/vs/std/normal.vs', 'mat/vs/std/color.vs', 'mat/vs/std/color_diffuse.vs',
        'mat/vs/leafcard/normal.vs', 'mat/vs/leafcard/color.vs'}
    for rel, dist in builder.FILES.items():
        text = (SHADER_DIR / rel).read_text(encoding='utf-8')
        assert text.count(builder.NO_BOX) == 1, rel
        assert text.count(dist) == 1, rel
        assert text.count(builder.ANCHOR) == 0, f'{rel}: the stock statement is still there'
        stock = undo(text, rel)
        assert stock.count(builder.ANCHOR) == 1, rel
        assert stock.count('{') == stock.count('}') and stock.count('(') == stock.count(')'), rel
        # a box-carrying object still takes the stock branch
        assert text.count('calcDistToBox(u_view.viewInverse[3].xyz, instAttrBbox)') == 1, rel
    print('shaders: patched once each, undo restores the stock statement')

    lua_exe = os.environ.get('LUA') or shutil.which('lua')
    if not lua_exe:
        print('no Lua interpreter (lua on PATH, or LUA=): mod.lua / strings.lua not loaded')
    else:
        snippet = r'''
function _(k) return k end
dofile("%s")
local info = data().info
assert(info.name == "Big Maps: street fade fix", "mod name")
assert(info.description == "mod_desc", "description key")
dofile("%s")
local langs, n = data(), 0
for code, t in pairs(langs) do
    n = n + 1
    assert(type(t.mod_desc) == "string" and t.mod_desc ~= "", code .. " lacks mod_desc")
end
assert(langs.en and langs.fr, "en and fr")
print(n)
''' % ((MOD / 'mod.lua').as_posix(), (MOD / 'strings.lua').as_posix())
        r = subprocess.run([lua_exe, '-e', snippet], capture_output=True, text=True, encoding='utf-8')
        assert r.returncode == 0, r.stderr.strip()
        assert int(r.stdout.strip()) == 16, f'expected 16 languages, got {r.stdout.strip()}'
        print('mod.lua / strings.lua: load, and the 16 languages each hold mod_desc')

    game = game_dir()
    if not game:
        print('SKIP: no game install found (set TPF2_GAME): regeneration check not run')
        print('PASS')
        return
    with tempfile.TemporaryDirectory() as tmp:
        sys.argv = ['build_fade_fix_mod.py', '--game', str(game), '--out', tmp]
        builder.main()
        for rel in builder.FILES:
            fresh = (Path(tmp) / 'res' / 'shaders2' / rel).read_bytes()
            kept = (SHADER_DIR / rel).read_bytes()
            assert fresh == kept, f'{rel}: differs from what the builder makes from {game}'
            stock = (game / 'res' / 'shaders2' / rel).read_text(encoding='utf-8')
            assert undo(fresh.decode('utf-8'), rel) == stock, f'{rel}: undo does not give the installed shader'
    print(f'regenerated from {game}: the five shaders equal the committed ones, undo gives the stock files')
    print('PASS')


if __name__ == '__main__':
    main()
