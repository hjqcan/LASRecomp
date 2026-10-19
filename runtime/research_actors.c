#include "research_actors.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool text(const unsigned char *data,size_t size,char *out) {
    for(size_t i=0;i<size;++i) {
        unsigned char c=data[i];out[i]=(char)c;
        if(!c)return i>0;
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c==':'))return false;
    }
    return false;
}
static uint64_t word(const void *bytes){uint64_t v;memcpy(&v,bytes,sizeof v);return v;}
static int order(const void *a,const void *b) {
    uint64_t x=((const ResearchActor *)a)->key,y=((const ResearchActor *)b)->key;
    return (x>y)-(x<y);
}
void research_collect_actors(uint64_t registry,ResearchRead read,void *context,ResearchActorSnapshot *out) {
    memset(out,0,sizeof *out);out->issue=RESEARCH_ACTORS_UNREADABLE;
    unsigned char header[32],after[32];
    if(!registry||!read||!read(context,registry,header,sizeof header))return;
    uint64_t total=word(header+24),node=word(header+16);
    if(total>2048){out->issue=RESEARCH_ACTORS_LIMIT;return;}
    out->registry_count=(unsigned)total;
    unsigned walked=0;
    while(node) {
        if(walked++>=total){out->issue=RESEARCH_ACTORS_LAYOUT;return;}
        unsigned char link[32],actor[0x88];
        if(!read(context,node,link,sizeof link))return;
        uint64_t object=word(link+24),key=word(link+16);node=word(link);
        if(!object)continue;
        if(!read(context,object,actor,sizeof actor))return;
        char name[64];
        if(!text(actor+0x18,sizeof name,name)){out->issue=RESEARCH_ACTORS_LAYOUT;return;}
        if(strncmp(name,"Npc",3)&&strncmp(name,"Enemy",5))continue;
        const char *suffix=strrchr(name,'-');char *end;
        if(!suffix||strlen(++suffix)!=16||strtoull(suffix,&end,16)!=key||*end){out->issue=RESEARCH_ACTORS_LAYOUT;return;}
        if(out->count==RESEARCH_MAX_ACTORS){out->issue=RESEARCH_ACTORS_LIMIT;return;}
        ResearchActor *a=&out->actors[out->count++];a->key=key;a->vtable=word(actor);strcpy(a->name,name);
        unsigned char component[0x8c];uint64_t p=word(actor+0x80);
        if(p&&read(context,p,component,sizeof component)&&text(component+0x18,sizeof a->component,a->component)) {
            memcpy(a->xyz,component+0x80,sizeof a->xyz);a->position=true;
            for(int i=0;i<3;++i)if(!isfinite(a->xyz[i])||fabsf(a->xyz[i])>=1e7f)a->position=false;
            if(!a->position)memset(a->xyz,0,sizeof a->xyz);
        } else a->component[0]=0;
    }
    if(walked!=total){out->issue=RESEARCH_ACTORS_LAYOUT;return;}
    if(!read(context,registry,after,sizeof after))return;
    if(memcmp(header,after,sizeof header)){out->issue=RESEARCH_ACTORS_CHANGED;return;}
    qsort(out->actors,out->count,sizeof *out->actors,order);
    for(unsigned i=1;i<out->count;++i)if(out->actors[i-1].key==out->actors[i].key){out->issue=RESEARCH_ACTORS_LAYOUT;return;}
    out->issue=RESEARCH_ACTORS_OK;out->complete=true;
}
