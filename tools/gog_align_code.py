"""Compare a Steam 35924 code region (bytes known from the plugin sources) with
the GOG 2024-12-12 exe at a given shift, instruction by instruction.

Every instruction must have the same mnemonic, length and non-relative bytes;
RIP-relative operands and call/jmp rel32 targets may differ, and each one gives
a Steam -> GOG address pair (code or data). Read-only.

    python tools/gog_align_code.py <GOG exe> <steam_rva> <gog_rva> <hex-or-@file>
"""
import sys
import capstone
import pefile

BASE = 0x140000000


def disasm(code, rva):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    return list(md.disasm(code, BASE + rva))


def refs(ins):
    """(kind, target rva, displacement offset in the instruction) for relative operands."""
    out = []
    for op in ins.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            out.append(('data', ins.address + ins.size + op.mem.disp - BASE, ins.disp_offset))
        elif op.type == capstone.x86.X86_OP_IMM and capstone.x86.X86_GRP_BRANCH_RELATIVE in ins.groups:
            out.append(('code', op.imm - BASE, ins.imm_offset))
    return out


def compare(pe, steam_rva, gog_rva, steam_code):
    gog_code = pe.get_data(gog_rva, len(steam_code))
    a, b = disasm(steam_code, steam_rva), disasm(gog_code, gog_rva)
    pairs, problems = {}, []
    for x, y in zip(a, b):
        if x.mnemonic != y.mnemonic or x.size != y.size:
            problems.append(f'{x.address - BASE:#x}: {x.mnemonic} {x.op_str} vs {y.mnemonic} {y.op_str}')
            break
        bx, by = bytearray(x.bytes), bytearray(y.bytes)
        rx, ry = refs(x), refs(y)
        for (k1, t1, off1), (k2, t2, off2) in zip(rx, ry):
            n = 4 if off1 + 4 <= len(bx) else len(bx) - off1
            bx[off1:off1 + n] = b'\0' * n; by[off2:off2 + n] = b'\0' * n
            if t1 in pairs and pairs[t1][1] != t2:
                problems.append(f'{t1:#x} maps to both {pairs[t1][1]:#x} and {t2:#x}')
            pairs[t1] = (k1, t2)
        if bx != by:
            problems.append(f'{x.address - BASE:#x}: bytes differ {x.bytes.hex()} vs {y.bytes.hex()}')
    covered = sum(i.size for i in a)
    return pairs, problems, covered


def fixups(pe, steam_rva, gog_rva, steam_code):
    """(disp offset, instruction end offset, GOG target) for every relative field
    whose bytes differ between the builds, offsets relative to the region start."""
    gog_code = pe.get_data(gog_rva, len(steam_code))
    out = []
    for x, y in zip(disasm(steam_code, steam_rva), disasm(gog_code, gog_rva)):
        for (_, _, off), (_, t2, _) in zip(refs(x), refs(y)):
            start = x.address - BASE - steam_rva
            if x.bytes[off:off + 4] != y.bytes[off:off + 4]:
                out.append((start + off, start + x.size, t2))
    return out


def main():
    emit = None
    if '--emit' in sys.argv:
        i = sys.argv.index('--emit'); emit = sys.argv[i + 1]; del sys.argv[i:i + 2]
    exe, s, g, h = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
    if h.startswith('@'):
        h = open(h[1:]).read().strip()
    code = bytes.fromhex(h)
    pe = pefile.PE(exe, fast_load=True)
    if emit:
        fx = fixups(pe, s, g, code)
        print(f'// {len(fx)} relative field(s) of Steam {s:#x} (GOG {g:#x}), from tools/gog_align_code.py')
        print(f'static const GogFixup {emit}[] = {{')
        for d, e, t in fx:
            print(f'    {{ {d:#06x}, {e:#06x}, {t:#09x} }},')
        print('};')
        return
    pairs, problems, covered = compare(pe, s, g, code)
    print(f'{covered} of {len(code)} bytes decoded; {len(problems)} problem(s)')
    for p in problems[:20]:
        print('  !', p)
    for t, (k, u) in sorted(pairs.items()):
        print(f'  {k:4} steam {t:#09x} -> gog {u:#09x} ({u - t:+#x})')


if __name__ == '__main__':
    main()
