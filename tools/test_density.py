"""Offline test for mod/bigmap_density_1/mod.lua.

Runs the mod's real runFn under lupa with the game globals stubbed, so the
density maths is checked without launching Transport Fever 2. Verifies:
  * every level multiplies town, industry and industry TARGET density correctly
    (scaling only the first would give a sparse map that refills itself to
    vanilla density over the first few decades)
  * the three defensive paths stay inert instead of crashing: no allModParams,
    our key absent, and an out-of-range param index

Needs `pip install lupa`. Exits non-zero on any mismatch.
"""
from pathlib import Path

import lupa, sys
L = lupa.LuaRuntime(unpack_returned_tuples=True)
L.execute('function _(s) return s end')
L.execute('function getCurrentModId() return "bigmap_density_1" end')
src = (Path(__file__).resolve().parents[1] / 'mod' / 'bigmap_density_1' / 'mod.lua').read_text(encoding='utf-8')
L.execute(src)

def fresh_game():
    L.execute('game = { config = { locations = { town = { maxNumberPerArea = 0.2 }, '
              'industry = { maxNumberPerArea = 0.8, targetMaxNumberPerArea = 0.8 } } }')

d = L.globals().data()
info = d['info']
params = info['params']
print("params declared:", [params[i]['key'] for i in range(1, len(params)+1)])
print("defaults      :", [params[i]['defaultIndex'] for i in range(1, len(params)+1)])
print("town labels   :", len(params[1]['values']), "industry labels:", len(params[2]['values']))

runFn = d['runFn']
SCALES = [1.00, 0.50, 0.30, 0.18, 0.10, 0.046, 0.022]
ok = True
for idx, sc in enumerate(SCALES):
    fresh_game()
    amp = L.table_from({"bigmap_density_1": L.table_from(
        {"bigmap_town_scale": idx, "bigmap_industry_scale": idx})})
    runFn(None, amp)
    loc = L.globals().game.config.locations
    t, i, tg = loc.town.maxNumberPerArea, loc.industry.maxNumberPerArea, loc.industry.targetMaxNumberPerArea
    exp = (round(0.2*sc,6), round(0.8*sc,6), round(0.8*sc,6))
    got = (round(t,6), round(i,6), round(tg,6))
    good = exp == got
    ok &= good
    print(f"  idx={idx} x{sc:<5} town={t:.4f} ind={i:.4f} tgt={tg:.4f}  {'OK' if good else 'MISMATCH exp='+str(exp)}")

# guards
fresh_game(); runFn(None, None)
g1 = L.globals().game.config.locations.town.maxNumberPerArea
print("nil allModParams -> no crash, town =", g1); ok &= (g1 == 0.2)

fresh_game(); runFn(None, L.table_from({"other_mod": L.table_from({"x":1})}))
g2 = L.globals().game.config.locations.town.maxNumberPerArea
print("no params anywhere -> inert, town =", g2); ok &= (g2 == 0.2)

# The reason this test exists: runFn used to call getCurrentModId(), which the
# loader redefines per-mod at LOAD time, so at world-generation time it names
# whichever mod loaded last. The mod must still find its own params when the id
# it was given does not match the key the engine used.
fresh_game()
runFn(None, L.table_from({"some_other_mod_that_loaded_last": L.table_from(
    {"bigmap_town_scale": 4, "bigmap_industry_scale": 4})}))
g3 = L.globals().game.config.locations.town.maxNumberPerArea
good3 = abs(g3 - 0.2*0.10) < 1e-9
ok &= good3
print(f"params under a DIFFERENT id -> fallback finds them, town = {g3:.4f} "
      f"({'OK' if good3 else 'MISMATCH exp 0.0200'})")
fresh_game()
runFn(None, L.table_from({"bigmap_density_1": L.table_from({"bigmap_town_scale": 99})}))
print("out-of-range idx -> falls back to 1.0, town =", L.globals().game.config.locations.town.maxNumberPerArea)
print("\nRESULT:", "ALL PASS" if ok else "FAILURES")
sys.exit(0 if ok else 1)
