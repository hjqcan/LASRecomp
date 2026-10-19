"""Small synthetic plaintext NSO and RomFS fixtures; no game assets."""
import struct


def synthetic_romfs(root_name='region_common'):
    empty = 0xffffffff
    dirs = struct.pack('<6I', 0, empty, 24, empty, empty, 0)
    roots = [root_name] if isinstance(root_name, str) else list(root_name)
    for i, root in enumerate(roots):
        name = root.encode()
        sibling = len(dirs) + 24 + len(name) + (-len(name) % 4) if i + 1 < len(roots) else empty
        dirs += struct.pack('<6I', 0, sibling, empty, 0 if i == 0 else empty, empty, len(name)) + name + bytes(-len(name) % 4)
    files = struct.pack('<IIQQII', 24, empty, 0, 8, empty, 11) + b'example.bin' + bytes(1)
    header = (80, 80, 4, 84, len(dirs), 84 + len(dirs), 4, 88 + len(dirs), len(files), 0x1000)
    image = bytearray(0x10000)
    struct.pack_into('<10Q', image, 0, *header)
    image[header[3]:header[3] + len(dirs)] = dirs
    image[header[7]:header[7] + len(files)] = files
    image[0x1000:0x1008] = b'original'
    return image


def synthetic_nso(build_id):
    main = bytearray(0x60)
    main[:4] = b'NSO0'
    main[0x40:0x60] = bytes.fromhex(build_id.ljust(64, '0'))
    return main
