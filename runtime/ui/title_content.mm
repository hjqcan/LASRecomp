#include "../title_profile.h"
#include <filesystem>
#include <cstdio>
namespace fs = std::filesystem;

extern "C" bool title_content_root(const TitleProfile *p, const char *romfs) {
    if (!p || !romfs) return false;
    try {
        const auto root = fs::u8path(romfs);
        for (unsigned i = 0; i < p->romfs_root_count; ++i)
            if (!fs::is_directory(root / fs::u8path(p->romfs_roots[i]))) return false;
        return p->romfs_root_count > 0;
    } catch (...) { return false; }
}

extern "C" bool title_content_validate(const TitleProfile *p, const char *romfs, bool has_image,
                                        int *count, char *why, size_t size) {
    if (!title_content_root(p, romfs)) {
        snprintf(why, size, "Game data missing or does not match the adapter's content rules"); return false;
    }
    if (has_image && p->image_satisfies_count) return true;
    try {
        if (*count < 0) {
            *count = 0;
            for (auto i = fs::recursive_directory_iterator(fs::u8path(romfs)); i != fs::recursive_directory_iterator(); ++i) {
                if (i.depth() > 32 || i->is_symlink()) {
                    snprintf(why, size, "Unsupported linked or deeply nested game data"); return false;
                }
                if (i->is_regular_file()) ++*count;
            }
        }
        if ((unsigned)*count >= p->minimum_files) return true;
        snprintf(why, size, "Game files incomplete (%d of %u files)", *count, p->minimum_files);
    } catch (const std::exception &e) { snprintf(why, size, "Cannot inspect game data: %s", e.what()); }
    return false;
}
