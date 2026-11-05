// Weather (Cosmetics tab "Weather"): rain, snow, a thunderstorm or a blizzard drawn by the renderer over the game's 3D overworld,
// looks only (no effect on the game, its logic or a seed). The particles live in the world (they stay put as the
// camera scrolls) and are hidden behind whatever the scene's depth says is in front of them; a rain drop ends in a
// splash where it meets what the depth says is under it (ground, roof, water), a snow flake settles there and fades. Only on the overworld (level "Field") while it is on screen: not indoors, in dungeons,
// on the title or file select, nor behind the pause menu (the game draws no scene composite there).
//
// This file is the backend-neutral part: the setting, when it shows, the camera, the particle volume and the storm's
// flashes and thunder. A renderer calls weather_scene_camera() for its scene draws and weather_frame() right after
// the game's scene composite into the window image (the HUD comes after it), then draws with the WeatherFrame:
//   - streaks / flakes: `count` instances of 4 vertices (a strip), instance i at a hashed spot of the volume, falling
//     through the world (the volume wraps around it), up to where it meets the scene under it (by the scene depth)
//   - splashes: `splashes` instances, each what one drop (every count / splashes-th) left where it landed, for
//     cam[3] s: rain a ring, snow the flake lying there
//   - leaves (a storm): leaf[3] instances blowing along in the wind, tumbling, landing and fading
//   - one full-screen triangle: rgb added, the picture multiplied by `keep` (overcast, lightning)
// Metal: runtime/nvn/renderer.m (weather_encode). Vulkan: not yet (the setting does nothing there).
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { WEATHER_OFF, WEATHER_RAIN, WEATHER_SNOW, WEATHER_STORM, WEATHER_BLIZZARD, WEATHER_RANDOM };

typedef struct {
    float vp[16];      // the scene camera's view-projection (rows, the game's GL clip space)
    float inv_vp[16];  // its inverse (rows)
    float origin[4];   // the volume's centre (world xyz), w: the ground's height
    float box[4];      // the volume's size across (x and z), its height (both fixed), the time (s), the wind (world x per s)
    float shape[4];    // streak length (world), streak / flake width (pixels at 1080p), fall speed (world/s), sway (world)
    float color[4];    // the particles' colour (rgb, alpha)
    float screen[4];   // the target's content size in pixels (w, h), the depth's content size (w, h)
    float flags[4];    // x: flip y (window origin lower left), y: depth -1..1, z: mirrored, w: kind (1 rain, 2 snow)
    float fill[4];     // full-screen pass: rgb added (lightning), a = what the picture keeps (1 - overcast)
    float zr[4];       // the scene depth's range (the viewport's near, far - near; the renderer fills it)
    float cam[4];      // the camera (world xyz), w: how long a splash plays / a landed flake stays (s)
    float wind[4];     // the wind: how far it has carried everything so far (world x, z; wrapped), its velocity (x, z)
    float leaf[4];     // the storm's blowing leaves: their colour (the Leaves setting's; r < 0: the game's own), w: how many
    uint32_t count, splashes;  // instances
    int kind;          // WEATHER_RAIN / WEATHER_SNOW / WEATHER_STORM / WEATHER_BLIZZARD
} WeatherFrame;

// A scene draw's camera block (vertex constant slot 5, 320 bytes: view, projection, view-projection; perspective only)
// - the renderer passes every one; the frame's first is the camera. frame: the frame being translated.
void weather_scene_camera(const float *u, uint64_t size, uint64_t frame);
// (a frame whose scene draws carry no camera block: the level model's view-projection, rows - the camera's place
// comes from its inverse)
void weather_scene_vp(const float *vp, uint64_t frame);
// After the frame's scene composite: what to draw (false: nothing - off, not outdoors, no camera this frame). The
// renderer fills screen, flags and zr first; probe: the scene depth at g_weather_probe_uv as the particle draw read it
// last frame (the ground's height; NULL: none yet) - written by its first vertex.
enum { WEATHER_PROBES = 5 };
extern const float g_weather_probe_uv[WEATHER_PROBES][2];
bool weather_frame(WeatherFrame *f, uint64_t frame, const float probe[WEATHER_PROBES]);
bool weather_probe_world(const WeatherFrame *f, int i, float d, float out[3]);
// The weather in effect now (WEATHER_OFF/RAIN/SNOW/STORM/BLIZZARD): the seed's weather cycle, else the setting, its
// random roll, or the seed's own cosmetics' pick
int weather_current(void);

// ---- the randomizer's weather cycle (seed option "weather-cycle"; runtime/nvn/weather_cycle.c): the weather
// changes on its own, saved with each save slot; the Cosmetics tab's Weather row is locked while it's on
bool weather_cycle_on(void);
int weather_cycle_kind(float *level);  // the weather showing now and how strongly (0..1: a change fading)
void weather_cycle_tick(void);         // game thread, every frame
void weather_cycle_scene(void);        // the renderer: a scene composite was drawn (the cycle's clock runs)
void weather_cycle_save_read(const char *path, uint64_t addr, int64_t off, int64_t size);     // runtime/services/fs.c
void weather_cycle_save_written(const char *path, uint64_t addr, int64_t off, int64_t size);

