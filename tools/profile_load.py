"""Sampling profiler for a Transport Fever 2 load (Steam 35924 or GOG 2024-12-12).

Attaches to a running TransportFever2.exe without patching it: about every
10 ms it reads the instruction pointer of each thread that used CPU since the
last round (SuspendThread / GetThreadContext / ResumeThread), and scans the
top of that thread's stack for return addresses into the exe to estimate
inclusive time. Samples are attributed to functions through the exe's own
.pdata table, grouped in windows, and functions the plugin already knows are
named (GOG addresses are translated back to the Steam ones the docs use).

Usage (start it before loading the save, stop with Ctrl+C once the world moves,
or give --seconds):
    python tools/profile_load.py [--exe <path>] [--seconds N] [--window 10] [--out report.txt]

Needs pefile. Reads the game's memory; writes only the report.
"""
import argparse
import bisect
import collections
import ctypes as C
import ctypes.wintypes as W
import json
import struct
import sys
import time
from pathlib import Path

import pefile

k32 = C.WinDLL('kernel32', use_last_error=True)
psapi = C.WinDLL('psapi', use_last_error=True)

TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x400, 0x10
THREAD_SUSPEND_RESUME, THREAD_GET_CONTEXT, THREAD_QUERY_INFORMATION = 0x2, 0x8, 0x40
CONTEXT_AMD64, CONTEXT_CONTROL, CONTEXT_INTEGER = 0x100000, 0x1, 0x2


class PROCESSENTRY32W(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ProcessID', W.DWORD),
                ('th32DefaultHeapID', C.c_void_p), ('th32ModuleID', W.DWORD), ('cntThreads', W.DWORD),
                ('th32ParentProcessID', W.DWORD), ('pcPriClassBase', C.c_long), ('dwFlags', W.DWORD),
                ('szExeFile', C.c_wchar * 260)]


class THREADENTRY32(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD),
                ('th32OwnerProcessID', W.DWORD), ('tpBasePri', C.c_long), ('tpDeltaPri', C.c_long),
                ('dwFlags', W.DWORD)]


class M128A(C.Structure):
    _fields_ = [('Low', C.c_uint64), ('High', C.c_int64)]


class CONTEXT(C.Structure):
    _pack_ = 16
    _fields_ = [('P1Home', C.c_uint64), ('P2Home', C.c_uint64), ('P3Home', C.c_uint64),
                ('P4Home', C.c_uint64), ('P5Home', C.c_uint64), ('P6Home', C.c_uint64),
                ('ContextFlags', W.DWORD), ('MxCsr', W.DWORD),
                ('SegCs', W.WORD), ('SegDs', W.WORD), ('SegEs', W.WORD), ('SegFs', W.WORD),
                ('SegGs', W.WORD), ('SegSs', W.WORD), ('EFlags', W.DWORD),
                ('Dr0', C.c_uint64), ('Dr1', C.c_uint64), ('Dr2', C.c_uint64), ('Dr3', C.c_uint64),
                ('Dr6', C.c_uint64), ('Dr7', C.c_uint64),
                ('Rax', C.c_uint64), ('Rcx', C.c_uint64), ('Rdx', C.c_uint64), ('Rbx', C.c_uint64),
                ('Rsp', C.c_uint64), ('Rbp', C.c_uint64), ('Rsi', C.c_uint64), ('Rdi', C.c_uint64),
                ('R8', C.c_uint64), ('R9', C.c_uint64), ('R10', C.c_uint64), ('R11', C.c_uint64),
                ('R12', C.c_uint64), ('R13', C.c_uint64), ('R14', C.c_uint64), ('R15', C.c_uint64),
                ('Rip', C.c_uint64),
                ('FltSave', C.c_byte * 512),
                ('VectorRegister', M128A * 26), ('VectorControl', C.c_uint64),
                ('DebugControl', C.c_uint64), ('LastBranchToRip', C.c_uint64),
                ('LastBranchFromRip', C.c_uint64), ('LastExceptionToRip', C.c_uint64),
                ('LastExceptionFromRip', C.c_uint64)]


class ADDRESS64(C.Structure):
    _fields_ = [('Offset', C.c_uint64), ('Segment', W.WORD), ('Mode', W.DWORD)]


