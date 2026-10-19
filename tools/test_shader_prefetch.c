#include "../runtime/nvn/shader_prefetch.h"
#include <assert.h>
#include <stdio.h>

typedef struct { uint64_t words[256]; size_t used; } List;
static CmdRec *add(List *list, uint32_t op, unsigned extra) {
    CmdRec *r=(CmdRec *)((uint8_t *)list->words+list->used);
    r->op=op; r->size=sizeof *r+extra*8; list->used+=r->size;
    assert(list->used<=sizeof list->words); return r;
}
static NvnProgram first, second;
static NvnTexture target={.format=3};
static NvnShaderPrefetch seen[8];
static int visits;
static void visit(const NvnShaderPrefetch *state) { assert(visits<8); seen[visits++]=*state; }
int main(void) {
    List parent={0}, child={0};
    CmdRec *r=add(&parent,CMD_BIND_PROGRAM,0); r->x[0]=(uintptr_t)&first;
    r=add(&parent,CMD_BIND_VERTEX_ATTRIB,1); r->x[0]=2;
    uint32_t attributes[2]={1,3}; memcpy(r->extra,attributes,sizeof attributes);
    r=add(&parent,CMD_SET_RENDER_TARGETS,8); r->x[0]=1; *(uint64_t *)r->extra=(uintptr_t)&target;
    add(&parent,CMD_DRAW_ARRAYS,0);
    r=add(&child,CMD_BIND_PROGRAM,0); r->x[0]=(uintptr_t)&second;
    add(&child,CMD_DRAW_ELEMENTS,0); add(&child,CMD_END,0);
    r=add(&parent,CMD_CALL,1); r->x[0]=1; *(uint64_t *)r->extra=(uintptr_t)child.words;
    add(&parent,CMD_DRAW_ARRAYS,0); add(&parent,CMD_END,0);
    NvnShaderPrefetch state={.render_enabled=true}; unsigned budget=100;
    assert(nvn_shader_prefetch((uintptr_t)parent.words,0,&budget,&state,visit));
    assert(visits==3 && seen[0].program==&first && seen[1].program==&second && seen[2].program==&second);
    assert(seen[2].nattrib==2 && seen[2].attrib[1]==3 && seen[2].rt_format[0]==3);
    visits=0; state.render_enabled=false; budget=100;
    assert(nvn_shader_prefetch((uintptr_t)parent.words,0,&budget,&state,visit) && visits==0);
    List writes={0}; add(&writes,CMD_DRAW_ARRAYS,0);
    add(&writes,CMD_UPDATE_UNIFORM_BUFFER,0); add(&writes,CMD_DRAW_ARRAYS,0); add(&writes,CMD_END,0);
    state.render_enabled=true; visits=0; budget=100;
    assert(!nvn_shader_prefetch((uintptr_t)writes.words,0,&budget,&state,visit) && visits==1);
    List cycle={0}; r=add(&cycle,CMD_JUMP,0); r->x[0]=(uintptr_t)cycle.words;
    budget=31; assert(!nvn_shader_prefetch((uintptr_t)cycle.words,0,&budget,&state,visit) && !budget);
    r->size=0; budget=31; assert(!nvn_shader_prefetch((uintptr_t)cycle.words,0,&budget,&state,visit));
    puts("PASS: exact draw state, nested lists, disabled rendering, write barriers and bounded malformed lists");
}
