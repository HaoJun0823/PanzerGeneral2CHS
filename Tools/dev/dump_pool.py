"""把运行中的 PG2UK220.EXE 的字符串池导出来，用于对比英文/中文两次运行的结构差异。

用法：游戏启动后运行本脚本，输出 PG2CHS_DLL/build/pool_<tag>.txt
"""
import ctypes
import re
import subprocess
import sys
from ctypes import wintypes

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p),
                ('AllocationProtect', wintypes.DWORD), ('RegionSize', ctypes.c_size_t),
                ('State', wintypes.DWORD), ('Protect', wintypes.DWORD), ('Type', wintypes.DWORD)]


def main(tag):
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq PG2UK220.EXE', '/FO', 'CSV'],
                         capture_output=True).stdout.decode('latin-1')
    m = re.search(r'PG2UK220\.EXE","(\d+)"', out)
    if not m:
        print('game not running')
        return
    pid = int(m.group(1))
    h = k32.OpenProcess(0x1F0FFF, False, pid)
    if not h:
        print('OpenProcess failed', ctypes.get_last_error())
        return

    buf = ctypes.create_string_buffer(1 << 20)
    # 逐区域扫描，找同时含多条 GUI97 行文本的堆块
    addr = 0x10000
    hits = []
    while addr < 0x7FFFFFFF:
        mbi = MBI()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base, size, state = mbi.BaseAddress or 0, mbi.RegionSize, mbi.State
        if state == 0x1000 and 0 < size <= (1 << 24):
            n = ctypes.c_size_t(0)
            if k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, min(size, 1 << 20), ctypes.byref(n)):
                data = buf.raw[:n.value]
                if data.count(b'Play A Scenario') or data.count(b'Play A Campaign') \
                        or data.count(b'\xbd\xf8\xd0\xd0\xb3\xa1\xbe\xb0'):
                    hits.append((base, data))
        addr = base + size

    print('pool regions:', [(hex(b), len(d)) for b, d in hits])
    for base, data in hits:
        path = f'PG2CHS_DLL/build/pool_{tag}_{base:08x}.bin'
        open(path, 'wb').write(data)
        print('saved', path, len(data))
        # 顺手列出该区块内的可打印串
        with open(f'PG2CHS_DLL/build/pool_{tag}_{base:08x}.txt', 'w', encoding='utf-8') as f:
            for mm in re.finditer(rb'[\x20-\x7e\x81-\xfe]{2,}\x00', data):
                f.write(f'{base + mm.start():#010x}\t{mm.group()[:-1]!r}\n')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'run')
