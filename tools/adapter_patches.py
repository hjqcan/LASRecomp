"""Guarded, reviewed instruction replacements for generic title adapters."""
import struct


def apply_instruction_patches(module, release):
    edits = []
    for patch in release.instruction_patches:
        if patch['module'] != module.name:
            continue
        address = module.base + patch['offset']
        if not module.in_text(address):
            raise ValueError(f'Adapter patch is outside {module.name} text: {address:#x}')
        if module.word(address) != patch['expected']:
            raise ValueError(f'Adapter patch instruction mismatch at {address:#x}')
        edits.append((address, patch['replacement']))
    # Validate every guard before altering even the temporary analysis image.
    words = list(module.words)
    for address, replacement in edits:
        words[(address - module.lo) // 4] = replacement
        struct.pack_into('<I', module.nso.image, address - module.base, replacement)
    module.words = tuple(words)
    return len(edits)
