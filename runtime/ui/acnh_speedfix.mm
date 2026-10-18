#include "../acnh_speedfix.h"
#include "../cache_sha256.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

namespace fs = std::filesystem;
#include "../acnh_speedfix_data.inc"

static std::atomic<bool> active{false};
static std::string active_root;
static TitleProfile variant;
static const TitleProfile *original;
static std::string variant_identity;
static std::once_flag initialized;

static int marker(const char *root) {
    auto path = fs::u8path(root) / ".acnh-speedfix60";
    if (!fs::exists(path)) return fs::u8path(root).lexically_normal().filename()==".acnh60" ? -1 : 0;
    FILE *f = fopen(path.u8string().c_str(), "rb");
    if (!f) return -1;
    char bytes[128] = {};
    size_t n = fread(bytes, 1, sizeof bytes, f);
    fclose(f);
    std::string expected = std::string("LASACNH60-1\n") + ACNH60_MANIFEST_SHA + "\n";
    return n == expected.size() && !memcmp(bytes, expected.data(), n) ? 1 : -1;
}

static bool patched_header(const char *root) {
    auto path = fs::u8path(root) / "exefs/main";
    FILE *f = fopen(path.u8string().c_str(), "rb");
    if (!f) return false;
    unsigned char header[0xc0];
    size_t n = fread(header, 1, sizeof header, f);
    fclose(f);
    if (n != sizeof header || memcmp(header, "NSO0", 4)) return false;
    static const char hex[] = "0123456789abcdef";
    char sha[65] = {};
    for (int i = 0; i < 32; ++i) sha[i*2] = hex[header[0xa0+i] >> 4], sha[i*2+1] = hex[header[0xa0+i] & 15];
    return !strcmp(sha, ACNH60_MAIN_TEXT_SHA);
}

extern "C" const TitleProfile *acnh60_profile(const TitleProfile *base, const char *root, char *why, size_t cap) {
    if (!base) return nullptr;
    try {
        int mode = marker(root);
        if (!mode && !patched_header(root)) return base;
        if (mode != 1 || strcmp(base->key, "acnh") || strcmp(base->version, "3.0.1")) {
            snprintf(why, cap, "The 60 FPS installation is incomplete or belongs to another release; prepare it again from normal 3.0.1 files.");
            return nullptr;
        }
        std::call_once(initialized, [base] {
            original = base;
            CacheSha256 h;
            std::string identity = std::string(base->code_identity) + "\nacnh60-1\n" + ACNH60_MANIFEST_SHA;
            h.add(identity.data(), identity.size());
            variant_identity = h.finish();
            variant = *base;
            variant.code_identity = variant_identity.c_str();
        });
        return &variant;
    } catch (const std::exception &e) {
        snprintf(why, cap, "Cannot inspect the 60 FPS installation: %s", e.what());
        return nullptr;
    }
}

extern "C" const char *acnh60_generator_identity(const TitleProfile *profile) {
    // The prepared NSOs already contain the reviewed patches. The generic
    // generator still validates the underlying release and its own source ID.
    return profile == &variant && original ? original->code_identity : profile->code_identity;
}

extern "C" bool acnh60_validate(const char *root, char *why, size_t cap) {
    try {
        int mode = marker(root);
        if (!mode && !patched_header(root)) return true;
        if (mode != 1) {
            snprintf(why, cap, "Missing or mismatched 60 FPS preparation record");
            return false;
        }
        for (const auto &entry : acnh60_files) {
            auto path = fs::u8path(root) / fs::u8path(entry.path);
            if (fs::is_symlink(fs::symlink_status(path)) || fs::file_size(path) != entry.size) {
                snprintf(why, cap, "60 FPS file is missing or has changed: %s", entry.path);
                return false;
            }
            FILE *f = fopen(path.u8string().c_str(), "rb");
            if (!f) { snprintf(why, cap, "Cannot read 60 FPS file: %s", entry.path); return false; }
            CacheSha256 h;
            char buffer[65536]; size_t count;
            while ((count = fread(buffer, 1, sizeof buffer, f))) h.add(buffer, count);
            bool ok = !ferror(f) && h.finish() == entry.sha;
            fclose(f);
            if (!ok) { snprintf(why, cap, "60 FPS integrity check failed: %s", entry.path); return false; }
        }
        return true;
    } catch (const std::exception &e) {
        snprintf(why, cap, "Cannot verify the 60 FPS installation: %s", e.what());
        return false;
    }
}

extern "C" bool acnh60_boot(const char *root, char *why, size_t cap) {
    acnh60_clear();
    const TitleProfile *p = acnh60_profile(title_profile_current(), root, why, cap);
    if (!p || !acnh60_validate(root, why, cap)) return false;
    if (p == &variant) {
        if (strlen(root)>1100) {snprintf(why,cap,"The 60 FPS installation path is too long.");return false;}
        active_root = root;
        active.store(true, std::memory_order_release);
        fprintf(stderr, "[acnh60] 60 FPS + SpeedFix active; all 3 modules and 4 resources verified\n");
    }
    return true;
}

extern "C" bool acnh60_active(void) { return active.load(std::memory_order_acquire); }
extern "C" void acnh60_clear(void) { active.store(false, std::memory_order_release); active_root.clear(); }

extern "C" bool acnh60_romfs_override(const char *relative, char *out, size_t cap) {
    if (!acnh60_active()) return false;
    while (*relative == '/') ++relative;
    for (const auto &entry : acnh60_files) {
        if (!strncmp(entry.path, "romfs/", 6) && !strcmp(entry.path + 6, relative))
            return snprintf(out, cap, "%s/%s", active_root.c_str(), entry.path) < (int)cap;
    }
    return false;
}

extern "C" bool acnh60_base_path(const char *root, char *out, size_t cap) {
    try {
        auto path = fs::absolute(fs::u8path(root)).lexically_normal();
        if (path.filename() != ".acnh60" || marker(root) != 1) return false;
        return snprintf(out, cap, "%s", path.parent_path().u8string().c_str()) < (int)cap;
    } catch (...) { return false; }
}
