// Experimental local co-op: P1 owns the world; P2 drives the existing BowWow companion.
// Only idle/follow decisions change. Native movement, collision and attack states still run.
#include "../hle.h"
#include "../game_addresses.h"
#include "../gameversion.h"
#include "../workshop.h"
#include "../rando.h"
#include "../coop.h"
#include "../title_profile.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

// Verified 1.0.0 entries; never translated to another version's addresses.
#define BOWWOW_VTABLE UINT64_C(0x7001bdb560)
#define IDLE_TICK UINT64_C(0x700092b260)
#define FOLLOW_TICK UINT64_C(0x700092be90)
#define CHANGE_STATE UINT64_C(0x7000927b80)
#define MOVE_TARGET UINT64_C(0x700091bbc0)
#define TRY_ATTACK UINT64_C(0x7000932710)
#define ADD_TARGET UINT64_C(0x70009411e0)
#define ANIMATE UINT64_C(0x7000931d90)

static _Atomic bool enabled, requested;
static _Atomic uint64_t host_input;
static uint64_t companion, controlled_steps, last_frame, warmup;
static bool called, trace, enemy_created;
static uint64_t test_enemy, test_ctrl;
static struct { unsigned start, length; int x,z; bool attack; } script[64];
static unsigned script_count;
static bool scripted;
extern uint64_t kernel_heap_size(void);
extern uint64_t renderer_present_count(void);
extern int game_in_play(void);
extern bool menu_input_captured(void);

static bool option(const char *name) { const char *v=getenv(name);return v&&!strcmp(v,"1"); }
bool coop_active(void) { return enabled; }
bool coop_requested(void) { return requested||option("LARECOMP_COOP"); }
void coop_request(bool on) { requested=on; }
void coop_host_input(int32_t x,int32_t y,bool attack) {
    x=x>32767?32767:x<-32767?-32767:x;
    y=y>32767?32767:y<-32767?-32767:y;
    host_input=(uint16_t)x|((uint64_t)(uint16_t)y<<16)|((uint64_t)attack<<32);
}
static void clear(void) {
    enabled=false;host_input=0;companion=controlled_steps=last_frame=warmup=0;
    called=trace=enemy_created=scripted=false;script_count=0;test_enemy=test_ctrl=0;
}
RT_ON_RESET(coop_reset) { clear(); }

