"""Generation exceptions for reviewed Pokemon Legends: Z-A executables."""

MAIN_BUILD_ID = '7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000'


def configure(module):
    if module.name != 'main':
        return
    if module.nso.build_id_full != MAIN_BUILD_ID:
        raise ValueError('Unsupported Z-A main executable')
    # The six-way rotation switch schedules vector work between its unsigned
    # range check and byte-table lookup. Keep the exception at this exact site;
    # neither other Z-A branches nor other adapters get a wider search window.
    guards = {
        0x1748318: 0x7100143f,  # cmp w1, #5
        0x174831c: 0x54001628,  # b.hi default
        0x1748324: 0x2a0103e8,  # mov w8, w1
        0x174832c: 0xd000d0e9,  # adrp x9, table page
        0x1748330: 0x91098929,  # add x9, x9, table offset
        0x1748390: 0x100000ca,  # adr x10, case origin
        0x1748394: 0x3868692b,  # ldrb w11, [x9, x8]
        0x1748398: 0x8b0b094a,  # add x10, x10, x11, lsl #2
        0x17483a4: 0xd61f0140,  # br x10
    }
    for offset, expected in guards.items():
        address = module.base + offset
        if not module.in_text(address) or module.word(address) != expected:
            raise ValueError(f'Z-A table instruction mismatch at {offset:#x}')
    module.table_guard_distances = {module.base + 0x17483a4: 34}
