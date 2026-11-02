// The Warp tab's maps (runtime/ui/warp_maps.mm), drawn with ImGui draw lists from our own simple shapes: the island
// as a 16 x 16 grid of squares coloured by region with small terrain marks and an icon per place, and a dungeon as its
// real floor of rooms (warp_rooms.h) with a closer sketch of one room.
#pragma once
#include "imgui.h"
#include "warp_rooms.h"

enum WarpIcon { WI_DOOR, WI_CAVE, WI_DUNGEON, WI_STAIRS, WI_LADDER, WI_DROP, WI_WARP, WI_PHONE, WI_FAIRY, WI_NOTE, WI_PIN,
                WI_ROOM, WI_ENTRANCE, WI_BOAT, WI_N };
void warp_icon(ImDrawList *dl, int icon, ImVec2 c, float size, ImU32 col);

// One square of the island: its region (an index into regions[]) and the icons of the places on it
struct WarpIslandSquare { int region = -1; int icons[4]; int n = 0; };
struct WarpIsland {
    WarpIslandSquare sq[16][16];
    const char *regions[32];
    int nregions = 0;
};
// a: top-left, b: bottom-right; cur: the square to frame (row, col; -1: none); region: the region to light up (-1)
void warp_draw_island(ImDrawList *dl, ImVec2 a, ImVec2 b, const WarpIsland &is, int cur_r, int cur_c, int region);

// The floor: every top-down room; sel: the room to frame (-1: none), dots: arrival points to mark ({room, x, z})
struct WarpDot { int room; float x, z; bool hot; };
void warp_draw_floor(ImDrawList *dl, ImVec2 a, ImVec2 b, const WarpLevel &L, int sel, const WarpDot *dots, int ndots);
// One room (all the cells of its unit) closer, with its arrival points
void warp_draw_room(ImDrawList *dl, ImVec2 a, ImVec2 b, const WarpLevel &L, int room, const WarpDot *dots, int ndots);
