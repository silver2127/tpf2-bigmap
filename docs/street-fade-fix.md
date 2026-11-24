# Street fade fix (`mod/bigmap_fade_fix_1`)

A small shader mod, separate from the plugin. It fixes streets (and some other
objects) whitening -- or, under Vulkan on AMD, turning into multicoloured
speckles -- when the camera is far from the map centre. It is most visible with
the line manager's grey layer (CommonAPI calls it the "white layer"), and it
happens on vanilla Megalomaniac maps as well as on bigger ones, so the plugin
cannot be what causes it.

| Renderer | Symptom (camera far from the centre, grey layer on) |
| --- | --- |
| OpenGL | streets whiten progressively from ~10 km of camera distance |
| Vulkan | streets whiten too; on AMD, past ~23 km they become coloured speckles |

## Cause

Five material vertex shaders fade an object out by the camera's distance to the
object's bounding box:

```glsl
float distToCam = calcDistToBox(u_view.viewInverse[3].xyz, instAttrBbox);
float alpha = compFadeOutAlpha(distToCam, u_vmat.fadeOutRanges[0] * ...);
```

When the renderer supplies no box, `instAttrBbox` falls back to
`defaultInstaAttrBBox = { vec3(0), vec3(0) }` -- a single point at the **map
centre**. The distance is then the camera's distance to the centre, not to the
object. Street materials fade out between 10 and 20 km (`fade_out_range`), so in
that pipeline every street fades by where the camera is. OpenGL blends the
half-transparent street over the white ground; Vulkan on AMD renders the
transparency with alpha-to-coverage, whose dither shows up as speckles.

Measured on a 57 km map: the speckle boundary is a circle of ~23,000 m around
the centre, and it moves with the zoom, because the camera *eye* is what is
measured (at a camera height pinned by CommonAPI's goto, the artefacts began at
x <= -19,929 and x >= 19,931; zooming in made them disappear).

## Fix

When both corners of the box are zero (no box), the distance is measured to the
vertex itself (the `std` shaders) or to the leafcard centre (the `leafcard`
shaders). Objects that do supply a box behave exactly as before.

Replaced files, all in `res/shaders2/mat/vs/`: `std/normal.vs`, `std/color.vs`,
`std/color_diffuse.vs`, `leafcard/normal.vs`, `leafcard/color.vs`. Each differs
from the game's file by one statement.

Purely visual and client-side: no save data, safe to add or remove at any time,
nothing needed on other multiplayer peers.

## Install

The Big Maps MSI (both the standard and the GOG package) installs it, with the
plugin, into the game's own mods folder: `<game>\mods\bigmap_fade_fix_1\`
(`mod.lua`, `strings.lua` and the five shaders). **Enabling it is still your
step**: it appears as "Big Maps: street fade fix" in the game's mod list, and a
mod is switched on per game, like any other. Uninstalling Big Maps removes
exactly those files and leaves the rest of the mods folder (Urban Games' campaign
missions live there) alone.

Without the MSI, copy `mod\bigmap_fade_fix_1` into
`%APPDATA%\Transport Fever 2\mods\` or into `<game>\mods\`. Do not keep both
copies: the game would list the same mod twice.

The mod's description is translated into 16 languages (`strings.lua`); apart from
English and French they are machine-written and not reviewed by native speakers.
The same mod is also published on its own, with its history, at
<https://github.com/Kalemillion/tpf2-street-fade-fix>.

## Rebuilding the shaders

The shipped shaders are the game's own files with one statement changed. To
regenerate them from an installation (for example after a game update):

```
python tools\build_fade_fix_mod.py --game "<Transport Fever 2 folder>"
```

The script requires each anchor to match exactly once and writes nothing
otherwise, so a game update that moves the line stops it instead of producing a
wrong shader. Only `res/shaders2` is written; `mod.lua` and `strings.lua` are
kept by hand. The shipped copies were generated from the GOG 2024-12-12 install.

`python tools\test_fade_fix_mod.py` checks that each shader holds the patched
distance once and no longer the stock statement, that undoing the patch gives
back the stock line, and (with a game install, `TPF2_GAME` or the default
folders) that regenerating gives the committed files byte for byte and that undo
recovers the installed shaders exactly. It loads `mod.lua` and `strings.lua`
with a Lua interpreter (`lua` on PATH, or `LUA=`) and checks the 16 languages.

## Verification

On the GOG build under Vulkan on an AMD GPU, with the 224 x 224 save: the
artefacts and the whitening are gone at every camera position that showed them.
Not checked here: NVIDIA/Intel Vulkan, and the Steam build.

## Licence

`tools/build_fade_fix_mod.py`, `mod.lua` and `strings.lua`: the repository's
licence. The shader files are Urban Games' shaders, redistributed modified as is
customary for Transport Fever 2 shader mods; all rights to the original code
remain theirs.
