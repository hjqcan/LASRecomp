"""Validate the finite randomizer patch catalog against supported main executables.

All 42 sites precede the changed function. Their original words, the replacement
branch targets and the two data locations are unchanged in the updated layout.
Only digests of the original input are kept here; no executable bytes are shipped.
"""
import hashlib
import struct

from game_versions import BASE_ID, UPDATE_ID

CODE_DIGEST = '06ae074d03c791a453cb1ae6bbc3b717a84636775b77e4dea63ff2b97a756412'
DATA_DIGESTS = {
    0x16cbd73: 'ab732d2bf544541457e70f7bfe3707ecf94048bab32f578848ca9ea87ba341fe',
    0x16fae60: '85d094fdbe5791267dbcf6cfd6cde08f32e64926e99d7b22a51323c3ed710820',
}


def validate_catalog(image, build_id, code):
    if build_id not in (BASE_ID, UPDATE_ID) or len(code) != 42:
        raise ValueError('Unsupported randomizer executable/catalog')
    sites = sorted(code)
    if any(a < 0 or a + 4 > len(image) or a >= 0xdfaee0 for a in sites):
        raise ValueError('Randomizer patch falls outside its validated layout')
    digest = hashlib.sha256(b''.join(struct.pack('<I', a) + image[a:a+4] for a in sites)).hexdigest()
    if digest != CODE_DIGEST:
        raise ValueError('Randomizer instruction preimage mismatch')
    for address, expected in DATA_DIGESTS.items():
        if hashlib.sha256(image[address:address+64]).hexdigest() != expected:
            raise ValueError('Randomizer data preimage mismatch')
    return code
