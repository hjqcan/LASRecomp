#include "research_actors.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>
struct Memory { std::vector<unsigned char> data=std::vector<unsigned char>(4096);unsigned reads=0;bool change=false; };
static bool read(void *context,uint64_t address,void *out,size_t size) {
    auto &m=*(Memory *)context;
    if(address<0x1000||address-0x1000>m.data.size()||size>m.data.size()-(address-0x1000))return false;
    memcpy(out,m.data.data()+address-0x1000,size);
    if(address==0x1000&&++m.reads==2&&m.change)((unsigned char *)out)[16]^=1;
    return true;
}
template<class T>static void put(Memory &m,uint64_t address,T value){memcpy(m.data.data()+address-0x1000,&value,sizeof value);}
static void text(Memory &m,uint64_t address,const char *value){strcpy((char *)m.data.data()+address-0x1000,value);}
int main(){
    Memory m;ResearchActorSnapshot out;
    put<uint64_t>(m,0x1010,0x1100);put<uint64_t>(m,0x1018,2);
    for(unsigned i=0;i<2;++i){uint64_t node=0x1100+i*32,actor=0x1200+i*256,component=0x1500+i*256;
        put<uint64_t>(m,node,i?0:node+32);put<uint64_t>(m,node+16,2-i);put<uint64_t>(m,node+24,actor);
        put<uint64_t>(m,actor,0x7001234567);put<uint64_t>(m,actor+0x80,component);
        text(m,actor+0x18,i?"EnemyTest-0000000000000001":"NpcTest-0000000000000002");
        text(m,component+0x18,"CharacterMovement");put<float>(m,component+0x80,float(i));
    }
    research_collect_actors(0x1000,read,&m,&out);
    assert(out.complete&&out.count==2&&out.registry_count==2&&out.actors[0].key==1&&out.actors[0].xyz[0]==1);
    put<float>(m,0x1680,NAN);research_collect_actors(0x1000,read,&m,&out);assert(out.complete&&!out.actors[0].position);
    put<uint64_t>(m,0x1138,0x999999);research_collect_actors(0x1000,read,&m,&out);assert(!out.complete&&out.issue==RESEARCH_ACTORS_UNREADABLE);
    put<uint64_t>(m,0x1138,0x1300);put<uint64_t>(m,0x1120,0x1100);research_collect_actors(0x1000,read,&m,&out);assert(!out.complete&&out.issue==RESEARCH_ACTORS_LAYOUT);
    put<uint64_t>(m,0x1120,0);m.change=true;m.reads=0;research_collect_actors(0x1000,read,&m,&out);assert(!out.complete&&out.issue==RESEARCH_ACTORS_CHANGED);
    m.change=false;put<uint64_t>(m,0x1018,2049);research_collect_actors(0x1000,read,&m,&out);assert(!out.complete&&out.issue==RESEARCH_ACTORS_LIMIT);
    std::cout<<"Actor registry bounds, ordering, invalid pointers, cycles, concurrent changes and nonfinite positions passed\n";
}
