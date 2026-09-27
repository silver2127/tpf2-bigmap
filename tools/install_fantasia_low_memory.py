"""Install/remove the Fantasia Map Generator (low memory) mod.

Copies mod/generation/fantasia_low_memory plus mod/generation/bigmap_memory.lua
into <game>/mods/tpf2_bigmap_fantasia_low_memory_1. The Fantasia workshop mod
itself is never read or written here; the variants load it at generation time.
Refuses to touch a folder that does not hold this mod's mod.lua.
"""
import argparse
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'mod/generation/fantasia_low_memory'
MEMORY = ROOT / 'mod/generation/bigmap_memory.lua'
FOLDER = 'tpf2_bigmap_fantasia_low_memory_1'
MARKER = b'tpf2_bigmap: low-memory variants of the Fantasia Map Generator'


def files():
    """Published relative path -> source bytes."""
    out = {p.relative_to(SOURCE).as_posix(): p.read_bytes() for p in SOURCE.rglob('*') if p.is_file()}
    out['res/scripts/terrain/bigmap_memory.lua'] = MEMORY.read_bytes()
    return out


def ours(target):
    mod = target / 'mod.lua'
    return mod.is_file() and MARKER in mod.read_bytes()


def install(mods, remove=False):
    target = mods / FOLDER
    if target.exists() and not ours(target):
        raise ValueError(f'{target}: exists and is not this mod; refusing to touch it')
    if remove:
        if not target.exists():
            return 0
        shutil.rmtree(target)
        return 1
    wanted = files()
    changes = 0
    for rel, data in wanted.items():
        path = target / rel
        if path.is_file() and path.read_bytes() == data:
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        changes += 1
    for path in sorted(target.rglob('*'), reverse=True):
        rel = path.relative_to(target).as_posix()
        if path.is_file() and rel not in wanted:
            path.unlink()
            changes += 1
        elif path.is_dir() and not any(path.iterdir()):
            path.rmdir()
    return changes


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--mods', type=Path, default=Path(
        r'C:\Program Files (x86)\Steam\steamapps\common\Transport Fever 2\mods'))
    ap.add_argument('--remove', action='store_true')
    args = ap.parse_args()
    count = install(args.mods, args.remove)
    print(f'{"Removed" if args.remove else "Installed"} {FOLDER}: {count} file changes')
