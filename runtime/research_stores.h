#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Optional export on a separately instrumented game library; never changes its ABI.
const char *gamelib_research_stores(void);
extern unsigned g_research_stores;
bool research_stores_boot(bool semantics,char *why,size_t cap);
void research_stores_boundary(bool before,size_t update,uint32_t thread,const uint32_t state[4]);
uint64_t research_store_begin(uint64_t site,uint64_t function,uint64_t caller,uint64_t address,
                              unsigned size,const uint32_t before[4]);
void research_store_end(uint64_t token,const uint32_t after[4]);
bool research_stores_export(const char *directory,char *why,size_t cap);
#ifdef __cplusplus
}
#endif
