// Dungeon rooms for the Warp tab's maps (runtime/ui/warp_rooms.mm): read from the extracted level files the first time
// a dungeon is shown - each top-down room's 10 x 8 tile grid (walls, water, pits, raised floor), its doors and
// landmarks (blocks, pots, chests, stairs, switches, statues, traps), and the arrival points (TagPlayerStart) by
// locator. Rooms joined by a wide open edge or one camera zone are one room (unit). Nothing is stored in the repo: the
// layout is the player's own files'.
#pragma once
#include <string>
#include <vector>

enum WarpTile { WT_FLOOR, WT_WALL, WT_WATER, WT_DEEP };  // (WT_DEEP: deep water, lava, a pit)
enum WarpFeat {
    WF_BLOCK, WF_CBLOCK, WF_LOCKBLOCK, WF_POT, WF_CHEST, WF_STAIRS, WF_HOLE, WF_SPIKES, WF_BELT, WF_SWITCH, WF_STATUE,
    WF_TRAP, WF_INSTRUMENT, WF_DOOR_LOCK, WF_DOOR_BOSS, WF_DOOR_SHUTTER, WF_DOOR_CRACKED, WF_DOOR_ONEWAY, WF_N
};
struct WarpRoomFeat { int kind; float x, z; bool mirror = false; };  // (x, z: tiles from the room's corner, 0-10 / 0-8;
                                                                    //  mirror: a door shown from its other side)
struct WarpSpawn { std::string locator; float x, z; };
struct WarpRoom {
    std::string key;  // the file's cell part ("04B", "05HL")
    int col = 0, row = 0;  // the cell on the level's grid (from the grid's origin)
    bool side = false;  // a side-view room (ladder passages): no cell, no tiles
    unsigned char tile[80] = {};  // WarpTile, row by row
    float elev[80] = {};
    std::vector<WarpRoomFeat> feats;
    std::vector<WarpSpawn> spawns;
    int unit = -1;  // rooms with the same unit are one room (indices into WarpLevel::rooms of its first cell)
};
struct WarpLevel {
    std::string name;  // "Lv01TailCave"
    bool loaded = false, ok = false;
    std::vector<WarpRoom> rooms;  // top-down ones first, then the side-view ones
    int c0 = 0, c1 = 0, r0 = 0, r1 = 0;  // the used part of the grid (top-down rooms)
    int cell(int col, int row) const;  // the top-down room at a cell (-1: none)
    int find_spawn(const char *locator) const;  // the room holding that arrival point (-1)
};
// where a warp into a top-down room puts Link (world units): a floor tile one step inside a doorway to another room of
// the level (else the floor tile nearest the middle)
bool warp_room_landing(const WarpLevel &L, int room, float *x, float *y, float *z);
// the level's rooms (loaded on first use; ok false when its files aren't there)
const WarpLevel &warp_level(const char *level);
