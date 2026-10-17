#include "research_gate.h"
#include <math.h>
#include <string.h>

bool research_gate_begin(ResearchGate *g,const char *level,unsigned warmup,unsigned stable) {
    if(!g||!level||!*level||strlen(level)>=sizeof g->level||warmup>3600||!stable||stable>3600)return false;
    for(const char *p=level;*p;++p)
        if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'))return false;
    memset(g,0,sizeof *g);strcpy(g->level,level);g->warmup_required=warmup;g->stable_required=stable;return true;
}
bool research_gate_step(ResearchGate *g,const RouteSample *s,bool observed,bool blocked) {
    if(g->ready)return true;
    bool level=observed&&s->playing&&!strcmp(g->level,s->level);
    if(level)g->holding=true; // Once the selected level appears, bootstrap input stays suppressed.
    bool valid=level&&!blocked&&s->position&&isfinite(s->x)&&isfinite(s->y)&&isfinite(s->z);
    if(!valid){g->warmup=g->stable=0;g->previous_valid=false;return false;}
    if(g->warmup<3600)++g->warmup;
    bool same=g->previous_valid&&g->previous.hp==s->hp&&fabsf(g->previous.x-s->x)<=1e-6f&&
              fabsf(g->previous.y-s->y)<=1e-6f&&fabsf(g->previous.z-s->z)<=1e-6f;
    g->stable=same?g->stable+1:1;
    g->previous=*s;g->previous_valid=true;
    g->ready=g->warmup>=g->warmup_required&&g->stable>=g->stable_required;
    return g->ready;
}
