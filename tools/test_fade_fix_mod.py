"""Offline checks of mod/bigmap_fade_fix_1 (the street fade fix shaders).

Without a game install:
  * each of the five shaders holds the patched distance exactly once and no
    longer holds the stock statement;
  * undoing the patch (the block back to the one stock line) leaves a shader
    whose only difference from itself is that statement, with balanced braces;
  * mod.lua and strings.lua parse, and every string key mod.lua uses exists in
    en and fr (under Lua, when `lupa` is installed).
With the game installed (TPF2_GAME, or C:\\Games\\Transport Fever 2, or the Steam
default): the builder's output, regenerated into a temp folder, equals the
committed shaders byte for byte, and the stock shaders are recovered exactly by
undoing the patch. Without it, those two checks are skipped, not failed.

    python tools/test_fade_fix_mod.py
"""
import importlib.util
import os
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

    try:
        import lupa
    except ImportError:
        print('lupa not installed: mod.lua / strings.lua not parsed (pip install lupa)')
    else:
        lua = lupa.LuaRuntime(unpack_returned_tuples=True)
        strings = lua.execute((MOD / 'strings.lua').read_text(encoding='utf-8') + '\nreturn data()')
        lua.execute('function _(k) return k end')
        info = lua.execute((MOD / 'mod.lua').read_text(encoding='utf-8') + '\nreturn data()')['info']
        assert info['name'] == 'mod_name' and info['description'] == 'mod_desc'
        for lang in ('en', 'fr'):
            for key in ('mod_name', 'mod_desc'):
                assert strings[lang][key], (lang, key)
        print('mod.lua / strings.lua: parse, en and fr hold mod_name and mod_desc')

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
