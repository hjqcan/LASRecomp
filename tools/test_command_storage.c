// Production recording path: compact storage, payload lifetime, recycling and
// the guest's out-of-memory callback. No graphics device is needed.
#include <assert.h>
#include <stdarg.h>
#include "../runtime/nvn/cmdbuf.c"
#include "../runtime/nvn/render_control.h"

static uint64_t pool_guest;
static unsigned callbacks;
static const TitleProfile profile = {.nvn_major=53};
const TitleProfile *title_profile_current(void) { return &profile; }
void backend_texture_view_format(NvnTexture *t,uint32_t format) { (void)t;(void)format; }
void nvn_sync_arm(uint64_t guest) { assert(guest); }
void rt_fatal_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
void rt_fatal_trace(void) { abort(); }
void guest_call(Cpu *c, uint64_t address) {
    assert(address == 1 && c->x[1] == 0 && c->x[2] <= 512 && c->x[3] == 123);
    callbacks++;
    c->x[1] = (uint64_t)(uintptr_t)&pool_guest; c->x[2] = 512; c->x[3] = 512;
    hle_nvnCommandBufferAddCommandMemory(c);
}
static void setup(Cpu *c, uint64_t *guest, uint8_t *control, unsigned offset, unsigned size) {
    c->x[0] = (uint64_t)(uintptr_t)guest; hle_nvnCommandBufferInitialize(c);
    c->x[0] = (uint64_t)(uintptr_t)guest; c->x[1] = (uint64_t)(uintptr_t)control; c->x[2] = 256;
    hle_nvnCommandBufferAddControlMemory(c);
    c->x[1] = (uint64_t)(uintptr_t)&pool_guest; c->x[2] = offset; c->x[3] = size;
    hle_nvnCommandBufferAddCommandMemory(c); hle_nvnCommandBufferBeginRecording(c);
}
static size_t live_lists(void) {
    size_t count = 0;
    for (unsigned i = 0; i < LIST_BUCKETS; i++)
        for (CommandList *p = command_lists[i]; p; p = p->next) count++;
    return count;
}
static CmdRec *command_at(uint64_t handle, unsigned index) {
    CmdRec *jump = (CmdRec *)(uintptr_t)handle; assert(jump->op == CMD_JUMP);
    CmdRec *record = (CmdRec *)(uintptr_t)jump->x[0];
    while (index--) record = (CmdRec *)((uint8_t *)record + record->size);
    return record;
}
int main(void) {
    uint8_t memory[1024], control[256]; memset(memory, 0xcc, sizeof memory);
    NvnPool pool = {.storage=(uint64_t)(uintptr_t)memory, .size=sizeof memory};
    pool_guest = (uint64_t)(uintptr_t)&pool;
    Cpu cpu = {0}; uint64_t guest = 0;
    setup(&cpu, &guest, control, 0, 512);
    float color[4] = {0.25f,0.5f,0.75f,1};
    cpu.x[1] = (uint64_t)(uintptr_t)color; hle_nvnCommandBufferSetBlendColor(&cpu);
    color[0] = 9; // command data must be snapshotted
    hle_nvnCommandBufferEndRecording(&cpu); uint64_t handle = cpu.x[0];
    assert(cb_of((uint64_t)(uintptr_t)&guest)->used == 16);
    assert(cb_of((uint64_t)(uintptr_t)&guest)->control_used == 56);
    cpu.x[0] = (uint64_t)(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&cpu);
    assert(command_at(handle,0)->op == CMD_BARRIER);
    assert(command_at(handle,1)->op == CMD_SET_BLEND_COLOR);
    assert(*(float *)command_at(handle,1)->extra == 0.25f);
    assert(command_at(handle,2)->op == CMD_END && memory[40] == 0xcc);
    uint8_t control2[256];
    setup(&cpu, &guest, control2, 0, 512);
    cpu.x[1] = (uint64_t)(uintptr_t)color; hle_nvnCommandBufferSetBlendColor(&cpu);
    hle_nvnCommandBufferEndRecording(&cpu); uint64_t scratch_handle = cpu.x[0];
    assert(*(float *)command_at(handle,1)->extra == 0.25f);
    assert(*(float *)command_at(scratch_handle,1)->extra == 9);
    nvn_command_handle_destroy(scratch_handle);
    cpu.x[0] = (uint64_t)(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&cpu);
    // Two active recorders can be interleaved over the same scratch budget.
    Cpu other = {0}; uint64_t other_guest = 0;
    setup(&cpu, &guest, control, 0, 512);
    color[0] = 0.5f; cpu.x[1] = (uint64_t)(uintptr_t)color; hle_nvnCommandBufferSetBlendColor(&cpu);
    setup(&other, &other_guest, control2, 0, 512);
    color[0] = 0.75f; other.x[1] = (uint64_t)(uintptr_t)color; hle_nvnCommandBufferSetBlendColor(&other);
    hle_nvnCommandBufferEndRecording(&other); scratch_handle = other.x[0];
    hle_nvnCommandBufferEndRecording(&cpu); handle = cpu.x[0];
    assert(*(float *)command_at(handle,1)->extra == 0.5f);
    assert(*(float *)command_at(scratch_handle,1)->extra == 0.75f);
    nvn_command_handle_destroy(scratch_handle);
    other.x[0] = (uint64_t)(uintptr_t)&other_guest; hle_nvnCommandBufferFinalize(&other);
    cpu.x[0] = (uint64_t)(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&cpu);
    // Re-record at overlapping 4-byte-aligned offsets, then a much larger payload.
    for (unsigned i = 0; i < 1000; i++) {
        setup(&cpu, &guest, control, (i % 4)*4, 512);
        cpu.x[1] = (uint64_t)(uintptr_t)color; hle_nvnCommandBufferSetBlendColor(&cpu);
        hle_nvnCommandBufferEndRecording(&cpu);
        assert(live_lists() == 1);
        cpu.x[0] = (uint64_t)(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&cpu);
    }
    uint8_t payload[8192]; memset(payload, 0x5a, sizeof payload);
    setup(&cpu, &guest, control, 0, 64);
    cpu.x[1] = 1; hle_nvnCommandBufferSetMemoryCallback(&cpu);
    cpu.x[1] = 123; hle_nvnCommandBufferSetMemoryCallbackData(&cpu);
    cpu.x[4] = sizeof payload; cpu.x[5] = (uint64_t)(uintptr_t)payload;
    hle_nvnCommandBufferUpdateUniformBuffer(&cpu);
    cpu.x[1] = 7; hle_nvnCommandBufferBarrier(&cpu);
    cpu.x[1] = 9; hle_nvnCommandBufferBarrier(&cpu);
    cpu.x[1] = 11; hle_nvnCommandBufferBarrier(&cpu);
    cpu.x[1] = 13; hle_nvnCommandBufferBarrier(&cpu);
    hle_nvnCommandBufferEndRecording(&cpu); handle = cpu.x[0];
    assert(callbacks == 1);
    assert(command_at(handle,1)->op == CMD_UPDATE_UNIFORM_BUFFER);
    assert(!memcmp(command_at(handle,1)->extra, payload, sizeof payload));
    assert(command_at(handle,3)->op == CMD_BARRIER && command_at(handle,3)->x[0] == 9);
    assert(command_at(handle,6)->op == CMD_END);
    assert(cb_of((uint64_t)(uintptr_t)&guest)->used == 16);
    assert(cb_of((uint64_t)(uintptr_t)&guest)->control_used == 88);
    for (size_t i = 0; i < sizeof memory; i++) assert(memory[i] == 0xcc);
    cpu.x[0] = (uint64_t)(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&cpu);
    uint8_t copy_memory[1024], copy_control[256];
    NvnPool copy_pool = {.storage=(uint64_t)(uintptr_t)copy_memory, .size=sizeof copy_memory};
    pool_guest = (uint64_t)(uintptr_t)&copy_pool;
    setup(&cpu, &guest, copy_control, 0, 512);
    cpu.x[1] = 1; cpu.x[2] = (uint64_t)(uintptr_t)&handle;
    hle_nvnCommandBufferCopyCommands(&cpu);
    hle_nvnCommandBufferEndRecording(&cpu); uint64_t copied = cpu.x[0];
    cpu.x[0] = (uint64_t)(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&cpu);
    nvn_command_pool_destroy(&pool); assert(!pool.command_storage);
    assert(command_at(copied,2)->op == CMD_UPDATE_UNIFORM_BUFFER);
    assert(!memcmp(command_at(copied,2)->extra, payload, sizeof payload));
    assert(command_at(copied,4)->op == CMD_BARRIER && command_at(copied,4)->x[0] == 9);
    assert(command_at(copied,7)->op == CMD_END);
    uint8_t caller_control[256];
    setup(&cpu,&guest,caller_control,0,512);
    cpu.x[1]=1; cpu.x[2]=(uint64_t)(uintptr_t)&copied;
    hle_nvnCommandBufferCallCommands(&cpu);
    hle_nvnCommandBufferEndRecording(&cpu); uint64_t caller=cpu.x[0];
    cpu.x[0]=(uint64_t)(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&cpu);
    void *executing=nvn_command_list_acquire(caller);
    nvn_command_handle_destroy(copied);
    nvn_command_handle_destroy(caller);
    CmdRec *first=(CmdRec *)(uintptr_t)nvn_command_list_records(executing);
    CmdRec *call=(CmdRec *)((uint8_t *)first+first->size);
    assert(call->op==CMD_CALL);
    CmdRec *child=(CmdRec *)(uintptr_t)*(uint64_t *)call->extra;
    for (int i=0;i<2;i++) child=(CmdRec *)((uint8_t *)child+child->size);
    assert(child->op==CMD_UPDATE_UNIFORM_BUFFER && !memcmp(child->extra,payload,sizeof payload));
    nvn_command_list_release(executing);
    nvn_command_pool_destroy(&copy_pool); assert(!copy_pool.command_storage);
    for (unsigned i = 0; i < LIST_BUCKETS; i++) assert(!command_lists[i]);
    // Counter timestamps are not part of the comparison. Neither half of the
    // 64-bit value may be truncated, and ReportValue must zero its upper half.
    uint64_t values[4] = {0,123,0,456};
    assert(nvn_render_condition(0,values) && !nvn_render_condition(1,values));
    values[2]=UINT64_C(1)<<32;
    assert(!nvn_render_condition(0,values) && nvn_render_condition(1,values));
    nvn_report_value(values,7,789); nvn_report_value(values+2,7,987);
    assert(values[0]==7 && values[1]==789 && values[2]==7 && values[3]==987);
    assert(nvn_render_condition(0,values));
    setup(&cpu,&guest,control,0,512);
    cpu.x[1]=1; cpu.x[2]=(uint64_t)(uintptr_t)values; cpu.x[7]=99;
    hle_nvnCommandBufferSetRenderEnableConditional(&cpu);
    cpu.x[1]=0; hle_nvnCommandBufferSetRenderEnable(&cpu);
    cpu.x[1]=42; cpu.x[2]=(uint64_t)(uintptr_t)values;
    hle_nvnCommandBufferReportValue(&cpu);
    hle_nvnCommandBufferEndRecording(&cpu); uint64_t predicate_handle=cpu.x[0];
    assert(command_at(predicate_handle,1)->x[6]==1);
    assert(command_at(predicate_handle,2)->x[6]==0);
    assert(command_at(predicate_handle,3)->op==CMD_REPORT_VALUE && values[0]==7);
    assert(nvn_command_needs_render(CMD_CLEAR_COLOR) && nvn_command_needs_render(CMD_DRAW_ELEMENTS));
    assert(!nvn_command_needs_render(CMD_REPORT_VALUE) && !nvn_command_needs_render(CMD_CALL));
    nvn_command_handle_destroy(predicate_handle);
    cpu.x[0]=(uint64_t)(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&cpu);
    puts("PASS: independent recording, 1000 reuse cycles, callbacks, deep copies and retained nested lists");
    return 0;
}
