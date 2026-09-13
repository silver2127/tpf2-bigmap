"""Validate buffer liveness/alias preservation using real game Lua pipelines.

Requires lupa.lua52. No game files or running processes are modified.
This checks operation graphs, not a rendered engine terrain comparison.
"""
from pathlib import Path
import tempfile
from lupa.lua52 import LuaRuntime
from install_generation_memory import install, patched, GENERATORS

ROOT = Path(__file__).resolve().parents[1]
RES = Path(r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\res')
KEYS = ('input', 'input1', 'input2', 'output')

def native(value):
    if hasattr(value, 'items'): return {k: native(v) for k, v in value.items()}
    return value

def verify(before, after):
    layers = before['layers']
    indices = sorted(k for k in layers if isinstance(k, int))
    assert {k:v for k,v in layers.items() if not isinstance(k, int)} == {
        k:v for k,v in after['layers'].items() if not isinstance(k, int)}
    assert {k:v for k,v in before.items() if k != 'layers'} == {
        k:v for k,v in after.items() if k != 'layers'}
    uses, names = {}, {}
    for i in indices:
        a, b = layers[i], after['layers'][i]
        assert {k:v for k,v in a.items() if k != 'params'} == {k:v for k,v in b.items() if k != 'params'}
        assert {k:v for k,v in a['params'].items() if k not in KEYS} == {
            k:v for k,v in b['params'].items() if k not in KEYS}
        for key in KEYS:
            old, new = a['params'].get(key), b['params'].get(key)
            assert (old is None) == (new is None)
            if old is None: continue
            assert names.setdefault(old, new) == new
            uses.setdefault(old, []).append(i)
        # Preserve every intra-operation read/write alias, not just output.
        for x in KEYS:
            for y in KEYS:
                assert (a['params'].get(x) == a['params'].get(y)) == (b['params'].get(x) == b['params'].get(y))
    for old, physical in names.items():
        for other, other_physical in names.items():
            if old == other or physical != other_physical: continue
            assert max(uses[old]) < min(uses[other]) or max(uses[other]) < min(uses[old])
        if old != physical:
            first = layers[min(uses[old])]
            assert first['type'] == 'OP' and first['params']['type'] == 'MAP'
            assert first['params']['output'] == old and first['params']['input'] != old
    # Symbolic execution: model unknown operations conservatively as reading
    # their old output; MAP is the only allowed full overwrite. The content
    # provenance of every input and final output must match at every step.
    a_state, b_state = {}, {}
    for i in indices:
        a, b = layers[i], after['layers'][i]
        for key in KEYS:
            old, new = a['params'].get(key), b['params'].get(key)
            if old is None: continue
            if key == 'output' and a['type'] == 'OP' and a['params']['type'] == 'MAP': continue
            assert a_state.get(old, 0) == b_state.get(new, 0), (i, key, old, new)
        old, new = a['params'].get('output'), b['params'].get('output')
        if old:
            a_state[old] = i
            b_state[new] = i
    for key in ('heightmapLayer', 'forestMap', 'assetsMap'):
        assert a_state.get(before.get(key), 0) == b_state.get(after.get(key), 0)
    return len(names), len(set(names.values()))

def main():
    counts = []
    for name in GENERATORS:
        for water in (0, 2, 4):
            for seed in (1, 35924):
                L = LuaRuntime(unpack_returned_tuples=True)
                L.globals().package.path = str(RES / 'scripts/?.lua').replace('\\','/') + ';' + L.globals().package.path
                L.execute('_=function(s) return s end; require "mathutil"')
                src = RES / 'config/terrain_generators' / (name + '.gen.lua')
                backup = src.with_name(src.name+'.bigmap-memory.bak')
                L.execute((backup if backup.exists() else src).read_text(encoding='utf-8'))
                info = L.globals().data()
                p = L.table_from({row['key']:row['defaultIndex'] for _,row in info.params.items()})
                p.water, p.mapSizeX, p.mapSizeY = water, 12288, 24576
                if name == 'desert' and water == 2 and seed == 35924:
                    p.mapSizeX, p.mapSizeY = 58368, 291840
                p.bounds = L.table_from(dict(min=L.table_from(dict(x=-p.mapSizeX/2,y=-p.mapSizeY/2)),
                                            max=L.table_from(dict(x=p.mapSizeX/2,y=p.mapSizeY/2))))
                L.globals().math.randomseed(seed)
                result = info.updateFn(p)
                before = native(result)
                optimizer = L.execute((ROOT/'mod/generation/bigmap_memory.lua').read_text())
                after = native(optimizer.Optimize(result))
                count = verify(before, after)
                counts.append((name, water, seed, *count))
    assert any(a > b for _,_,_,a,b in counts)
    print('PASS: 18 real pipeline combinations (defaults, water settings, seeds, 292 km case); parameters, lifetimes, aliases and symbolic contents preserved')
    for name in GENERATORS:
        print(name, sorted(set((a,b) for n,_,_,a,b in counts if n == name)))

    # Unknown operations and metadata references cannot accidentally reuse.
    L = LuaRuntime(unpack_returned_tuples=True)
    L.globals().opt = L.execute((ROOT/'mod/generation/bigmap_memory.lua').read_text())
    L.execute('''
      local r={heightmapLayer="HM", layers={
        {type="FEATURE",params={type="CONSTANT",output="__t_1"}},
        {type="OP",params={type="MAP",input="HM",output="__t_2"}}}}
      r.extra={nested="__t_1"}
      opt.Optimize(r); assert(r.layers[2].params.output=="__t_2")
      r.extra=nil; r.layers[2].type="UNKNOWN"
      opt.Optimize(r); assert(r.layers[2].params.output=="__t_2")
      r.layers[2].type="OP"; r.layers[2].params.type="MAD"
      opt.Optimize(r); assert(r.layers[2].params.output=="__t_2")
    ''')
    with tempfile.TemporaryDirectory() as td:
        res=Path(td)
        for name in GENERATORS:
            path=res/'config/terrain_generators'/(name+'.gen.lua')
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(b'function f()\r\n\t\treturn result\r\nend\r\n')
        assert install(res) == 7
        assert install(res) == 0
        assert install(res, True) == 3
        assert install(res, True) == 0
        assert install(res) == 3
        broken=res/'config/terrain_generators/tropical.gen.lua'
        broken.write_bytes(broken.read_bytes()+b'-- manual edit')
        snapshot={p:p.read_bytes() for p in res.rglob('*') if p.is_file()}
        try: install(res, True)
        except ValueError: pass
        else: raise AssertionError('modified generator not refused')
        assert snapshot == {p:p.read_bytes() for p in res.rglob('*') if p.is_file()}
    print('PASS: unknown schema/read-modify-write/pinned metadata guards; install, restore, idempotence and conflict preflight')

if __name__ == '__main__': main()
