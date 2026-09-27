-- tpf2_bigmap: low-memory variants of the Fantasia Map Generator (workshop 2916150031).
-- Installed by tools/install_fantasia_low_memory.py; source in tpf2-bigmap/mod/generation.
function data()
return {
	info = {
		minorVersion = 0,
		severityAdd = "NONE",
		severityRemove = "NONE",
		name = _("Fantasia Map Generator (low memory)"),
		description = _([[Adds a "(low memory)" copy of each Fantasia Map Generator (temperate, dry, tropical). It runs Fantasia's own generator unchanged, then lets temporary terrain buffers share memory, so generating a map needs 10 full-map buffers instead of 60 or more. Enable the Fantasia Map Generator mod as well. Only matters while a new map is being generated.]]),
		tags = { "Terrain", "Generation", "Script Mod" },
		authors = {
			{ name = "tpf2_bigmap", role = "CREATOR" },
		},
		visible = true,
	},
}
end
