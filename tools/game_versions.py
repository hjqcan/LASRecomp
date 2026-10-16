"""Explicitly supported executable layouts. Addresses in patches.py use v1.0.0.

The 1.1.0 executable preserves the data layout and all function boundaries up to
main+0xdfadc0. That function grows by 0xd0 bytes; following text moves by 0xd0.
Its changed interior is deliberately excluded from the patch-address mapping.
"""
import re
import struct

BASE = 0x7000000000
BASE_ID = 'ae16f71e002af8cb059a9a74c4d90f34ba984892'
UPDATE_ID = '909e904af78ac1b8deefe97ab2ccdb51968f0ec7'
VERSIONS = {BASE_ID: '1.0.0', UPDATE_ID: '1.1.0'}
TEXT_CUT = BASE + 0xdfaf50
TEXT_END = BASE + 0x1433830
TEXT_DELTA = 0xd0


def game_version(build_id):
    try:
        return VERSIONS[build_id]
    except KeyError:
        raise ValueError(f'Unsupported game executable Build ID: {build_id}') from None


def address(addr, version):
    if version == '1.1.0':
        if BASE + 0xdfaee0 <= addr < TEXT_CUT:
            raise ValueError(f'No validated mapping inside changed function: {addr:#x}')
        if TEXT_CUT <= addr < TEXT_END:
            return addr + TEXT_DELTA
    return addr


def original_address(addr, version):
    if version == '1.1.0' and TEXT_CUT + TEXT_DELTA <= addr < TEXT_END + TEXT_DELTA:
        return addr - TEXT_DELTA
    return addr


def patch_word(gen, canonical):
    version = getattr(gen, 'game_version', '1.0.0')
    actual = address(canonical, version)
    word = struct.unpack_from('<I', gen.nso.image, actual - gen.base)[0]
    # Existing assertions check the original instruction, including relative BL
    # immediates. Translate the target back before checking, never skip the check.
    if version == '1.1.0' and word & 0x7c000000 == 0x14000000:
        imm = word & 0x3ffffff
        if imm & 0x2000000:
            imm -= 0x4000000
        target = original_address(actual + imm * 4, version)
        word = (word & 0xfc000000) | (((target - canonical) // 4) & 0x3ffffff)
    return word


def patch_table(patches, version):
    if version == '1.0.0':
        return patches.copy()
    def wrap(fn, canonical):
        def emit(gen, actual):
            if actual != address(canonical, version):
                raise ValueError('Patch address/profile mismatch')
            code = fn(gen, canonical)
            return re.sub(r'(?<![0-9a-fA-F])700[0-9a-fA-F]{7}(?![0-9a-fA-F])',
                          lambda m: f'{address(int(m[0], 16), version):x}', code)
        return emit
    return {address(a, version): wrap(fn, a) for a, fn in patches.items()}
