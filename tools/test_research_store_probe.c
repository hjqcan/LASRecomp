#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char region[64];
#define RESEARCH_STORE_TARGET ((uint64_t)(uintptr_t)(region+16))
#include "research_store_probe.h"
unsigned g_research_stores;
static unsigned begins,ends,addresses,values;
uint64_t research_store_begin(uint64_t site,uint64_t function,uint64_t caller,uint64_t address,unsigned size,const uint32_t before[4]) {
    assert(site==1&&function==2&&caller==3&&size<=16&&address<RESEARCH_STORE_TARGET+16);
    assert(!memcmp(before,region+16,16));++begins;return 42;
}
void research_store_end(uint64_t token,const uint32_t after[4]) {assert(token==42&&!memcmp(after,region+16,16));++ends;}
static uint64_t address(void){++addresses;return RESEARCH_STORE_TARGET;}
static uint32_t value(void){++values;return 456;}
int main(void) {
    Cpu c={0};c.x[30]=3;
    assert(research_probe_st32(&c,1,2,RESEARCH_STORE_TARGET,123)==123&&begins==0);
    g_research_stores=1;
    assert(research_probe_st32(&c,1,2,address(),value())==456&&addresses==1&&values==1&&begins==1&&ends==1);
    research_probe_st32(&c,1,2,RESEARCH_STORE_TARGET+20,99);assert(begins==1);
    research_probe_st8(&c,1,2,RESEARCH_STORE_TARGET,7);
    research_probe_st16(&c,1,2,RESEARCH_STORE_TARGET-1,0xabcd); // An overlapping partial store.
    research_probe_st64(&c,1,2,RESEARCH_STORE_TARGET+8,0x123456789abcdef0ull);
    research_probe_st128(&c,1,2,RESEARCH_STORE_TARGET,(__uint128_t)1234567);
    assert(begins==5&&ends==5&&LD32(RESEARCH_STORE_TARGET)==1234567);
    g_research_stores=0;research_probe_st8(&c,1,2,RESEARCH_STORE_TARGET,3);assert(begins==5&&region[16]==3);
    puts("Instrumented stores preserve writes, expression values, argument evaluation and disabled behavior");
}
