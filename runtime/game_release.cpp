// Which release the game files are, for the start screen and the log (game_addresses.h game_release_label).
// Releases that share one main program (1.0.1 and 1.1.0) differ only in exefs/main.npdm: its SHA-256 tells them
// apart. Only the hashes are kept here (game_addresses/versions.h), nothing of the files.
#include "game_addresses.h"

#include <cstdio>
#include <cstring>

#include "cache_sha256.h"

namespace {
struct Release {
    const char *key, *label, *npdm_sha256;
};
const Release releases[] = {
#define GA_VERSION(key, label, build_id)
#define GA_RELEASE(key, label, npdm_sha256) {#key, label, npdm_sha256},
#include "game_addresses/versions.h"
#undef GA_VERSION
#undef GA_RELEASE
};

// SHA-256 of the file as lower-case hex; empty when it is missing or larger than an npdm can be.
std::string file_sha256(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return {};
    CacheSha256 h;
    unsigned char buf[4096];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && (total += n) <= (1u << 20)) h.add(buf, n);
    bool ok = !ferror(f) && total <= (1u << 20);
    fclose(f);
    return ok ? h.finish() : std::string();
}
}  // namespace

extern "C" const char *game_release_label(const char *exefs_dir, char *buf, size_t size) {
    char path[1100];
    snprintf(path, sizeof path, "%s/main", exefs_dir);
    const GameVersion *v = game_version_of_main(path);
    if (!v) return nullptr;
    snprintf(buf, size, "%s", v->label);
    bool shared = false;
    for (const Release &r : releases) shared |= !strcmp(r.key, v->key);
    if (!shared) return buf;
    snprintf(path, sizeof path, "%s/main.npdm", exefs_dir);
    std::string sha = file_sha256(path);
    for (const Release &r : releases)
        if (!strcmp(r.key, v->key) && sha == r.npdm_sha256) snprintf(buf, size, "%s", r.label);
    return buf;
}
