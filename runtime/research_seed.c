#include "research_seed.h"

void research_seed_initial(uint32_t seed,uint32_t state[4]) {
    static const uint32_t initial[4]={123456789,362436069,521288629,88675123};
    for(int i=0;i<4;++i) {
        uint32_t value=initial[i]^seed;
        value^=value>>21;value^=value>>4;
        state[i]=value*UINT32_C(0x4f6cdd1d);
    }
}
