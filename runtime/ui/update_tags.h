// Release tags for the update check (runtime/ui/launcher.mm): their order and the name players see.
// Tags: beta_<n> and its fixes releases beta_<n>.<m> (to Beta 5), v0.<n>.<m> from Beta 6, v1.0.0 and up after the beta.
// Under the hood every release is a semantic version: beta_<n> = 0.<n>.0, beta_<n>.<m> = 0.<n>.<m> (the macOS
// bundle version, tools/bundle_macos.sh). Test (tools/test_update_tags.cpp): no game, no network.
#pragma once
#include <stdio.h>
#include <string>

// Higher = newer; -1: not a release tag (alpha_*, test-*, anything else). v0.<n>.<m> ranks with beta_<n>.<m>
// (so Beta 5 builds offer v0.6.0 as a newer beta), v1.0.0+ above every beta.
static inline long long tag_rank(const std::string &t) {
    int n, m, a, b, c;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1 && n > 0 && n < (1 << 20)) return (long long)n << 12;
    if (sscanf(t.c_str(), "beta_%d.%d%c", &n, &m, &end) == 2 && n > 0 && n < (1 << 20) && m > 0 && m < 4096)
        return ((long long)n << 12) + m;
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &b, &c, &end) == 3 && a == 0 && b > 0 && b < (1 << 20) && c >= 0 && c < 4096)
        return ((long long)b << 12) + c;
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &b, &c, &end) == 3 && a >= 1 && a < (1 << 20) && b >= 0 && c >= 0 &&
        b < 4096 && c < 4096)
        return (1LL << 48) + ((long long)a << 24) + ((long long)b << 12) + c;
    return -1;
}

// "Beta 2" / "Beta 5.1" / "Beta 6" (v0.6.0) / "Beta 6.1" (v0.6.1) / "1.0.0" (with_word: "Version 1.0.0")
static inline std::string tag_label(const std::string &t, bool with_word) {
    int n, m, a;
    char end;
    if (sscanf(t.c_str(), "beta_%d%c", &n, &end) == 1) return "Beta " + std::to_string(n);
    if (sscanf(t.c_str(), "beta_%d.%d%c", &n, &m, &end) == 2 && m > 0)
        return "Beta " + std::to_string(n) + "." + std::to_string(m);
    if (sscanf(t.c_str(), "v%d.%d.%d%c", &a, &n, &m, &end) == 3 && a == 0 && n > 0)
        return "Beta " + std::to_string(n) + (m > 0 ? "." + std::to_string(m) : std::string());
    if (!t.compare(0, 5, "test-")) return "Test build " + t.substr(5);
    std::string v = t.size() > 1 && t[0] == 'v' ? t.substr(1) : t;
    return with_word ? "Version " + v : v;
}
