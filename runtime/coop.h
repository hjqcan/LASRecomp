#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
bool coop_active(void);
bool coop_requested(void);
void coop_request(bool enabled);
void coop_host_input(int32_t x,int32_t y,bool attack);
bool coop_boot(char *why,size_t cap);
#ifdef __cplusplus
}
#endif
