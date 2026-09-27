-- tpf2_bigmap: low-memory variants of the Fantasia Map Generator (workshop 2916150031).
-- Installed by tools/install_fantasia_low_memory.py; source in tpf2-bigmap/mod/generation.
function data()
return {
	info = {
		minorVersion = 0,
		severityAdd = "NONE",
		severityRemove = "NONE",
		name = _("Fantasia Map Generator (low memory)"),
		description = _([[For maps larger than 32 x 32 km, the Fantasia Map Generator (temperate, dry, tropical) lets temporary terrain buffers share memory, so generating a map needs 10 full-map buffers instead of 60 or more. Fantasia's own generator runs unchanged and makes the same map. Enable the Fantasia Map Generator mod as well, above this one in the list. Only matters while a new map is being generated.]]),
		tags = { "Terrain", "Generation", "Script Mod" },
		authors = {
			{ name = "tpf2_bigmap", role = "CREATOR" },
		},
		visible = true,
	},
}
end
