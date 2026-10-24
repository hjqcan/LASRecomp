// Only included by tools/research_instrument.py output, never by normal generated code.
#pragma once
#include "cpu.h"
#include "game_addresses.h"
#include "research_stores.h"
#ifndef RESEARCH_STORE_TARGET
#define RESEARCH_STORE_TARGET GA(RESEARCH_RNG_STATE)
#endif
// Evaluate address/value once. The original store is outside the recorder's lock.
// Snapshots are observations, not atomic guest transactions. Overlap is reported.
typedef __uint128_t __attribute__((aligned(1), may_alias)) research_u128u;
#define RESEARCH_RAW_ST128(a,v) (*(research_u128u *)(uintptr_t)(a)=(__uint128_t)(v))
#define RESEARCH_STORE_HELPER(N,T,STORE) \
static inline T research_probe_st##N(Cpu *c,uint64_t site,uint64_t function,uint64_t address,T value) { \
    uint64_t token=0; \
    if(address<RESEARCH_STORE_TARGET+16 && address>RESEARCH_STORE_TARGET-sizeof(T) && \
       __atomic_load_n(&g_research_stores,__ATOMIC_RELAXED)) { \
        uint32_t before[4];memcpy(before,(const void *)(uintptr_t)RESEARCH_STORE_TARGET,16); \
        token=research_store_begin(site,function,c->x[30],address,sizeof(T),before); \
    } \
    STORE(address,value); \
    if(token) { uint32_t after[4];memcpy(after,(const void *)(uintptr_t)RESEARCH_STORE_TARGET,16);research_store_end(token,after); } \
    return value; \
}
RESEARCH_STORE_HELPER(8,uint8_t,ST8)
RESEARCH_STORE_HELPER(16,uint16_t,ST16)
RESEARCH_STORE_HELPER(32,uint32_t,ST32)
RESEARCH_STORE_HELPER(64,uint64_t,ST64)
RESEARCH_STORE_HELPER(128,__uint128_t,RESEARCH_RAW_ST128)
#undef RESEARCH_STORE_HELPER
#undef RESEARCH_RAW_ST128