class KDHELP64(C.Structure):
    _fields_ = [('Thread', C.c_uint64), ('ThCallbackStack', W.DWORD), ('ThCallbackBStore', W.DWORD),
                ('NextCallback', W.DWORD), ('FramePointer', W.DWORD), ('KiCallUserMode', C.c_uint64),
                ('KeUserCallbackDispatcher', C.c_uint64), ('SystemRangeStart', C.c_uint64),
                ('KiUserExceptionDispatcher', C.c_uint64), ('StackBase', C.c_uint64), ('StackLimit', C.c_uint64),
                ('BuildVersion', W.DWORD), ('RetpolineStubFunctionTableSize', W.DWORD),
                ('RetpolineStubFunctionTable', C.c_uint64), ('RetpolineStubOffset', W.DWORD),
                ('RetpolineStubSize', W.DWORD), ('Reserved0', C.c_uint64 * 2)]


class STACKFRAME64(C.Structure):
    _fields_ = [('AddrPC', ADDRESS64), ('AddrReturn', ADDRESS64), ('AddrFrame', ADDRESS64),
                ('AddrStack', ADDRESS64), ('AddrBStore', ADDRESS64), ('FuncTableEntry', C.c_void_p),
                ('Params', C.c_uint64 * 4), ('Far', W.BOOL), ('Virtual', W.BOOL),
                ('Reserved', C.c_uint64 * 3), ('KdHelp', KDHELP64)]


