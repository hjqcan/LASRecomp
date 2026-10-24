// Observe on the game input thread. No gameplay state is written here.
#include "../workshop.h"
#include "../research.h"
#include "../game_addresses.h"
#include "../settings.h"
#include "../rt.h"
#include <math.h>
#include <string.h>
extern int game_in_play(void);
extern uint64_t doorjump_player(uint64_t frame);
bool workshop_read_sample(RouteSample *out,bool position) {
    RouteSample s=*out;
    s.playing=s.position=s.assisted=false;s.hp=0;s.level[0]=0;s.x=s.y=s.z=0;
    const uint8_t *save=(const uint8_t *)(uintptr_t)GA(LIVE_SAVE);
    s.playing=game_in_play() && *(const uint32_t *)save==0xE && *(const uint16_t *)(save+0x5388)!=0;
    if (!s.playing) { *out=s;return true; }
    s.hp=*(const int16_t *)(save+0x5544);
    uint64_t ps=*(const uint64_t *)(uintptr_t)GA(PLAYER_STATUS_PTR);
    if (ps<GUEST_LO || ps>GUEST_HI-64) return false;
    const char *level=(const char *)(uintptr_t)ps;
    size_t n=0;
    for (;n<sizeof s.level-1 && level[n];++n) {
        char c=level[n]; if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_')) return false;
        s.level[n]=c;
    }
    if (!n || level[n]) return false;
    s.level[n]=0;
    if (position) {
        uint64_t player=doorjump_player(s.frame);
        if (player>=GUEST_LO && player<GUEST_HI-0x100) {
            const float *xyz=(const float *)(uintptr_t)(player+0x80);
            if (isfinite(xyz[0])&&isfinite(xyz[1])&&isfinite(xyz[2])&&fabsf(xyz[0])<1e7f&&fabsf(xyz[1])<1e7f&&fabsf(xyz[2])<1e7f) {
                s.x=xyz[0];s.y=xyz[1];s.z=xyz[2];s.position=true;
            }
        }
    }
    s.assisted=g_settings.cheat_hearts||g_settings.cheat_ammo||g_settings.cheat_rupees||g_settings.cheat_hookshot;
    *out=s;return true;
}
void workshop_game_tick(uint64_t frame,uint64_t buttons,int lx,int ly,int rx,int ry) {
    static uint64_t last_frame=UINT64_MAX,last_position;
    bool probe=research_active(),fresh=frame!=last_frame;
    if ((!workshop_recording()&&!probe)||(!fresh&&!probe)) return;
    last_frame=frame;
    RouteSample s={.frame=frame,.now_ns=plat_now_ns(),.buttons=buttons,.lx=lx,.ly=ly,.rx=rx,.ry=ry};
    bool route_position=s.now_ns-last_position>=100000000ull;
    if(!workshop_read_sample(&s,probe||route_position))return;
    if(s.playing&&route_position)last_position=s.now_ns;
    research_state(&s,s.playing?(const void *)(uintptr_t)(GA(LIVE_SAVE)+0x5388):NULL,s.playing?0x259:0);
    if(fresh){s.position&=route_position;workshop_observe(&s);}
}
