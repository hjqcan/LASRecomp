#include "workshop.h"
#include <string.h>
static bool identifier(const char *s,size_t cap,bool slug) {
    size_t n=0;
    for (;n<cap && s[n];++n) {
        char c=s[n];
        if (!((c>='a'&&c<='z')||(!slug&&c>='A'&&c<='Z')||(n&&c>='0'&&c<='9')||(n&&(c=='_'||(slug&&c=='-'))))) return false;
    }
    return n>0 && n<cap;
}
bool challenge_config_valid(const ChallengeConfig *c) {
    return c && identifier(c->name,sizeof c->name,true) && identifier(c->goal_level,sizeof c->goal_level,false) &&
        (!memcmp(c->game_version,"1.0.0\0",6)||!memcmp(c->game_version,"1.1.0\0",6)) &&
        c->seconds>=10 && c->seconds<=3600 && c->damage>=1 && c->damage<=8;
}
void challenge_begin(ChallengeState *s) { memset(s,0,sizeof *s); s->status=CHALLENGE_WAITING; }
void challenge_clock(ChallengeState *s,const ChallengeConfig *c,uint64_t now_ns) {
    if (s->status!=CHALLENGE_RUNNING || now_ns<s->start_ns) return;
    double elapsed=(now_ns-s->start_ns)/1e9;
    if (elapsed<s->elapsed) return;
    s->elapsed=elapsed;
    if (s->elapsed>=c->seconds) { s->status=CHALLENGE_LOST; s->reason=CHALLENGE_TIME; }
}
void challenge_step(ChallengeState *s,const ChallengeConfig *c,const RouteSample *o) {
    if (s->status==CHALLENGE_WAITING && o->playing && o->hp>0) { s->status=CHALLENGE_RUNNING; s->start_ns=o->now_ns; s->last_hp=o->hp; }
    challenge_clock(s,c,o->now_ns);
    if (s->status!=CHALLENGE_RUNNING || !o->playing) return;
    if (o->hp<=0) { s->status=CHALLENGE_LOST; s->reason=CHALLENGE_HEALTH; }
    else if (c->no_hit && o->hp<s->last_hp) { s->status=CHALLENGE_LOST; s->reason=CHALLENGE_HIT; }
    else if (!strcmp(o->level,c->goal_level)) s->status=CHALLENGE_WON;
    s->last_hp=o->hp;
}
