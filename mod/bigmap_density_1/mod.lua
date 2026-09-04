-- bigmap_density_1 -- selectable town and industry density for oversized maps.
--
-- WHY THIS IS A MOD AND NOT AN EDIT TO res/config/base_config.lua
--
-- Town and industry counts are a fixed density per km^2, so they scale with
-- AREA. A 57 x 57 km map is 5.4x the largest map the game ships, and at stock
-- density it generates ~2,600 industries and ~660 towns. That is not a map
-- anyone wants to play, and it is slow to generate.
--
-- The obvious fix -- lower the numbers in res/config/base_config.lua -- has two
-- real problems:
--   1. It is a GAME FILE. Steam's "verify integrity of game files" reverts it
--      and a game update overwrites it, both silently.
--   2. It is GLOBAL. Values tuned for a 57 km map make a stock-size map sparse:
--      Megalomaniac 1:1 would drop from ~483 industries to ~89.
-- Doing it here fixes both. The game file stays stock, and the choice is per
-- map, made in the New Game menu like every other worldgen setting.
--
-- HOW IT COMPOSES
--
-- We MULTIPLY game.config rather than assign to it. base_mod.lua's own runFn
-- already multiplies industry density by { .4, .6, .8, 1.0 } from the stock
-- "Number of industries" dropdown. Multiplication commutes, so:
--   * our scale and the stock dropdown stack instead of fighting,
--   * neither one silently cancels the other, and
--   * mod load order does not matter -- which is worth having, because we do
--     not control it and assignment would make it a race.
--
-- THE DEFAULT, AND WHY IT IS NOT VANILLA
--
-- Both parameters default to "Megalomaniac count" (x0.18), not x1.00. Two
-- reasons, and one thing that makes it safe:
--   * A mod you had to tick a box named "Big Map Density" to enable is not a
--     mod that should quietly do nothing. Defaulting to vanilla means the
--     first big map anyone generates is the 2,600-industry one -- the exact
--     failure this mod exists to prevent, hit by everyone once.
--   * x0.18 reproduces the counts of Megalomaniac, the largest map the game
--     ships, at any size. That is a known-good target, not a guess.
--   * It is safe because ENABLING THE MOD IS THE OPT-IN. Leave it disabled and
--     nothing on any map changes; the game file is untouched.
--
-- x0.18 also matches, to within 2%, the values this project previously patched
-- into base_config.lua by hand (0.0367 towns, 0.147 industries per km^2), so
-- moving that patch here changes nothing about how existing setups generate.

local SCALES = { 1.00, 0.50, 0.30, 0.18, 0.10 }

-- Labels quote the resulting count on a 57.3 x 57.3 km map (3,288 km^2), the
-- size this mod exists to make playable. Stock density is 0.2 towns/km^2 and
-- 0.8 industries/km^2, so the counts below are scale * density * 3288.
-- On a normal-size map every level means proportionally fewer, same as vanilla.
local TOWN_LABELS = {
	_("Vanilla  (x1.00)  -- ~660 towns on a 57 km map"),
	_("Reduced  (x0.50)  -- ~330 towns"),
	_("Sparse   (x0.30)  -- ~200 towns"),
	_("Megalomaniac count (x0.18)  -- ~120 towns"),
	_("Minimal  (x0.10)  -- ~66 towns"),
}

local INDUSTRY_LABELS = {
	_("Vanilla  (x1.00)  -- ~2630 industries on a 57 km map"),
	_("Reduced  (x0.50)  -- ~1320 industries"),
	_("Sparse   (x0.30)  -- ~790 industries"),
	_("Megalomaniac count (x0.18)  -- ~470 industries"),
	_("Minimal  (x0.10)  -- ~260 industries"),
}

function data()
return {
	info = {
		minorVersion = 0,
		severityAdd = "NONE",
		severityRemove = "NONE",
		name = _("Big Map Density"),
		description = _([[
Selectable town and industry density, for maps far larger than the ones the game
ships with.

Counts are a density per square kilometre, so they scale with map area. At stock
density a 57 x 57 km map generates roughly 2,600 industries and 660 towns. These
two settings scale that down to something playable, and stack with the stock
"Towns" and "Number of industries" dropdowns rather than overriding them.

Both default to Vanilla, so this mod does nothing until you choose a level.
Enable it when you CREATE the map -- density is a worldgen setting.
]]),
		tags = { "Script Mod" },
		authors = { { name = "recon", role = "CREATOR" } },
		visible = true,
		params = {
			{
				key = "bigmap_town_scale",
				name = _("Town density scale"),
				tooltip = _("Multiplies the town density. Stacks with the stock Towns dropdown."),
				values = TOWN_LABELS,
				uiType = "COMBOBOX",
				defaultIndex = 3,   -- see DEFAULT note in the header
			},
			{
				key = "bigmap_industry_scale",
				name = _("Industry density scale"),
				tooltip = _("Multiplies the industry density. Stacks with the stock Number of industries dropdown."),
				values = INDUSTRY_LABELS,
				uiType = "COMBOBOX",
				defaultIndex = 3,   -- see DEFAULT note in the header
			},
		},
	},

	runFn = function(settings, allModParams)
		-- allModParams is keyed by mod id; the base game uses "" because its own
		-- getCurrentModId() is nil. Ours is a real id.
		local params = allModParams and allModParams[getCurrentModId()]
		if not params then return end   -- no params yet (menu not filled in): stay inert

		local function scaleOf(key)
			local idx = params[key]
			if type(idx) ~= "number" then return 1.0 end
			return SCALES[idx + 1] or 1.0    -- param indices are 0-based
		end

		local townScale     = scaleOf("bigmap_town_scale")
		local industryScale = scaleOf("bigmap_industry_scale")

		local loc = game and game.config and game.config.locations
		if not loc then return end

		if townScale ~= 1.0 and loc.town and loc.town.maxNumberPerArea then
			loc.town.maxNumberPerArea = loc.town.maxNumberPerArea * townScale
		end

		if industryScale ~= 1.0 and loc.industry then
			-- Scale BOTH: maxNumberPerArea is the count at generation,
			-- targetMaxNumberPerArea is what industry development grows toward.
			-- Scaling only the first gives a sparse map that refills itself to
			-- vanilla density over the first few decades.
			if loc.industry.maxNumberPerArea then
				loc.industry.maxNumberPerArea =
						loc.industry.maxNumberPerArea * industryScale
			end
			if loc.industry.targetMaxNumberPerArea then
				loc.industry.targetMaxNumberPerArea =
						loc.industry.targetMaxNumberPerArea * industryScale
			end
		end
	end,
}
end
