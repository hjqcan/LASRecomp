"""Internal file paths; extended Windows paths do not need a registry change."""
import os
from pathlib import Path


def logical_path(path):
    value = os.fspath(path)
    if os.name == 'nt':
        if value.startswith('\\\\?\\UNC\\'):
            return Path('\\\\' + value[8:])
        if value.startswith('\\\\?\\'):
            return Path(value[4:])
    return Path(value)


def io_path(path):
    if os.name != 'nt':
        return Path(path)
    value = os.path.abspath(logical_path(path))
    if value.startswith('\\\\'):
        return Path('\\\\?\\UNC\\' + value[2:])
    return Path('\\\\?\\' + value)


def read_shared(path, limit=4096):
    """Read a small atomic record without blocking replacement on Windows."""
    path = io_path(path)
    if not path.is_file(): raise ValueError(f'Not a regular installation record: {path.name}')
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        import msvcrt
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        handle = kernel.CreateFileW(str(path), 0x80000000, 7, None, 3, 0x80, None)
        if handle == ctypes.c_void_p(-1).value: raise ctypes.WinError(ctypes.get_last_error())
        try:
            fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        except OSError:
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel.CloseHandle(handle)
            raise
        stream = os.fdopen(fd, 'rb')
    else:
        stream = path.open('rb')
    with stream:
        value = stream.read(limit + 1)
    if len(value) > limit: raise ValueError(f'Installation record exceeds {limit} bytes: {path.name}')
    return value
