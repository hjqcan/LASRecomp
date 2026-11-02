// Dungeon rooms for the Warp tab's maps (warp_rooms.h). The level files are FixedHash containers (the layout is in
// tools/rando/fixedhash.py); a room (.leb) holds its actors and, for a top-down room, a 10 x 8 tile grid (1.5 units a
// tile, origin = the cell's corner: a cell is 15 x 12 units, column -> x, row -> z). How tiles, doors and joined cells
// are read follows tools/rando/rooms.py (load_room / catalog).
#include "warp_rooms.h"

#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>

extern "C" {
#include "../hle.h"
extern const char *g_romfs_dir;
}

namespace {

struct Blob { std::vector<uint8_t> d; };
static bool read_file(const std::string &p, Blob &b) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64 << 20)) { fclose(f); return false; }
    b.d.resize((size_t)n);
    bool ok = fread(b.d.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

// One FixedHash container at `off` (bounds-checked: a damaged file gives no entries)
struct FH {
    const uint8_t *d = nullptr;
    size_t n = 0;
    uint64_t eo = 0, ds = 0, ns = 0, nsize = 0;
    int count = 0;
    template <class T> T at(uint64_t o) const { T v{}; if (o + sizeof(T) <= n) memcpy(&v, d + o, sizeof(T)); return v; }
    bool open(const uint8_t *data, size_t size, uint64_t off) {
        d = data, n = size;
        if (off + 8 > n) return false;
        uint16_t nb = at<uint16_t>(off + 2);
        eo = ((off + 8 + 4ull * (nb + 1) + 3) & ~7ull) + 8;
        uint64_t cnt = at<uint64_t>(eo - 8) / 16;
        if (eo > n || cnt > 100000) return false;
        uint64_t oo = eo + 16 * cnt + 8;
        ds = ((oo + 4 * cnt + 7) & ~7ull) + 8;
        uint64_t dsize = at<uint64_t>(ds - 8);
        ns = ((ds + dsize + 3) & ~3ull) + 4;
        nsize = at<uint32_t>(ns - 4);
        if (ds > n || ns > n || ns + nsize > n) return false;
        count = (int)cnt;
        return true;
    }
    const char *name(int i) const {
        uint16_t no = at<uint16_t>(eo + 16ull * i + 2);
        return ns + no < n && nsize ? (const char *)d + ns + no : "";
    }
    bool is_hash(int i) const { return at<uint16_t>(eo + 16ull * i) <= 0xFFED; }
    uint64_t data_off(int i) const { return ds + at<uint32_t>(eo + 16ull * i + 12); }
    bool child(int i, FH &out) const { return is_hash(i) && out.open(d, n, data_off(i)); }
    // a raw entry's bytes (pointer, size)
    const uint8_t *raw(int i, uint64_t *size) const {
        uint64_t o = data_off(i), s = at<uint64_t>(o);
        if (is_hash(i) || o + 8 + s > n) return *size = 0, nullptr;
        return *size = s, d + o + 8;
    }
    int find(const char *nm) const {
        for (int i = 0; i < count; i++)
            if (!strcmp(name(i), nm)) return i;
        return -1;
    }
    const char *str(uint32_t off) const { return off < nsize ? (const char *)d + ns + off : ""; }  // (root's names)
};

// Door actors (rooms.py DOORS): ObjLockDoor, ObjBossDoor, ObjClosedDoor, ObjOnewayDoor, the cracked / bombable walls
static int door_kind(int type) {
    switch (type) {
    case 0xED: return WF_DOOR_LOCK;
    case 0xEE: return WF_DOOR_BOSS;
    case 0xEF: return WF_DOOR_SHUTTER;
    case 0x1A3: case 0x1A4: return WF_DOOR_ONEWAY;
    case 0x121: case 0x123: case 0x131: case 0x132: case 0x133: case 0x139: case 0x20c: return WF_DOOR_CRACKED;
    }
    return -1;
}
// Landmarks by actor name (the files cut names to 12 characters: ObjPurpleUne...)
static int feat_kind(const char *nm) {
    static const struct { const char *sub; int kind; } k[] = {
        {"FullMoon", WF_INSTRUMENT}, {"ConchHorn", WF_INSTRUMENT}, {"SeaLily", WF_INSTRUMENT}, {"SurfHarp", WF_INSTRUMENT},
        {"WindMari", WF_INSTRUMENT}, {"CoralTri", WF_INSTRUMENT}, {"EveningCalm", WF_INSTRUMENT}, {"ThunderD", WF_INSTRUMENT},
        {"TreasureBox", WF_CHEST}, {"DungeonPot", WF_POT}, {"ClosedPot", WF_POT}, {"BigPot", WF_POT},
        {"LockBlock", WF_LOCKBLOCK}, {"PurpleUne", WF_CBLOCK}, {"OrangeUne", WF_CBLOCK},
        {"SquareBlock", WF_BLOCK}, {"IceBlock", WF_BLOCK}, {"CrackedBl", WF_BLOCK},
        {"Downstairs", WF_STAIRS}, {"Upstairs", WF_STAIRS}, {"DungeonSt", WF_STAIRS}, {"Lv9Stair", WF_STAIRS},
        {"HoleLowe", WF_HOLE}, {"SpikeTile", WF_SPIKES}, {"BeltConv", WF_BELT},
        {"CrystalSw", WF_SWITCH}, {"FootSwitch", WF_SWITCH}, {"Lever", WF_SWITCH},
        {"DungeonOw", WF_STATUE}, {"ElephantS", WF_STATUE}, {"HorseNeck", WF_STATUE}, {"TowerPillar", WF_STATUE},
        {"BladeTrap", WF_TRAP}, {"Beamos", WF_TRAP},
    };
    for (auto &e : k)
        if (strstr(nm, e.sub)) return e.kind;
    return -1;
}

// Eagle's Tower's rooms before the floor collapses (rooms.py DEAD): no seed or save shows them
static bool dead_room(const std::string &level, const std::string &key) {
    if (level != "Lv07EagleTower") return false;
    static const char *const k[] = {"05F", "05G", "06E", "06F", "06G", "06H", "07E", "07F", "07G", "07H", "08F", "08G", "05E"};
    for (const char *x : k)
        if (key == x) return true;
    return false;
}

static void load_room(const std::string &level, const std::string &key, const Blob &b, std::vector<WarpRoom> &top,
                      std::vector<WarpRoom> &side) {
    FH root;
    if (!root.open(b.d.data(), b.d.size(), 0)) return;
    WarpRoom rm;
    rm.key = key;
    // the grid: a top-down room's when it is 10 x 8 tiles at a cell's corner
    bool grid = false;
    float ox = 0, oz = 0;
    int gi = root.find("grid");
    FH g;
    if (gi >= 0 && root.child(gi, g)) {
        int ii = g.find("info"), di = g.find("data");
        uint64_t is = 0, dsz = 0;
        const uint8_t *info = ii >= 0 ? g.raw(ii, &is) : nullptr, *data = di >= 0 ? g.raw(di, &dsz) : nullptr;
        if (info && is >= 16 && data && dsz >= 80 * 16) {
            uint16_t h, w;
            memcpy(&h, info, 2), memcpy(&w, info + 2, 2), memcpy(&ox, info + 8, 4), memcpy(&oz, info + 12, 4);
            float fc = ox / 15.0f, fr = oz / 12.0f;
            if (h == 8 && w == 10 && fabsf(fc - roundf(fc)) < 0.01f && fabsf(fr - roundf(fr)) < 0.01f && fc >= 0 && fr >= 0) {
                grid = true;
                rm.col = (int)roundf(fc), rm.row = (int)roundf(fr);
                for (int i = 0; i < 80; i++) {
                    const uint8_t *t = data + 16 * i;
                    float e;
                    memcpy(&e, t + 12, 4);
                    rm.elev[i] = e;
                    bool wall = (t[0] & 2) && !(t[2] & 8);  // (bit 3 of byte 2: a doorway's tile, open even when it reads solid)
                    bool water = t[2] & 4;
                    rm.tile[i] = (water || (t[0] & 1)) && e < -0.3f ? WT_DEEP : water ? WT_WATER : wall ? WT_WALL : WT_FLOOR;
                }
            }
        }
    }
    rm.side = !grid;
    int ai = root.find("actor");
    FH acts;
    if (ai >= 0 && root.child(ai, acts))
        for (int i = 0; i < acts.count; i++) {
            uint64_t sz = 0;
            const uint8_t *a = acts.raw(i, &sz);
            if (!a || sz < 0x78) continue;
            uint32_t no;
            uint16_t type;
            float px, pz;
            memcpy(&no, a + 8, 4), memcpy(&type, a + 0xC, 2), memcpy(&px, a + 0x14, 4), memcpy(&pz, a + 0x1C, 4);
            float lx = (px - ox) / 1.5f, lz = (pz - oz) / 1.5f;
            if (type == 0x18F) {  // TagPlayerStart: its first parameter (a string) is the locator the warp list names
                uint32_t v, t;
                memcpy(&v, a + 0x38, 4), memcpy(&t, a + 0x3C, 4);
                if (t == 4) rm.spawns.push_back({root.str(v), lx, lz});
                continue;
            }
            if (!grid) continue;
            int k = door_kind(type);
            if (k < 0) k = feat_kind(root.str(no));
            if (k >= 0 && lx > -1 && lx < 11 && lz > -1 && lz < 9) rm.feats.push_back({k, lx, lz});
        }
    if (grid) top.push_back(std::move(rm));
    else if (!rm.spawns.empty()) side.push_back(std::move(rm));
}

static int find_root(std::vector<int> &p, int k) {
    while (p[k] != k) k = p[k] = p[p[k]];
    return k;
}

static void load_level(WarpLevel &L) {
    L.loaded = true;
    std::string dir = std::string(g_romfs_dir ? g_romfs_dir : "extracted/romfs") + "/region_common/level/" + L.name;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    for (struct dirent *e; (e = readdir(d));) {
        std::string f = e->d_name;
        if (f.size() > L.name.size() + 5 && !f.compare(0, L.name.size() + 1, L.name + "_") && !f.compare(f.size() - 4, 4, ".leb"))
            files.push_back(f);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    std::vector<WarpRoom> top, side;
    for (auto &f : files) {
        std::string key = f.substr(L.name.size() + 1, f.size() - L.name.size() - 5);
        if (dead_room(L.name, key)) continue;
        Blob b;
        if (read_file(dir + "/" + f, b)) load_room(L.name, key, b, top, side);
    }
    if (top.empty() && side.empty()) return;
    L.rooms = std::move(top);
    size_t ntop = L.rooms.size();
    for (auto &r : side) L.rooms.push_back(std::move(r));
    // a door actor stands in one of the two rooms it joins, and its tiles read as solid in both: open them, and show
    // the door from the other side too
    for (size_t i = 0; i < ntop; i++) {
        WarpRoom &a = L.rooms[i];
        for (size_t k = 0, nf = a.feats.size(); k < nf; k++) {
            WarpRoomFeat f = a.feats[k];
            if (f.kind < WF_DOOR_LOCK || f.mirror) continue;
            int dc = f.x > 8.8f ? 1 : f.x < 1.2f ? -1 : 0, dr = !dc && f.z > 6.8f ? 1 : !dc && f.z < 1.2f ? -1 : 0;
            if (!dc && !dr) continue;
            int nb = L.cell(a.col + dc, a.row + dr);
            for (int s = 0; s < 2; s++) {
                WarpRoom *r = s ? (nb >= 0 ? &L.rooms[nb] : nullptr) : &a;
                if (!r) continue;
                float p = dc ? f.z : f.x, lo = floorf(p - 0.5f), hi = floorf(p);
                int edge = dc ? ((dc > 0) != (s == 1) ? 9 : 0) : ((dr > 0) != (s == 1) ? 7 : 0);
                for (int t = (int)lo; t <= (int)hi; t++) {
                    if (t < 0 || t >= (dc ? 8 : 10)) continue;
                    int idx = dc ? t * 10 + edge : edge * 10 + t;
                    if (r->tile[idx] == WT_WALL) r->tile[idx] = WT_FLOOR;
                }
                if (s) r->feats.push_back({f.kind, f.x - 10.0f * dc, f.z - 8.0f * dr, true});
            }
        }
    }
    // joined cells: a shared edge with 3+ open tiles on both sides ...
    std::vector<int> par(ntop);
    for (size_t i = 0; i < ntop; i++) par[i] = (int)i;
    auto open_t = [](const WarpRoom &r, int row, int col) { return r.tile[row * 10 + col] != WT_WALL; };
    for (size_t i = 0; i < ntop; i++) {
        const WarpRoom &a = L.rooms[i];
        int e = L.cell(a.col + 1, a.row), s = L.cell(a.col, a.row + 1);
        if (e >= 0) {
            int n = 0;
            for (int row = 0; row < 8; row++) n += open_t(a, row, 9) && open_t(L.rooms[e], row, 0);
            if (n >= 3) par[find_root(par, (int)i)] = find_root(par, e);
        }
        if (s >= 0) {
            int n = 0;
            for (int col = 0; col < 10; col++) n += open_t(a, 7, col) && open_t(L.rooms[s], 0, col);
            if (n >= 3) par[find_root(par, (int)i)] = find_root(par, s);
        }
    }
    // ... and cells of one camera zone (the level's .lvb: its areas, and each zone's list of area indices)
    Blob lb;
    FH root, areas, zones;
    if (read_file(dir + "/" + L.name + ".lvb", lb) && root.open(lb.d.data(), lb.d.size(), 0)) {
        int ai = root.find("area"), zi = root.find("zone");
        if (ai >= 0 && zi >= 0 && root.child(ai, areas) && root.child(zi, zones))
            for (int z = 0; z < zones.count; z++) {
                uint64_t sz = 0;
                const uint8_t *zd = zones.raw(z, &sz);
                if (!zd || sz < 0x9c + 7) continue;
                int n = zd[0x9c + 6], first = -1;
                for (int k = 0; k < n && 0x9c + 7 + (uint64_t)k < sz; k++) {
                    int idx = zd[0x9c + 7 + k];
                    if (idx >= areas.count) continue;
                    std::string an = areas.name(idx);
                    if (an.compare(0, L.name.size() + 1, L.name + "_")) continue;
                    an = an.substr(L.name.size() + 1);
                    for (size_t i = 0; i < ntop; i++)
                        if (L.rooms[i].key == an) {
                            if (first < 0) first = (int)i;
                            else par[find_root(par, (int)i)] = find_root(par, first);
                        }
                }
            }
    }
    for (size_t i = 0; i < ntop; i++) L.rooms[i].unit = find_root(par, (int)i);
    L.c0 = L.r0 = 1 << 20, L.c1 = L.r1 = -1;
    for (size_t i = 0; i < ntop; i++) {
        const WarpRoom &r = L.rooms[i];
        L.c0 = std::min(L.c0, r.col), L.c1 = std::max(L.c1, r.col), L.r0 = std::min(L.r0, r.row), L.r1 = std::max(L.r1, r.row);
    }
    if (!ntop) L.c0 = L.r0 = 0, L.c1 = L.r1 = 0;
    L.ok = true;
    LOG("[warp] %s: %zu rooms (%zu seen from the side)\n", L.name.c_str(), L.rooms.size(), L.rooms.size() - ntop);
}

}  // namespace

int WarpLevel::cell(int col, int row) const {
    for (size_t i = 0; i < rooms.size(); i++)
        if (!rooms[i].side && rooms[i].col == col && rooms[i].row == row) return (int)i;
    return -1;
}
int WarpLevel::find_spawn(const char *locator) const {
    for (size_t i = 0; i < rooms.size(); i++)
        for (auto &s : rooms[i].spawns)
            if (s.locator == locator) return (int)i;
    return -1;
}

const WarpLevel &warp_level(const char *level) {
    static std::map<std::string, WarpLevel> g_levels;
    WarpLevel &L = g_levels[level];
    if (!L.loaded) L.name = level, load_level(L);
    return L;
}

bool warp_room_landing(const WarpLevel &L, int room, float *x, float *y, float *z) {
    if (room < 0 || room >= (int)L.rooms.size() || L.rooms[room].side) return false;
    const WarpRoom &r = L.rooms[room];
    auto at = [&](int row, int col) { return r.tile[row * 10 + col]; };
    int best_r = -1, best_c = -1, best = 1 << 30;
    // doorways: an open edge tile with a floor tile one step in, toward a room of the level (not the way out)
    static const int side_d[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};  // N S W E as (row, col) steps outward
    for (int s = 0; s < 4; s++) {
        int dr = side_d[s][0], dc = side_d[s][1];
        bool to_room = L.cell(r.col + dc, r.row + dr) >= 0;
        int n = dr ? 10 : 8;
        for (int k = 0; k < n; k++) {
            int er = dr < 0 ? 0 : dr > 0 ? 7 : k, ec = dc < 0 ? 0 : dc > 0 ? 9 : k;
            int ir = er - dr, ic = ec - dc;
            if (at(er, ec) == WT_WALL || at(ir, ic) != WT_FLOOR) continue;
            int score = (to_room ? 0 : 1000) + abs(2 * ir - 7) + abs(2 * ic - 9);  // (a doorway to another room, then central)
            if (score < best) best = score, best_r = ir, best_c = ic;
        }
    }
    if (best_r < 0)
        for (int i = 0; i < 80; i++)
            if (r.tile[i] == WT_FLOOR) {
                int score = abs(2 * (i / 10) - 7) + abs(2 * (i % 10) - 9);
                if (score < best) best = score, best_r = i / 10, best_c = i % 10;
            }
    if (best_r < 0) return false;
    *x = r.col * 15.0f + (best_c + 0.5f) * 1.5f;
    *z = r.row * 12.0f + (best_r + 0.5f) * 1.5f;
    *y = r.elev[best_r * 10 + best_c];
    return true;
}
