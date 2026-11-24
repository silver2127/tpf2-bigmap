"""Regenerate mod/bigmap_fade_fix_1's shaders from an installed Transport Fever 2.

THE BUG (vanilla, every renderer). Five material vertex shaders fade an object
out by its distance to the camera, measured to the instance's bounding box:

    float distToCam = calcDistToBox(u_view.viewInverse[3].xyz, instAttrBbox);

When a pipeline does not supply that box, the attribute falls back to
defaultInstaAttrBBox = { vec3(0), vec3(0) } -- a point at the WORLD ORIGIN.
distToCam is then the camera's distance to the map centre, not to the object,
and streets (fade_out_range 10..20 km) fade by where the CAMERA is. With the
white layer (the game's layer 2, the grey world of the line manager) the street
pipeline hits that fallback: under OpenGL the streets whiten as the camera moves
away from the centre; under Vulkan they whiten too, and on AMD the
alpha-to-coverage dither turns the half-transparent street into multicoloured
speckles past ~23 km of camera distance.

THE FIX. With the default box (both corners zero) the distance is measured to
the vertex itself (std) or to the leafcard centre (leafcard). Objects that do
supply a box are unchanged.

Every anchor must match exactly once, or nothing is written.

    python tools/build_fade_fix_mod.py [--game <game folder>] [--out <mod folder>]

Only res/shaders2 is written; mod.lua and strings.lua are kept by hand. See
docs/street-fade-fix.md and tools/test_fade_fix_mod.py.
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANCHOR = 'float distToCam = calcDistToBox(u_view.viewInverse[3].xyz, instAttrBbox);'
NO_BOX = '(instAttrBbox[0] == vec3(.0) && instAttrBbox[1] == vec3(.0))'
COMMENT = ('\t// tpf2-bigmap fade fix: without an instance box (both corners zero) stock measured the\n'
           '\t// camera\'s distance to the world origin; measure it to this object instead.\n')
TO_VERTEX = 'distance(u_view.viewInverse[3].xyz, (instAttrModel * vec4(attrPosition.xyz, 1.0)).xyz)'
TO_CENTER = 'distance(u_view.viewInverse[3].xyz, centerWorld.xyz)'
FILES = {
    'mat/vs/std/normal.vs': TO_VERTEX,
    'mat/vs/std/color.vs': TO_VERTEX,
    'mat/vs/std/color_diffuse.vs': TO_VERTEX,
    'mat/vs/leafcard/normal.vs': TO_CENTER,
    'mat/vs/leafcard/color.vs': TO_CENTER,
}


def patch(text, rel, dist):
    if text.count(ANCHOR) != 1:
        sys.exit(f'{rel}: anchor found {text.count(ANCHOR)} time(s), expected 1 -- nothing written')
    line = next(l for l in text.splitlines() if ANCHOR in l)
    indent = line[:len(line) - len(line.lstrip())]
    if dist == TO_CENTER and not (0 <= text.find('vec4 centerWorld') < text.find(ANCHOR)):
        sys.exit(f'{rel}: centerWorld is not computed before the distance -- nothing written')
    new = (COMMENT + f'{indent}float distToCam = {NO_BOX}\n'
           f'{indent}\t? {dist}\n'
           f'{indent}\t: calcDistToBox(u_view.viewInverse[3].xyz, instAttrBbox);')
    return text.replace(line, new, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=r'C:\Games\Transport Fever 2')
    ap.add_argument('--out', default=str(ROOT / 'mod' / 'bigmap_fade_fix_1'))
    args = ap.parse_args()
    src = Path(args.game) / 'res' / 'shaders2'
    patched = {rel: patch(((src / rel).read_text(encoding='utf-8')), rel, dist) for rel, dist in FILES.items()}
    for rel, text in patched.items():
        dst = Path(args.out) / 'res' / 'shaders2' / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(text, encoding='utf-8', newline='\n')
    print(f'wrote {len(patched)} shader(s) into {args.out}')


if __name__ == '__main__':
    main()