dbghelp = C.WinDLL('dbghelp', use_last_error=True)
dbghelp.StackWalk64.argtypes = [W.DWORD, W.HANDLE, W.HANDLE, C.POINTER(STACKFRAME64), C.c_void_p,
                                C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
dbghelp.SymInitialize.argtypes = [W.HANDLE, C.c_wchar_p, W.BOOL]
IMAGE_FILE_MACHINE_AMD64 = 0x8664


def unwind(hproc, hthread, ctx, max_frames):
    """Return addresses of a suspended thread, innermost first (rip included)."""
    c = CONTEXT.from_buffer_copy(bytes(ctx))
    f = STACKFRAME64()
    f.AddrPC.Offset, f.AddrPC.Mode = c.Rip, 3
    f.AddrStack.Offset, f.AddrStack.Mode = c.Rsp, 3
    f.AddrFrame.Offset, f.AddrFrame.Mode = c.Rbp, 3
    out = []
    fta = C.cast(dbghelp.SymFunctionTableAccess64, C.c_void_p)
    gmb = C.cast(dbghelp.SymGetModuleBase64, C.c_void_p)
    while len(out) < max_frames and dbghelp.StackWalk64(IMAGE_FILE_MACHINE_AMD64, hproc, hthread, C.byref(f),
                                                        C.byref(c), None, fta, gmb, None):
        if not f.AddrPC.Offset:
            break
        out.append(f.AddrPC.Offset)
    return out


k32.OpenThread.restype = W.HANDLE
k32.OpenProcess.restype = W.HANDLE
k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.SuspendThread.restype = W.DWORD
k32.ResumeThread.restype = W.DWORD
k32.GetThreadContext.argtypes = [W.HANDLE, C.POINTER(CONTEXT)]
k32.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
k32.GetThreadTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4

# Functions named in the plugin's sources and docs, by STEAM 35924 RVA.
KNOWN = {
    0x157390: 'InitNewGame (world entry)', 0x230440: 'world allocation',
    0x2e5ec0: 'LoadGame', 0x2e97c0: 'SaveGame',
    0x30a540: 'terrain block copy', 0x30a55c: 'terrain block copy (loop)',
    0x310230: 'terrain block ctor', 0x315f20: 'material index loop',
    0x32a6f0: 'material grid dtor', 0x330350: 'material intern destroy',
    0x33cb60: 'terrain AddTile', 0x33cd10: 'terrain cache publish (CalcMinMaxHeight)',
    0x33d330: 'terrain level', 0x33d7c0: 'terrain vertices', 0x33da70: 'terrain bind',
    0x33de30: 'terrain destroy', 0x347090: 'terrain upload',
    0x3ac6c0: 'bicubic terrain refine', 0x3b3470: 'CalculateHeightMod (terrain alignment)',
    0x3b8660: 'scenery assets', 0x3b8bd0: 'trees', 0x3bc9e0: 'terrain publication',
    0x3bcb90: 'terrain preparation', 0x3c4620: 'terrain refine (cache)',
    0x1d5830: 'material resize', 0x1d5c50: 'terrain resize', 0x1dedd0: 'terrain copy',
    0x1b3d90: 'material assign', 0x112210: 'GetTerrainComponent',
    0x674aa0: 'GetNumTilesNew', 0x90d410: 'street occupancy ctor',
    0x937590: 'town/industry road connections', 0xaad6c0: 'terrain alignment update',
}
# Steam -> GOG shift by region (tools/gog_map_sites.py): (steam_lo, steam_hi, delta).
GOG_REGIONS = [(0x090000, 0x0c0000, 0x210), (0x100000, 0x700000, 0x220),
               (0x800000, 0xb00000, 0x0f0), (0x2320000, 0x2330000, 0x2b30)]
GOG_STAMP = 0x675ad7cc
NAMED_DLLS = {'ntdll.dll', 'kernelbase.dll', 'kernel32.dll', 'ucrtbase.dll', 'vcruntime140.dll',
              'msvcp140.dll', 'win32u.dll'}


def map_symbols(path):
    """(sorted RVAs, names) of the functions in an MSVC linker .map file."""
    dbghelp = C.WinDLL('dbghelp')
    base, rows = 0x180000000, []
    for line in Path(path).read_text(errors='replace').splitlines():
        if 'Preferred load address is' in line:
            base = int(line.split()[-1], 16)
        parts = line.split()
        if len(parts) >= 4 and parts[0].startswith('0001:') and ' f ' in f' {line} ':
            try:
                rva = int(parts[2], 16) - base
            except ValueError:
                continue
            out = C.create_string_buffer(512)
            dbghelp.UnDecorateSymbolName(parts[1].encode(), out, 512, 0x1000)   # UNDNAME_NAME_ONLY
            rows.append((rva, out.value.decode(errors='replace') or parts[1]))
    rows.sort()
    return ([r[0] for r in rows], [r[1] for r in rows]) if rows else None


def enum_processes(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W(); e.dwSize = C.sizeof(e)
    ok = k32.Process32FirstW(snap, C.byref(e)); out = []
    while ok:
        if e.szExeFile.lower() == name.lower():
            out.append(e.th32ProcessID)
        ok = k32.Process32NextW(snap, C.byref(e))
    k32.CloseHandle(snap)
    return out


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    t = THREADENTRY32(); t.dwSize = C.sizeof(t)
    ok = k32.Thread32First(snap, C.byref(t)); out = []
    while ok:
        if t.th32OwnerProcessID == pid:
            out.append(t.th32ThreadID)
        ok = k32.Thread32Next(snap, C.byref(t))
    k32.CloseHandle(snap)
    return out


def modules_of(hproc):
    arr = (W.HMODULE * 1024)(); need = W.DWORD()
    psapi.EnumProcessModulesEx(hproc, arr, C.sizeof(arr), C.byref(need), 3)
    out = []
    for i in range(need.value // C.sizeof(W.HMODULE)):
        name = C.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(hproc, W.HMODULE(arr[i]), name, 260)

        class MI(C.Structure):
            _fields_ = [('base', C.c_void_p), ('size', W.DWORD), ('entry', C.c_void_p)]
        mi = MI()
        psapi.GetModuleInformation(hproc, W.HMODULE(arr[i]), C.byref(mi), C.sizeof(mi))
        out.append((mi.base or 0, mi.size, name.value))
    out.sort()
    return out


class Functions:
    """Function ranges of the exe from its .pdata (RUNTIME_FUNCTION) table."""

    def __init__(self, exe):
        pe = pefile.PE(exe, fast_load=True)
        self.stamp = pe.FILE_HEADER.TimeDateStamp
        self.gog = self.stamp == GOG_STAMP
        d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        raw = pe.get_data(d.VirtualAddress, d.Size)
        starts, ends = [], []
        for i in range(0, len(raw) - 11, 12):
            b, e, _ = struct.unpack_from('<III', raw, i)
            if b:
                starts.append(b); ends.append(e)
        order = sorted(range(len(starts)), key=starts.__getitem__)
        self.starts = [starts[i] for i in order]; self.ends = [ends[i] for i in order]
        text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
        self.text_lo, self.text_hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
        self.text = text.get_data()

    def func(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and rva < self.ends[i]:
            return self.starts[i]
        return None

    def after_call(self, rva):
        """True when the bytes before rva end with a call instruction."""
        off = rva - self.text_lo
        if off < 6 or rva >= self.text_hi:
            return False
        b = self.text
        return (b[off - 5] == 0xE8 or (b[off - 6] == 0xFF and (b[off - 5] & 0x38) == 0x10)
                or (b[off - 2] == 0xFF and (b[off - 1] & 0x38) == 0x10)
                or (b[off - 3] == 0xFF and (b[off - 2] & 0x38) == 0x10))

    def steam(self, rva):
        if not self.gog:
            return rva
        for lo, hi, d in GOG_REGIONS:
            if lo + d <= rva < hi + d:
                return rva - d
        return None

    def name(self, rva):
        s = self.steam(rva)
        label = KNOWN.get(s) if s is not None else None
        tag = f'exe+{rva:#x}' + (f' (steam {s:#x})' if self.gog and s is not None else '')
        return f'{tag} {label}' if label else tag


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--exe', default=None, help='path of the running TransportFever2.exe (default: from the process)')
    ap.add_argument('--seconds', type=float, default=0, help='stop after N seconds (default: Ctrl+C)')
    ap.add_argument('--window', type=float, default=10.0)
    ap.add_argument('--interval', type=float, default=0.01)
    ap.add_argument('--stack', type=int, default=4096, help='stack bytes scanned for return addresses')
    ap.add_argument('--out', default='profile_load_report.txt')
    ap.add_argument('--frames', type=int, default=40, help='frames unwound per sample (0 = stack scan only)')
    ap.add_argument('--process', default='TransportFever2.exe', help='process name to attach to')
    ap.add_argument('--pid', type=int, default=0, help='attach to this process id instead')
    ap.add_argument('--map', default=str(Path(__file__).resolve().parents[1] / 'out' / 'tpf2_bigmap.map'),
                    help='linker map of the running tpf2_bigmap.dll, to name its functions')
    args = ap.parse_args()

    print(f'waiting for {args.process} ...', flush=True)
    while True:
        pids = [args.pid] if args.pid else enum_processes(args.process)
        if pids:
            break
        time.sleep(0.5)
    pid = pids[0]
    hproc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not hproc:
        sys.exit(f'OpenProcess failed: {C.get_last_error()}')
    # A process caught at startup has no readable module list yet: retry.
    exe_mod = None
    for _ in range(300):
        mods = modules_of(hproc)
        image = C.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(hproc, None, image, 260)
        exe_mod = next((m for m in mods if image.value and m[2].lower() == image.value.lower()), None)
        if exe_mod:
            break
        time.sleep(0.1)
    if not exe_mod:
        sys.exit('the exe module never became readable')
    exe_base, exe_size = exe_mod[0], exe_mod[1]
    exe_path = args.exe
    if not exe_path:
        buf = C.create_unicode_buffer(1024)
        psapi.GetModuleFileNameExW(hproc, None, buf, 1024)
        exe_path = buf.value
    fn = Functions(exe_path)
    if args.frames:
        dbghelp.SymSetOptions(0x2 | 0x4)   # undecorated names, deferred loads: unwind tables only, no symbols
        if not dbghelp.SymInitialize(hproc, None, True):
            print('SymInitialize failed; falling back to the stack scan'); args.frames = 0
    print(f'pid {pid}, exe {exe_path} at {exe_base:#x} ({"GOG" if fn.gog else "Steam/other"} build), '
          f'{len(fn.starts)} functions; sampling every {args.interval * 1000:.0f} ms -- Ctrl+C to stop', flush=True)
    mod_starts = [m[0] for m in mods]

    # System and runtime DLLs are named by their nearest export, read from the
    # same file on disk the process mapped (ntdll!RtlAllocateHeap and so on).
    export_cache = {}

    def exports_of(name):
        if name in export_cache:
            return export_cache[name]
        table = None
        if name.lower() == 'tpf2_bigmap.dll' and args.map and Path(args.map).exists():
            table = map_symbols(args.map)
        elif name.lower() in NAMED_DLLS:
            path = C.create_unicode_buffer(1024)
            base_mod = next((m for m in mods if m[2] == name), None)
            if base_mod and psapi.GetModuleFileNameExW(hproc, W.HMODULE(base_mod[0]), path, 1024):
                try:
                    pe = pefile.PE(path.value, fast_load=True)
                    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT']])
                    rows = sorted((e.address, (e.name or b'#%d' % e.ordinal).decode(errors='replace'))
                                  for e in getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None).symbols if e.address)
                    table = ([r[0] for r in rows], [r[1] for r in rows])
                except Exception:
                    table = None
        export_cache[name] = table
        return table

    def where(addr):
        if exe_base <= addr < exe_base + exe_size:
            r = addr - exe_base
            f = fn.func(r)
            return ('exe', f if f is not None else r)
        i = bisect.bisect_right(mod_starts, addr) - 1
        if i >= 0 and addr < mods[i][0] + mods[i][1]:
            name = mods[i][2]
            table = exports_of(name)
            if table:
                j = bisect.bisect_right(table[0], addr - mods[i][0]) - 1
                if j >= 0:
                    off = addr - mods[i][0] - table[0][j]
                    return (f'{name}!{table[1][j]}+{off:#x}', None)
            return (name, None)
        return ('?', None)

    handles, last_cpu = {}, {}
    windows = collections.defaultdict(lambda: collections.Counter())
    win_incl = collections.defaultdict(lambda: collections.Counter())
    win_threads = collections.defaultdict(lambda: collections.Counter())
    total_self, total_incl = collections.Counter(), collections.Counter()
    callers = collections.defaultdict(lambda: collections.Counter())
    stacks = collections.defaultdict(lambda: collections.Counter())
    total_callers = collections.Counter()
    ctx = CONTEXT(); ctx.ContextFlags = CONTEXT_AMD64 | CONTEXT_CONTROL | CONTEXT_INTEGER
    stackbuf = (C.c_ubyte * args.stack)()
    t0 = time.perf_counter(); rounds = 0; samples = 0
    def write_report():
        elapsed = time.perf_counter() - t0
        lines = [f'profile of pid {pid} ({"GOG" if fn.gog else "Steam/other"}), {elapsed:.1f} s, {samples} samples, '
                 f'window {args.window:.0f} s, interval {args.interval * 1000:.0f} ms',
                 'self = the sampled instruction is in that function; incl = the function is also on the scanned stack', '']
        lines.append('== whole run: self (top 40)')
        for k, v in total_self.most_common(40):
            lines.append(f'{v:7d} {100.0 * v / max(1, samples):5.1f}%  {k}')
        lines.append(''); lines.append('== whole run: samples outside the exe, by the first game function on the stack (top 30)')
        for k, v in total_callers.most_common(30):
            lines.append(f'{v:7d} {100.0 * v / max(1, samples):5.1f}%  {k}')
        lines.append(''); lines.append('== whole run: inclusive (top 40)')
        for k, v in total_incl.most_common(40):
            lines.append(f'{v:7d} {100.0 * v / max(1, samples):5.1f}%  {k}')
        for w in sorted(windows):
            n = sum(windows[w].values())
            busy = win_threads[w].most_common(4)
            lines.append(''); lines.append(f'== t+{w * args.window:.0f}..{(w + 1) * args.window:.0f} s: {n} samples, '
                                           f'busiest threads ' + ', '.join(f'{t}:{c}' for t, c in busy))
            for k, v in windows[w].most_common(8):
                lines.append(f'   self {v:6d} {100.0 * v / max(1, n):5.1f}%  {k}')
            for k, v in win_incl[w].most_common(8):
                lines.append(f'   incl {v:6d} {100.0 * v / max(1, n):5.1f}%  {k}')
            for k, v in callers[w].most_common(4):
                lines.append(f'   via  {v:6d} {100.0 * v / max(1, n):5.1f}%  {k}')
            for k, v in stacks[w].most_common(5):
                lines.append(f'   stk  {v:6d} {100.0 * v / max(1, n):5.1f}%  {k}')
        Path(args.out).write_text('\n'.join(lines) + '\n', encoding='utf-8')
        Path(args.out).with_suffix('.json').write_text(json.dumps({
            'pid': pid, 'gog': fn.gog, 'elapsed': elapsed, 'samples': samples, 'window': args.window,
            'self': total_self.most_common(200), 'incl': total_incl.most_common(200),
            'windows': {str(w): windows[w].most_common(30) for w in windows},
            'windows_incl': {str(w): win_incl[w].most_common(30) for w in win_incl},
        }, indent=1), encoding='utf-8')

    try:
        while True:
            now = time.perf_counter() - t0
            if args.seconds and now >= args.seconds:
                break
            code = W.DWORD()
            if not k32.GetExitCodeProcess(hproc, C.byref(code)) or code.value != 259:
                print('game exited'); break
            if rounds % 50 == 0:
                for tid in threads_of(pid):
                    if tid not in handles:
                        h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, False, tid)
                        if h:
                            handles[tid] = h
                mods = modules_of(hproc); mod_starts = [m[0] for m in mods]
            win = int(now // args.window)
            for tid, h in list(handles.items()):
                ft = [W.FILETIME() for _ in range(4)]
                if not k32.GetThreadTimes(h, *[C.byref(x) for x in ft]):
                    k32.CloseHandle(h); del handles[tid]; continue
                cpu = (ft[2].dwHighDateTime << 32 | ft[2].dwLowDateTime) + (ft[3].dwHighDateTime << 32 | ft[3].dwLowDateTime)
                prev = last_cpu.get(tid); last_cpu[tid] = cpu
                if prev is None or cpu == prev:
                    continue
                if k32.SuspendThread(h) == 0xFFFFFFFF:
                    continue
                try:
                    if not k32.GetThreadContext(h, C.byref(ctx)):
                        continue
                    rip, rsp = ctx.Rip, ctx.Rsp
                    got = C.c_size_t()
                    frames = unwind(hproc, h, ctx, args.frames) if args.frames else []
                    ok_stack = False if frames else k32.ReadProcessMemory(hproc, C.c_void_p(rsp), stackbuf, args.stack, C.byref(got))
                finally:
                    k32.ResumeThread(h)
                samples += 1
                m, f = where(rip)
                key = fn.name(f) if m == 'exe' else m
                windows[win][key] += 1; total_self[key] += 1; win_threads[win][tid] += 1
                seen = {key}
                first_exe = None
                if frames:
                    # the summary: the first frame outside ntdll/KernelBase (a
                    # runtime lock or wait, say) and the first two game frames
                    chain, outside, exes = [], None, []
                    for addr in frames[1:]:
                        fm, ff = where(addr)
                        name = fn.name(ff) if fm == 'exe' else fm
                        seen.add(name)
                        if fm == 'exe':
                            if first_exe is None:
                                first_exe = name
                            if len(exes) < 2 and name not in exes:
                                exes.append(name)
                        elif outside is None and not fm.lower().startswith(('ntdll', 'kernelbase', 'kernel32')):
                            outside = name
                    chain = ([outside] if outside else []) + exes
                    if m != 'exe':
                        stacks[win][key + ' <- ' + ' <- '.join(chain)] += 1
                elif ok_stack or got.value:
                    data = bytes(stackbuf[:got.value or args.stack])
                    for off in range(0, len(data) - 7, 8):
                        v = struct.unpack_from('<Q', data, off)[0]
                        if exe_base <= v < exe_base + exe_size:
                            r = v - exe_base
                            if fn.after_call(r):
                                fs = fn.func(r)
                                if fs is not None:
                                    seen.add(fn.name(fs))
                                    if first_exe is None:
                                        first_exe = fn.name(fs)
                if m != 'exe':
                    callers[win][f'{m.split("!")[0]} <- {first_exe or "?"}'] += 1
                    total_callers[f'{m.split("!")[0]} <- {first_exe or "?"}'] += 1
                for k in seen:
                    win_incl[win][k] += 1; total_incl[k] += 1
            rounds += 1
            if rounds % int(max(1, args.window / args.interval)) == 0:
                write_report()
                top = windows[win].most_common(3)
                print(f't+{now:6.1f}s  {samples} samples  top: ' + '; '.join(f'{k} {v}' for k, v in top), flush=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    write_report()
    print(f'report: {Path(args.out).resolve()}')


if __name__ == '__main__':
    main()
