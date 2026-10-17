#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char name[64], game_version[16], goal_level[64];
    int seconds, damage;
    bool no_hit, ohko;
} ChallengeConfig;
enum { CHALLENGE_IDLE, CHALLENGE_WAITING, CHALLENGE_RUNNING, CHALLENGE_WON, CHALLENGE_LOST, CHALLENGE_CANCELLED };
enum { CHALLENGE_REASON_NONE, CHALLENGE_TIME, CHALLENGE_HEALTH, CHALLENGE_HIT };
typedef struct { int status, reason, last_hp; uint64_t start_ns; double elapsed; } ChallengeState;
typedef struct {
    uint64_t now_ns, frame, buttons;
    char level[64];
    float x,y,z;
    int hp, lx,ly,rx,ry;
    bool playing, position, assisted;
} RouteSample;
bool challenge_config_valid(const ChallengeConfig *c);
void challenge_begin(ChallengeState *s);
void challenge_clock(ChallengeState *s,const ChallengeConfig *c,uint64_t now_ns);
void challenge_step(ChallengeState *s,const ChallengeConfig *c,const RouteSample *sample);
bool workshop_config_read(const char *path,ChallengeConfig *out,char *why,size_t cap);
bool workshop_config_write(const char *path,const ChallengeConfig *config,char *why,size_t cap);
bool workshop_prepare_run(const char *data,const char *source,const ChallengeConfig *config,
                          char *run,size_t run_cap,char *why,size_t cap);
bool workshop_route_write(const char *run,const RouteSample *samples,size_t count,const ChallengeState *state,
                          const char *seed,bool assisted,char *why,size_t cap);
int workshop_route_read(const char *path,RouteSample *out,size_t max,char *why,size_t cap);
void workshop_selection(const char *root,const char *seed);
bool workshop_boot(const char *root,char *why,size_t cap);
void workshop_reset(void); // main thread, after guest threads have stopped
const char *workshop_save_dir(void); // immutable throughout a boot
bool workshop_recording(void);
void workshop_observe(const RouteSample *sample);
void workshop_frame(uint64_t frame);
void workshop_game_tick(uint64_t frame,uint64_t buttons,int lx,int ly,int rx,int ry);
bool workshop_read_sample(RouteSample *sample,bool position); // guest thread; preserves frame/time/input fields
int workshop_damage(int base); // -1: no override
void workshop_mark_assisted(void);
bool workshop_overlay_wanted(void);
void workshop_overlay(float scale);
void workshop_draw(float scale,bool in_game);
#ifdef __cplusplus
}
#endif
