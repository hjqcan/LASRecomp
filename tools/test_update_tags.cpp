// Update check tags (runtime/ui/update_tags.h): their order and labels; the macOS build numbers
// (tools/bundle_version.sh) go up in the same order. No game, no network.
// Build + run (from the repo root): clang++ -std=c++17 -O1 tools/test_update_tags.cpp -o build/test-update-tags &&
// build/test-update-tags
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "../runtime/ui/update_tags.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// tools/bundle_version.sh <tag> [<test-of>] -> CFBundleVersion ("" when it fails)
static std::string bundle_build(const char *tag, const char *test_of = "") {
    std::string cmd = std::string("sh tools/bundle_version.sh '") + tag + "' '" + test_of + "' 2>/dev/null";
    FILE *f = popen(cmd.c_str(), "r");
    char shortv[64] = "", build[64] = "";
    int n = f ? fscanf(f, "%63s %63s", shortv, build) : 0;
    int rc = f ? pclose(f) : -1;
    return n == 2 && rc == 0 ? build : "";
}

// macOS compares bundle versions part by part, numerically (a missing part is 0): <0, 0, >0
static int bundle_cmp(const std::string &a, const std::string &b) {
    std::vector<long> x, y;
    for (const char *p = a.c_str(); *p; p += *p == '.') x.push_back(strtol(p, (char **)&p, 10));
    for (const char *p = b.c_str(); *p; p += *p == '.') y.push_back(strtol(p, (char **)&p, 10));
    for (size_t i = 0; i < x.size() || i < y.size(); i++) {
        long u = i < x.size() ? x[i] : 0, v = i < y.size() ? y[i] : 0;
        if (u != v) return u < v ? -1 : 1;
    }
    return 0;
}

int main(void) {
    // oldest to newest; equal ranks share a group
    const char *order[][2] = {
        {"beta_1", "v0.1.0"}, {"beta_4", NULL}, {"beta_5", "v0.5.0"}, {"beta_5.1", "v0.5.1"}, {"beta_5.2", NULL},
        {"beta_5.3", "v0.5.3"}, {"beta_5.4", NULL}, {"v0.6.0", "beta_6"}, {"v0.6.1", "beta_6.1"}, {"v0.6.12", NULL},
        {"v0.7.0", NULL}, {"v0.99.0", NULL}, {"v1.0.0", NULL}, {"v1.0.1", NULL}, {"v1.1.0", NULL}, {"v2.0.0", NULL},
    };
    int n = sizeof order / sizeof *order;
    for (int i = 0; i < n; i++) {
        long long r = tag_rank(order[i][0]);
        CHECK(r > 0, "%s ranks (%lld)", order[i][0], r);
        if (order[i][1]) CHECK(tag_rank(order[i][1]) == r, "%s == %s", order[i][1], order[i][0]);
        for (int j = i + 1; j < n; j++)
            CHECK(tag_rank(order[j][0]) > r, "%s > %s", order[j][0], order[i][0]);
    }
    // macOS build numbers: one number that goes up with every release (equal ranks: the same number), above what
    // Beta 5.2 shipped (0.9.5.2); a test build sits between the release before the one it previews and that release
    for (int i = 0; i < n; i++) {
        std::string b = bundle_build(order[i][0]);
        CHECK(!b.empty() && b.find('.') == std::string::npos, "%s: build number '%s'", order[i][0], b.c_str());
        CHECK(bundle_cmp(b, "0.9.5.2") > 0, "%s: build %s above Beta 5.2's 0.9.5.2", order[i][0], b.c_str());
        if (order[i][1]) CHECK(bundle_build(order[i][1]) == b, "%s build == %s build", order[i][1], order[i][0]);
        if (i + 1 < n) {
            std::string c = bundle_build(order[i + 1][0]);
            CHECK(bundle_cmp(c, b) > 0, "%s build %s > %s build %s", order[i + 1][0], c.c_str(), order[i][0], b.c_str());
        }
    }
    CHECK(bundle_build("beta_5.3") == "503" && bundle_build("v0.6.0") == "600" && bundle_build("v1.0.0") == "10000",
          "build numbers 503 / 600 / 10000");
    {
        std::string t = bundle_build("test-foo", "beta_5.4");
        CHECK(bundle_cmp(t, bundle_build("beta_5.3")) > 0 && bundle_cmp(t, bundle_build("beta_5.4")) < 0,
              "test build of beta_5.4: %s between 5.3 and 5.4", t.c_str());
        CHECK(bundle_build("test-foo").empty() && bundle_build("alpha_3").empty(), "no build number without a release");
    }
    const char *never[] = {"alpha_3", "test-388-grass", "beta_0", "beta_5.0", "v0.0.1", "v1.0", "v0.6.0-rc1", "beta_5.3x",
                           "ci-test-1", "", "v", "beta_", "experimental", "experimental-202610302315-abc1234"};
    for (const char *t : never) CHECK(tag_rank(t) == -1, "'%s' is not a release (%lld)", t, tag_rank(t));

    const char *labels[][3] = {  // tag, label, with the word
        {"beta_5", "Beta 5", "Beta 5"},
        {"beta_5.3", "Beta 5.3", "Beta 5.3"},
        {"v0.6.0", "Beta 6", "Beta 6"},
        {"v0.6.1", "Beta 6.1", "Beta 6.1"},
        {"v0.10.2", "Beta 10.2", "Beta 10.2"},
        {"v1.0.0", "1.0.0", "Version 1.0.0"},
        {"v1.2.3", "1.2.3", "Version 1.2.3"},
        {"test-foo", "Test build foo", "Test build foo"},
        {"experimental", "Experimental build", "Experimental build"},
        {"experimental-202610302315-abc1234", "Experimental build 2026-10-30", "Experimental build 2026-10-30"},
    };
    for (auto &l : labels) {
        std::string a = tag_label(l[0], false), b = tag_label(l[0], true);
        CHECK(a == l[1], "%s -> '%s', want '%s'", l[0], a.c_str(), l[1]);
        CHECK(b == l[2], "%s (with word) -> '%s', want '%s'", l[0], b.c_str(), l[2]);
    }
    {  // the newest experimental build of one platform, from asset names
        std::string body = "\"name\":\"LAS.Recomp-experimental-202610302315-abc1234-linux-aarch64.AppImage\"\n"
                           "\"name\":\"LAS.Recomp-experimental-202611011200-def5678-linux-aarch64.AppImage\"\n"
                           "\"name\":\"LAS.Recomp-experimental-202612010000-0123456-windows-arm64.zip\"\n"
                           "\"name\":\"LAS.Recomp-experimental-2026120100-0123456-linux-aarch64.AppImage\"\n"
                           "\"name\":\"LAS.Recomp-experimental-202612020000-0123456-linux-aarch64x.AppImage\"\n"
                           "\"name\":\"LAS.Recomp-beta_5.4-linux-aarch64.AppImage\"\n";
        CHECK(newest_experimental(body, "linux-aarch64") == 202611011200LL, "linux-aarch64: %lld",
              newest_experimental(body, "linux-aarch64"));
        CHECK(newest_experimental(body, "windows-arm64") == 202612010000LL, "windows-arm64");
        CHECK(newest_experimental(body, "linux-x86_64") == 0 && newest_experimental("", "linux-aarch64") == 0, "none");
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("update tags: all passed\n");
    return 0;
}