// ---- snow on the ground (snow only; prototype). While it snows, surfaces facing up turn white over ~1.5 minutes
// (patchy at first), it melts again in ~30 s once the weather is something else. Drawn by the renderer INSIDE the
// game's scene: after the level's own model and before the characters, grass tufts, blob shadows and water (the
// game draws them in that order), so whatever comes later stays clear of it. Link leaves footprints in it (a
// world-anchored trail map the cover reads; they fill in again as it keeps snowing) and kicks up snow as he walks
// (a burst with the Pegasus Boots).
typedef struct {
    float vp[16];      // the scene camera's view-projection (rows, the game's GL clip space)
    float inv_vp[16];  // its inverse (rows)
    float screen[4];   // the colour target's content size (w, h), the depth's content size (w, h)
    float flags[4];    // x: flip y, y: depth -1..1, z: the cover (0..1), w: the time (s)
    float zr[4];       // the scene depth's range (near, far - near), the trail map's period (world), its size (texels)
    float trail[4];    // x: how long a print takes to fill in again (s), y: test view (1 the cover's mask in red)
    float tone[4];     // the snow's colour in the scene's (linear, pre-tone-map) units (rgb), w: their reference white
    float shake[8];    // trees shaken bare (a bonk): slot 1..6's share of the cover now (stencil bits 1-3; [0] unused;
                       // [7] 1: the face trees by Crazy Tracy's, no wall for the alcoves' test)
} SnowFrame;
// The cover for this frame (false: none - not snowing / nothing settled / not the overworld). vp: the scene pass's
// view-projection (rows). The renderer fills screen, flags.xy and zr.xy.
bool weather_snow_cover(SnowFrame *s, const float vp[16]);
bool weather_snow_on(void);  // (any snow lying now: the renderer marks its scene pass for the cover)
// Footprints made since the last call (stamped into the trail map): world xyz, time stamped; the walking direction
// (radians, around y) and the foot (-1 left, 1 right)
typedef struct { float x, y, z, t; float ang, side, len, wid; } SnowPrint;
int weather_snow_prints(SnowPrint *out, int max);
// Snow kicked up at Link's feet: p xyz + birth time, v xyz + size (pixels at 1080p); `count` live slots in a ring
enum { SNOW_PUFFS = 160 };
// p: xyz + birth time; v: xyz + size (pixels at 1080p); g: x the height it falls to, y its life (s), z gravity
typedef struct { float p[4]; float v[4]; float g[4]; } SnowPuff;
int weather_snow_puffs(SnowPuff out[SNOW_PUFFS]);
float weather_snow_now(void);  // (their time base: the weather's time)
// A tree drawn this frame (the renderer: a scenery actor with canopy maps; its model's world position): which shaken
// tree slot it is (1..6, its snow regrowing after a bonk), 0: none
int weather_snow_tree(const float pos[3]);
// A scenery-looking actor drawn this frame (one drawn with the level's own shaders): whether it is animated - an
// animal (its vertex data, posed for each frame, keeps changing), not a prop or a rock (still). key: its vertex
// buffer's address, vtx / size: the data there. Render thread
bool weather_snow_animated(uint64_t key, const void *vtx, uint64_t size, uint64_t frame);
// Game thread, every frame: Link's position for the footprints and the kicked-up snow (snow on the overworld only),
// and for a storm's lightning (it aims at him now and then; it shocks him if he is still there when it strikes)
struct Cpu;
void weather_player_tick(struct Cpu *c, uint64_t frame);

// ---- a storm's lightning strike: ~1 s of a glow and sparks crackling on the ground at the target, then the bolt
// from the sky with a flash and a thunderclap, sparks, and a scorch mark that fades. The target's height is the
// scene's under it (the renderer's wx_surface); the bolt's points are relative to it.
enum { STRIKE_PTS = 40 };
typedef struct {
    float at[4];     // the target (world xyz; y: Link's feet, the shader takes the ground's), w: the time now (s)
    float tm[4];     // x: when the glow began, y: when the bolt strikes, z: a seed for the sparks, w: the points used
    float pts[STRIKE_PTS][4];  // the bolt: xyz from the target (y up), w: its width there (pixels at 1080p; 0: a break
                               // - a branch starts at the next point)
} StrikeFrame;
bool weather_strike(StrikeFrame *s);  // (false: none showing)
// Lightning that hurts: Link standing where it strikes gets a Buzz Blob's shock (the game's own damage and reaction).
// Off by default - the Cosmetics tab's weather and a seed's own are looks only; meant for a randomizer weather option
// (LARECOMP_WEATHER_HARM=1: on, for tests)
void weather_lightning_harm(bool on);
// A blizzard's gusts pushing Link a little - not its steady wind between them (through his collision: walls stop it;
// the stick works against it). Off by default, like weather_lightning_harm - for the randomizer
// (LARECOMP_WEATHER_PUSH=1: on, for tests)
void weather_wind_push(bool on);
