# Save file format (`savedataN.bin`)

The layout of one save slot, mapped on game version 1.0.0. As far as we know 1.0.1 and 1.1.0 use the same layout
(1.0.1 changed one function in the save loader, not the format). Code addresses are v1.0.0 offsets in the `main` module (`main+0x...`). For other versions, look them up in `runtime/game_addresses/`. Our
reading and writing of saves is in `runtime/services/fs.c` and is covered by `tools/test_save.py`.

How sure each entry is:

- **[V]**: confirmed in the game's code and in real save files
- **[C]**: confirmed in the code, but not yet seen in a save
- **[U]**: not verified

## Code locations

| Address | What |
|---|---|
| `main+0x1CBB200` | save manager. `+0x20` is the live save buffer: 0x5EE0 bytes, exactly the file |
| `main+0x1CC1368` | global `PlayerStatus*`, file offset 0x54C0 (all inventory reads go through it) |
| `main+0xDFAB90` | new game: clears the buffer, header version 0xE, UUID, header CRC, then body defaults (`0xDF9890`) |
| `main+0xDFAD00` | checksums (header CRC and body CRC), run on every save |
| `main+0xDFCC60` | save: copies the live buffer, fills the header summary (`0xDFC640`), runs the checksums |
| `main+0xDF8260` | `GetMaxHealth()` |
| `main+0x7E2B7C` | `HasItem(itemType)` (EventFlow item enum) |
| `main+0x8D1A50` / `0x8D090C` | `HasItemByKey` / `AddItemByKey` (Items.gsheet item IDs) |

## Layout

| Range | Size | Block |
|---|---|---|
| 0x0000-0x00DF | 0xE0 | header: the file-select summary, with its own CRC |
| 0x00E0-0x021F | 0x140 | dungeon records, 0x10 each |
| 0x0220-0x481F | 0x4600 | room records, 0xE0 each |
| 0x4820-0x521F | 0xA00 | 0x20-byte records |
| 0x5220-0x537F | | other flags |
| 0x5380-0x54BF | 0x140 | profile: name, death count |
| 0x54C0-0x561F | 0x160 | PlayerStatus: inventory, health, rupees, position |
| 0x5620-0x5DBF | | not mapped |
| 0x5DC0-0x5EBF | 0x100 | event flags |
| 0x5EC4 | 4 | body CRC |

All integers are little-endian.

## Checksums [V]

There are two CRC32s, fixed after any edit. They use the standard reflected table (polynomial 0xEDB88320), but the
starting value is 0xEDB88320 and there's no final XOR, so this is not `zlib.crc32`.

| Field | Offset | Covers | Zeroed while computing |
|---|---|---|---|
| Header CRC | 0x0004 (u32) | 0x0000-0x00DF | 0x0004-0x0007 |
| Body CRC | 0x5EC4 (u32) | 0x00E0-0x5EDF | 0x5EC4-0x5EC7 |

Neither covers the other. On load the game checks that the version is 0xE, the buffer is 32-byte aligned and the
size is at least 0x5EE0.

```python
import struct
_T = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    _T.append(c)

def la_crc(buf, init=0xEDB88320):
    c = init
    for b in buf:
        c = _T[(c ^ b) & 0xFF] ^ (c >> 8)
    return c  # no final XOR

def fix_checksums(d: bytearray):
    d[4:8] = b'\0' * 4
    struct.pack_into('<I', d, 4, la_crc(d[0:0xE0]))
    d[0x5EC4:0x5EC8] = b'\0' * 4
    struct.pack_into('<I', d, 0x5EC4, la_crc(d[0xE0:0x5EE0]))
```

## Header (0x0000-0x00DF)

The file-select screen shows these. They are copies the game rewrites from the body on every save. If you edit the
body, update them too, then fix the header CRC.

| Field | Off | Type | Notes | |
|---|---|---|---|---|
| Version | 0x00 | u32 | always 0xE | V |
| Header CRC | 0x04 | u32 | see above | V |
| UUID | 0x08 | 16 bytes | made at new game; leave alone | C |
| Player name | 0x20 | UTF-16LE x 9 | copy of 0x5388 | V |
| Current health | 0x32 | u16 | quarter hearts, copy of 0x5544 | V |
| Max health | 0x34 | u16 | quarter hearts, `GetMaxHealth()` | V |
| Death count | 0x36 | u16 | copy of 0x5384 | C |
| Location name | 0x38 | char[0x40] | level name, for example `Field` | V |
| Key/instrument mask | 0x78 | u32 | bits 0-16 of 0x556C | C |
| Rupees | 0x7C | u32 | copy of 0x5540 | V |
| Seashells | 0x80 | u64 | copy of 0x5548 | V |
| Save time | 0x88 | u16 year, then u8 month, day, hour, minute, second | | V |
| Has Secret Medicine | 0xD6 | u8 | bit 1 of 0x556C | C |

## Profile (0x5380-0x54BF)

| Field | Off | Type | Notes | |
|---|---|---|---|---|
| Death count | 0x5384 | u16 | copied to header 0x36 | C |
| Player name | 0x5388 | UTF-16LE, 9 units including the null | | V |

## PlayerStatus (0x54C0-0x561F)

