# Read the live battle list out of the running game, to check the layout claim
# against the process rather than against the notes.
#
# usage: m0_live_read.py <pid> <holder_hex> <vec_hex>
import ctypes, ctypes.wintypes as w, struct, sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

def open_proc(pid):
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        raise OSError('OpenProcess(%d) failed: %d' % (pid, ctypes.get_last_error()))
    return h

def rd(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    if not ok or got.value != n:
        raise OSError('ReadProcessMemory(0x%X, %d) failed: %d (got %d)'
                      % (addr, n, ctypes.get_last_error(), got.value))
    return buf.raw

def main():
    pid  = int(sys.argv[1])
    hold = int(sys.argv[2], 16)
    vec  = int(sys.argv[3], 16)
    h = open_proc(pid)

    print("holder 0x%X  vec 0x%X  (pid %d)" % (hold, vec, pid))

    slot = struct.unpack('<Q', rd(h, hold + 0x1F90, 8))[0]
    print("holder+0x1F90 -> 0x%X   %s" % (slot, "== vec" if slot == vec else "!! DIFFERS from the log"))

    head = rd(h, vec, 0x18)
    refcount, pad, cap, count = struct.unpack('<IIII', head[:16])
    data = struct.unpack('<Q', head[16:24])[0]
    print("vec[+00]=%u  [+04]=%u  [+08]cap=%u  [+0C]count=%u  [+10]data=0x%X"
          % (refcount, pad, cap, count, data))

    # The mod reads count at +0xC and data at +0x10. If that is right, the memory
    # at data is an array of Character* and count of them should be readable.
    print("\nfirst 16 pointer slots:")
    ptrs = []
    for i in range(min(count, 16)):
        p = struct.unpack('<Q', rd(h, data + i * 8, 8))[0]
        ptrs.append(p)
        print("   [%2d] 0x%X" % (i, p))

    print("\n(if these are Character objects, the first 4 bytes are a vtable pointer)")
    if ptrs:
        vt = struct.unpack('<Q', rd(h, ptrs[0], 8))[0]
        print("   cat0 vtable 0x%X" % vt)

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(__doc__); sys.exit(1)
    main()
