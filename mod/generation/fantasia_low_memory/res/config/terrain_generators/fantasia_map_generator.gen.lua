-- tpf2_bigmap: stands in for the Fantasia Map Generator's file of the same name. It runs
-- Fantasia's own file, with buffer reuse for maps over 32 x 32 km; see terrain/bigmap_wrap.lua.
local wrap = require "terrain/bigmap_wrap"

function data()
	return wrap.Wrap("fantasia_map_generator.gen.lua", "terrain/fmg_mapgenutil", "temperate.clima.lua", 32768 * 32768)
end
