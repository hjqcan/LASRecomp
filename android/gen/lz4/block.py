"""lz4.block for the game code generator on Android (python-lz4's C extension isn't built there): the decompress
tools/nso.py uses, through liblz4 with ctypes. LIBLZ4_PATH: the folder with liblz4.so (else the library search path).
From LASRecompAndroid by mamcjr (MIT).
"""
import ctypes
import os

_lib = ctypes.CDLL(os.path.join(os.environ.get('LIBLZ4_PATH', ''), 'liblz4.so'))
_lib.LZ4_decompress_safe.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
_lib.LZ4_decompress_safe.restype = ctypes.c_int


def decompress(source, uncompressed_size=-1, return_bytearray=False):
    """As python-lz4's: with uncompressed_size, source is the raw block; without, it starts with its size (u32 LE)."""
    source = bytes(source)
    if uncompressed_size < 0:
        uncompressed_size = int.from_bytes(source[:4], 'little')
        source = source[4:]
    dst = ctypes.create_string_buffer(max(uncompressed_size, 1))
    n = _lib.LZ4_decompress_safe(source, dst, len(source), uncompressed_size)
    if n < 0:
        raise RuntimeError('lz4: corrupt input')
    out = dst.raw[:n]
    return bytearray(out) if return_bytearray else out