static bool read_script(void) {
    const char *value=getenv("LARECOMP_COOP_INPUT");
    if(!value)return true;
    scripted=true;
    if(strlen(value)>=2048)return false;
    char buffer[2048];strcpy(buffer,value);
    for(char *part=buffer;part&&*part;) {
        char *next=strchr(part,',');if(next)*next++=0;
        unsigned start=0,length=0;char key[16];int used=0;
        if(script_count==64||sscanf(part,"%u:%15[A-Z]:%u%n",&start,key,&length,&used)!=3||part[used]||
           !length||start>10000000||length>10000000)return false;
        int x=0,z=0;bool attack=false;
        if(!strcmp(key,"RIGHT"))x=1;else if(!strcmp(key,"LEFT"))x=-1;
        else if(!strcmp(key,"UP"))z=-1;else if(!strcmp(key,"DOWN"))z=1;
        else if(!strcmp(key,"ATTACK"))attack=true;else if(strcmp(key,"WAIT"))return false;
        script[script_count++]=(typeof(script[0])){start,length,x,z,attack};part=next;
    }
    return true;
}
bool coop_boot(char *why,size_t cap) {
    clear();
    if(!coop_requested())return true;
    if(!title_is_las()||!workshop_save_dir()||rando_seed_dir()||game_version_current()!=GAME_100) {
        snprintf(why,cap,"Local co-op requires vanilla 1.0.0 and an isolated Workshop save.");return false;
    }
    static const struct { uint64_t address;uint32_t words[4]; } guards[]={
        {IDLE_TICK,{0xd101c3ff,0xfd0013ea,0x6d02a3e9,0xf9001ff7}},
        {FOLLOW_TICK,{0xd10143ff,0xfd0013e8,0xf90017f5,0xa9034ff4}},
        {CHANGE_STATE,{0xd100c3ff,0xf9000bf3,0xa9027bfd,0x910083fd}},
        {MOVE_TARGET,{0xd10203ff,0xf9002bf5,0xa9064ff4,0xa9077bfd}},
        {TRY_ATTACK,{0xd10103ff,0xf9000bf5,0xa9024ff4,0xa9037bfd}},
        {ADD_TARGET,{0xf81a0ffb,0xa90167fa,0xa9025ff8,0xa90357f6}},
        {ANIMATE,{0xd10143ff,0xa9034ff4,0xa9047bfd,0x910103fd}},
    };
    for(size_t i=0;i<sizeof guards/sizeof guards[0];++i) {
        if(!dispatch_lookup(guards[i].address)||memcmp((const void *)(uintptr_t)guards[i].address,guards[i].words,16)) {
            snprintf(why,cap,"Local co-op callback guard failed (%zu).",i);return false;
        }
    }
    if(!read_script()){snprintf(why,cap,"Invalid LARECOMP_COOP_INPUT script.");return false;}
    trace=option("LARECOMP_COOP_TRACE");enabled=true;
    LOG("[coop] isolated 1.0.0 companion prototype enabled\n");return true;
}
static bool heap(uint64_t p,size_t size) {
    uint64_t total=kernel_heap_size();
    if(!total||total>HEAP_MAX)return false;
    uint64_t limit=HEAP_BASE+total;
    return p>=HEAP_BASE&&p<=limit&&size<=limit-p;
}
static uint64_t player_actor(void) {
    uint64_t scene=LD64(GA_GAME_MGR);
    uint64_t player=heap(scene,0x4c0)?LD64(scene+0x4b8):0;
    return heap(player,0x1ba8)&&!(LD8(player+0x43)&0x40)?player:0;
}
static uint64_t owned_actor(void) {
    uint64_t player=player_actor(),follower=player?LD64(player+0x1b98):0;
    uint64_t actor=follower>=0x2a8?follower-0x2a8:0;
    return heap(actor,0xa80)&&LD64(actor)==BOWWOW_VTABLE&&!(LD8(actor+0x43)&0x40)?actor:0;
}
static bool position(uint64_t actor,float out[4]) {
    if(!heap(actor,0x88))return false;
    uint64_t transform=LD64(actor+0x80);
    if(!heap(transform,0x90))return false;
    memcpy(out,(const void *)(uintptr_t)(transform+0x80),16);
    return isfinite(out[0])&&isfinite(out[1])&&isfinite(out[2]);
}
// Native target insertion owns references and deduplicates actors. The native selector
// checks eligibility/range; the host never writes enemy health or reference counts.
static void register_targets(Cpu *c,uint64_t actor,const float origin[4]) {
    uint64_t map=LD64(GA_ACTOR_REGISTRY);if(!heap(map,32))return;
    uint64_t node=LD64(map+16);unsigned count=0;
    for(;node&&count++<2048;) {
        if(!heap(node,0x28))return;
        uint64_t next=LD64(node),other=LD64(node+0x18);float pos[4];
        if(heap(other,0xa8)&&!memcmp((const void *)(uintptr_t)(other+0x18),"Enemy",5)&&
           !(LD8(other+0x43)&0x40)&&position(other,pos)) {
            float dx=pos[0]-origin[0],dz=pos[2]-origin[2];
            if(dx*dx+dz*dz<=36) { c->x[0]=actor;c->x[1]=node+0x18;guest_call(c,ADD_TARGET); }
        }
        node=next;
    }
}
static void drive(Cpu *c,uint64_t actor) {
    uint64_t movement=LD64(actor+0x2e0);float pos[4],host[4];
    if(!heap(movement,0x190)||!position(actor,pos)||!position(player_actor(),host))return;
    bool playable=game_in_play()&&!menu_input_captured();
    uint64_t sample=host_input;
    float dx=(int16_t)sample/32767.0f,dz=-(int16_t)(sample>>16)/32767.0f;
    bool attack=(sample>>32)&1;
    if(scripted) {
        dx=dz=0;attack=false;
        for(unsigned i=0;i<script_count;++i) {
            if(controlled_steps>=script[i].start&&controlled_steps-script[i].start<script[i].length) {
                dx+=script[i].x;dz+=script[i].z;attack|=script[i].attack;
            }
        }
    }
    float magnitude=sqrtf(dx*dx+dz*dz);
    if(magnitude<0.2f)dx=dz=0;else if(magnitude>1){dx/=magnitude;dz/=magnitude;}
    // Keep P2 near the P1 camera, including when P1 walks away. Pathfinding handles obstacles.
    float hx=host[0]-pos[0],hz=host[2]-pos[2],distance=sqrtf(hx*hx+hz*hz);
    if(distance>5.5f){dx=hx/distance;dz=hz/distance;attack=false;}
    else if(distance>4.5f&&dx*hx+dz*hz<0)dx=dz=0;
    if(!playable){dx=dz=0;attack=false;}else ++controlled_steps;
    if(attack) {
        if(controlled_steps%15==0)register_targets(c,actor,pos);
        c->x[0]=actor;guest_call(c,TRY_ATTACK);
        if(c->x[0]) {
            if(trace)LOG("[coop-attack] step %llu target %llx state %u\n",controlled_steps,LD64(actor+0x9d0),LD8(actor+0x61d));
            return;
        }
    }
    bool moving=dx||dz;unsigned desired=moving?1:0;
    if(LD8(actor+0x61d)!=desired){c->x[0]=actor;c->x[1]=desired;guest_call(c,CHANGE_STATE);}
    float target[4];memcpy(target,pos,sizeof target);target[0]+=dx*0.6f;target[2]+=dz*0.6f;
    uint64_t sp=c->sp;c->sp-=32;memcpy((void *)(uintptr_t)c->sp,target,16);
    c->x[0]=movement;c->x[1]=c->sp;guest_call(c,MOVE_TARGET);c->sp=sp;
    float speed=moving?3.0f:0;memcpy((void *)(uintptr_t)(movement+0x98),&speed,4);
    if(heap(LD64(actor+0x2b0),0x330)){c->x[0]=actor;guest_call(c,ANIMATE);}
    if(trace&&controlled_steps%30==0)
        LOG("[coop-control] step %llu dir %.2f %.2f state %u pos %.3f %.3f distance %.3f\n",
            controlled_steps,dx,dz,LD8(actor+0x61d),pos[0],pos[2],distance);
}
// Explicit native acceptance fixture. Retaining the factory result makes the death
// observation valid after registry removal. Normal co-op never creates enemies.
static void enemy_fixture(Cpu *c,uint64_t actor) {
    if(!option("LARECOMP_COOP_TEST_ENEMY"))return;
    Cpu saved=*c;float pos[4];
    if(!enemy_created&&controlled_steps>90&&position(actor,pos)) {
        enemy_created=true;c->sp-=0x240;uint64_t out=c->sp,info=out+0x20;
        memset((void *)(uintptr_t)c->sp,0,0x240);
        ST64(info,UINT64_C(0x7b434f4f50000001));ST16(info+8,2);ST32(info+12,UINT32_MAX);
        float *t=(void *)(uintptr_t)(info+0x180);memcpy(t,pos,12);t[0]+=1.4f;t[1]=0.1f;t[7]=1;t[8]=t[9]=t[10]=1;
        c->x[0]=out;c->x[1]=LD64(GA_GAME_MGR)+0x498;c->x[2]=info;guest_call(c,GA_CREATE_ACTOR);
        test_enemy=LD64(out);test_ctrl=LD64(out+8);
        LOG("[coop-test] enemy %llx ctrl %llx\n",test_enemy,test_ctrl);
    }
    if(test_enemy&&heap(test_enemy,0xa8)) {
        if(last_frame%60==0)LOG("[coop-test-enemy] flags %#x\n",LD32(test_enemy+0x40));
        if(LD8(test_enemy+0x43)&0x40) {
            LOG("[coop-test] enemy defeated after %llu controlled steps\n",controlled_steps);
            if(test_ctrl){c->x[0]=test_ctrl;guest_call(c,GA_SP_RELEASE);}test_enemy=test_ctrl=0;
        }
    }
    *c=saved;
}
void coop_game_tick(Cpu *c) {
    if(!enabled)return;
    uint64_t frame=renderer_present_count();if(frame==last_frame)return;last_frame=frame;
    if(!game_in_play()){warmup=frame;companion=0;return;}
    if(frame-warmup<120)return;
    if(!called){extern void pets_call(int);pets_call(1);called=true;}
    uint64_t actor=owned_actor();
    if(actor!=companion){companion=actor;LOG("[coop] follower %llx player %llx\n",actor,player_actor());}
    if(!actor)return;
    enemy_fixture(c,actor);
    float pos[4];
    if(trace&&frame%60==0&&position(actor,pos))
        LOG("[coop] f%llu actor %llx pos %.3f %.3f %.3f state %u\n",frame,actor,pos[0],pos[1],pos[2],LD8(actor+0x61d));
}
static void controlled_tick(Cpu *c) {
    uint64_t actor=c->x[0];
    // Revalidate ownership each call: cached addresses may be reused after a room unload.
    if(actor&&actor==owned_actor())drive(c,actor);
    else dispatch_lookup(c->pc)(c);
}
GuestFn coop_dispatch_override(uint64_t address) {
    return enabled&&(address==IDLE_TICK||address==FOLLOW_TICK)?controlled_tick:NULL;
}
