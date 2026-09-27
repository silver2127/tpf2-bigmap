-- Fantasia Map Generator with tpf2_bigmap terrain buffer reuse; see terrain/bigmap_wrap.lua.
local wrap = require "terrain/bigmap_wrap"

function data()
	return wrap.Wrap("fantasia_map_generator.gen.lua", "terrain/fmg_mapgenutil", "temperate.clima.lua")
end
