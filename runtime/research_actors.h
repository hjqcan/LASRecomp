#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define RESEARCH_MAX_ACTORS 64
typedef struct {
    uint64_t key,vtable;
    char name[64],component[32];
    float xyz[3];
    bool position;
} ResearchActor;
enum { RESEARCH_ACTORS_OK, RESEARCH_ACTORS_UNREADABLE, RESEARCH_ACTORS_LIMIT,
       RESEARCH_ACTORS_LAYOUT, RESEARCH_ACTORS_CHANGED };
typedef struct {
    ResearchActor actors[RESEARCH_MAX_ACTORS];
    unsigned count,registry_count,issue;
    bool complete;
} ResearchActorSnapshot;
typedef bool (*ResearchRead)(void *context,uint64_t address,void *destination,size_t size);
// Bounded registry walk through a checked reader; contains no guest writes.
void research_collect_actors(uint64_t registry,ResearchRead read,void *context,ResearchActorSnapshot *out);
void research_read_actors(ResearchActorSnapshot *out);
#ifdef __cplusplus
}
#endif