| Field | Off | Type | Notes | |
|---|---|---|---|---|
| Current level | 0x54C0 | char[0x40] | for example `Field`; bytes after the null are leftovers | V |
| Room / spawn | 0x5500 | char[0x40] | | U |
| Rupees | 0x5540 | u32 | 0-999 | V |
| Current health | 0x5544 | s16 | quarter hearts; keep at or under max. 0 loads as 3 to 10 hearts depending on max | V |
| Secret Seashells | 0x5548 | u64 bits | one bit per shell location, bits 0-49; count = bits set | V |
| Heart pieces | 0x5550 | u64 bits | one bit per piece location, bits 0-31 | V |
| Heart containers | 0x5558 | u16 bits | bits 0-8 | C |
| Trading item | 0x555A | u8 | 0 none, 1 Yoshi Doll, 2 Ribbon, 3 Dog Food, 4 Bananas, 5 Stick, 6 Honeycomb, 7 Pineapple, 8 Hibiscus, 9 Goat's Letter, 0xA Broom, 0xB Fishing Hook, 0xC Necklace, 0xD Mermaid's Scale, 0xE Magnifying Lens | V |
| Ocarina songs | 0x555B | u8 bits | bit 0 Ballad of the Wind Fish, 1 Manbo's Mambo, 2 Frog's Song of Soul | C |
| Upgrades | 0x555C | u8 bits | bit 0 sword level 2, 1 Mirror Shield, 2 bracelet level 2, 3 powder cap 40, 4 bomb cap 60, 5 arrow cap 60, 6 red tunic, 7 blue tunic | C |
| Companion | 0x555D | u8 | 0 none, 1 BowWow, 2 Marin, 3 Ghost, 4 Flying Rooster | V |
| Bombs | 0x555E | u8 | cap 30, or 60 with the upgrade bit; needs the bomb bag bit | C |
| Arrows | 0x555F | u8 | cap 30 / 60; needs the bow bit | C |
| Magic Powder | 0x5560 | u8 | cap 20 / 40; needs the powder bit | C |
| Equipped Y / X | 0x5562 / 0x5563 | u8 | item slot; 0xFF empty | U |
| Items | 0x5568 | u32 bits | see below | V |
| Key items, instruments | 0x556C | u32 bits | see below | C |
| Seashell Sensor | 0x556E | u8 | bit 0 | C |
| Bottles | 0x5580 | u8 bits | bit n: bottle n holds a fairy | C |
| Golden Leaves | 0x5582 | u8 bits | bits 0-4, one per leaf | V |
| Position, rotation | 0x5584 | f32s | | U |
| Figurines | 0x55A8 | u32 bits | 0x1 Chain Chomp, 0x2 Goomba, 0x4 Spiny, 0x8 Boo, 0x10 Cheep Cheep, 0x20 Blooper, 0x40 Shy Guy, 0x80 Piranha Plant, 0x100 Pokey, 0x200 BowWow | C |

### Items, 0x5568 [C]

| Bit | Item | Bit | Item |
|---|---|---|---|
| 0 | Sword (level 2 also sets 0x555C bit 0) | 8 | Magic Powder |
| 1 | Shield (Mirror Shield also sets 0x555C bit 1) | 9 | Roc's Feather |
| 2 | Bomb bag | 10 | Power Bracelet (level 2 also sets 0x555C bit 2) |
| 3 | Bow | 11 | Pegasus Boots |
| 4 | Hookshot | 12 | Ocarina |
| 5 | Boomerang | 13 | Bottle 1 |
| 6 | Magic Rod | 14 | Bottle 2 |
| 7 | Shovel | 15 | Bottle 3 |

### Key items and instruments, 0x556C [C]

| Bit | Item | Bit | Item |
|---|---|---|---|
| 0 | Flippers | 8 | Conch Horn (instrument 2) |
| 1 | Secret Medicine | 9 | Sea Lily's Bell (3) |
| 2 | Tail Key | 10 | Surf Harp (4) |
| 3 | Slime Key | 11 | Wind Marimba (5) |
| 4 | Angler Key | 12 | Coral Triangle (6) |
| 5 | Face Key | 13 | Evening Calm Organ (7) |
| 6 | Bird Key | 14 | Thunder Drum (8) |
| 7 | Full Moon Cello (instrument 1) | 15 | Sleepy Toadstool |

Bit 16 is the Seashell Sensor (byte 0x556E).

## Max health [C]

Max health isn't stored. `GetMaxHealth()` computes it in quarter hearts:

```
max = 12 + 4 * (popcount(u16 @ 0x5558) + popcount(u64 @ 0x5550) / 4)
```

That's 3 hearts, plus 1 per container, plus 1 per 4 pieces. If you change these, also set the current health
(0x5544) and the header copies (0x32, 0x34).

## Dungeon records (0x00E0 + 0x10 x i)

Dungeon n uses record 2(n-1) (Tail Cave 0, Bottle Grotto 2, ...). The Color Dungeon uses record 18. [V for keys]

| Offset | Type | Meaning |
|---|---|---|
| +0 | u8 | small keys |
| +1 | u8 bits | bit 0 Compass, 1 Dungeon Map, 2 Stone Beak, 3 Nightmare Key |

## Event flags (0x5DC0, 256 bytes) [V]

`GlobalFlags.gsheet` flag n is bit n of this 2048-bit field. The game itself uses indices up to 1118; LAS Recomp's
randomizer adds its own flags above those.

## Not found yet

Play time, the Hero Mode flag, and Chamber Dungeon data.
