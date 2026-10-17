#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool research_seed_boot(bool semantics,char *why,size_t cap);
uint64_t research_seed_time(uint64_t original,bool startup,uint64_t caller,uint32_t thread);
bool research_seed_export(const char *directory,char *why,size_t cap);
bool research_seed_site_matches(bool updated,const void *bytes,size_t size);
bool research_time_site_matches(const void *bytes,size_t size);
bool research_seed_initializer_matches(const void *bytes,size_t size);
void research_seed_initial(uint32_t seed,uint32_t state[4]);
#ifdef __cplusplus
}
#endif
