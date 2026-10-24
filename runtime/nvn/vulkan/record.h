// The recorder thread (record.c): every Vulkan call that touches a command buffer, the command pool, the descriptor
// pools or the queue is made by a thread of its own. The translating thread (the render thread, rqueue.c; the game's
// GPU thread with LARECOMP_RENDER_THREAD=0; the UI thread for the start screen) keeps every decision and calls the
// rec_* functions below instead of vkCmd* etc.: each copies its arguments into a packet in a ring, in the order it
// was called, and the recorder makes the real call in that order. With the recorder off (LARECOMP_VK_RECORDER=0, or
// no thread could be made) the same functions make the call at once, on the calling thread, so the two modes differ
// only in which thread holds the driver's lock: the calls and their order are the same.
//
// What the two threads share, and how (the whole table is in record.c's header comment).
#pragma once
#include "vk.h"

// Submissions in flight (exec.c's ring of Sub slots; the recorder's descriptor pools follow them). A frame is ~10
// submissions with the render thread (LARECOMP_CHUNK_PASSES) plus the window's; with the game's frame fence waited
// one frame late (exec.c overlap_on) two frames are queued, so 8 slots would have had sub() wait for the GPU on
// every slot reuse within the frame. 24: two frames and a margin.
#define VK_SUBS 24

// ---- start / mode (exec.c vk_exec_init, before any command)
void rec_init(bool prof);
bool rec_on(void);  // commands go through the recorder thread

// ---- command buffers, submissions, presents (the translating thread only)
void rec_BeginCommandBuffer(VkCommandBuffer cb);  // ONE_TIME_SUBMIT
void rec_EndCommandBuffer(VkCommandBuffer cb);
// Submits si (up to 2 command buffers, 1 wait, 2 signals; nothing chained). seq: the timeline value it signals, which
// rec_submitted() reaches once the driver has it. dl_trace: LARECOMP_VK_DEVLOST_TRACE's dump runs on a device loss.
void rec_QueueSubmit2(const VkSubmitInfo2 *si, uint64_t seq);
// vkQueuePresentKHR(image of swapchain, after wait). The result reaches rec_present_recreate(); the wait inside the
// driver (FIFO: until the display) is the recorder's, counted in g_vk_frame_waits[2].
void rec_QueuePresentKHR(VkSemaphore wait, VkSwapchainKHR swapchain, uint32_t image);
bool rec_present_recreate(void);  // (and clears it) the last present asked for a new swapchain (OUT_OF_DATE / SUBOPTIMAL)

// ---- descriptor sets (the translating thread only)
// A set of layout from slot's pools: the real set with the recorder off; a stand-in otherwise, which the recorder
// replaces with the set it allocates when it reaches this packet - so the handle may only be bound / written through
// rec_* (never passed to the driver by the caller), and only before slot's pools are reset again.
VkDescriptorSet rec_alloc_set(int slot, VkDescriptorSetLayout layout);
void rec_reset_pools(int slot);  // the slot's submission is done (exec.c sub()): its sets go
bool rec_set_batch(void);          // sets are allocated in batches (LARECOMP_VK_NO_SET_BATCH=1: one per call)
bool rec_set_recycle(void);        // a slot's sets are handed out again instead of resetting its pools (LARECOMP_VK_NO_SET_RECYCLE=1: off)
// n writes, each of one descriptor (buffer or image info), to sets from rec_alloc_set or real ones
void rec_UpdateDescriptorSets(uint32_t n, const VkWriteDescriptorSet *w);

// ---- waits (any thread)
uint64_t rec_submitted(void);          // the latest submission handed to the driver (the timeline value it signals)
void rec_wait_submitted(uint64_t seq); // until the driver has submission seq (seq must have been passed to rec_QueueSubmit2)
void rec_wait_presents(uint64_t n);    // until the recorder made n presents (rec_presents_queued() counts them)
uint64_t rec_presents_queued(void);
// Everything queued so far is recorded and submitted (the translating thread): before vkDeviceWaitIdle, before
// recording into a command buffer directly (ImGui), before using the queue outside the ring.
void rec_drain(void);
void rec_drain_published(void);  // (any thread, exit) what the translating thread had published so far
// Quitting (device.c backend_quit, after rq_stop; any thread): from now on the recorder makes no driver call - the
// packets it still has or gets are dropped, so nothing is submitted or presented after the window closed - and this
// returns once it is idle (bounded: 5 s for a recorder stuck in the driver). Waits for submissions never end after it.
void rec_quit(void);

// ---- pipelines
// A graphics pipeline still being built (exec.c Pipe): the recorder resolves obj through this at the bind, waiting
// for it with LARECOMP_VK_RECORDER_WAIT_PIPES=1 (0: ready pipelines only reach it). Returns the pipeline, or NULL
// once the build failed (the draws until the next bind are dropped).
typedef VkPipeline (*RecPipeResolve)(void *obj, bool wait);
void rec_set_pipe_resolver(RecPipeResolve f);
bool rec_wait_pipes(void);  // LARECOMP_VK_RECORDER_WAIT_PIPES=1: draws wait for their pipeline instead of being skipped

// ---- profiling (LARECOMP_VK_PROF, read by exec.c renderer_stats; counts since the last take)
typedef struct {
    uint64_t packets, bytes, enqueue_ns;          // packets queued, their bytes, the translating thread's time queuing them
    uint64_t busy_ns, wakeups;                    // the recorder's time making calls; times it woke from its sleep
    uint64_t submit_ns;                           // its time in vkQueueSubmit2
    uint64_t drains, drain_ns;                    // rec_drain waits
    uint64_t seq_waits, seq_wait_ns;              // rec_wait_submitted waits that had to wait
    uint64_t present_waits, present_wait_ns;      // rec_wait_presents waits that had to wait
    uint64_t full_waits, full_ns;                 // the ring was full
    uint64_t set_allocs, set_calls;               // descriptor sets allocated, vkAllocateDescriptorSets calls
    uint64_t sets_fresh;                          // of those, new from the pools (the rest recycled: LARECOMP_VK_NO_SET_RECYCLE)
    uint64_t pool_resets, pool_reset_ns;          // vkResetDescriptorPool calls and their time (on the recorder when it is on)
    uint64_t pipe_waits, pipe_wait_ns;            // binds that waited for a pipeline build (WAIT_PIPES)
    uint64_t draws_dropped;                       // draws dropped after a failed pipeline (WAIT_PIPES)
} RecStats;
void rec_stats_take(RecStats *out);

// ---- commands (the translating thread only; cb: a command buffer exec.c owns)
void rec_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p);
void rec_CmdBindPipelineObj(VkCommandBuffer cb, VkPipelineBindPoint bp, void *obj);  // (resolved by the resolver above)
void rec_CmdDraw(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst);
void rec_CmdDrawIndexed(VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, int32_t base_vertex, uint32_t first_inst);
void rec_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z);
void rec_CmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type);
void rec_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs, const VkDeviceSize *offs);
void rec_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                               const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn);
void rec_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off, uint32_t size,
                          const void *data);
void rec_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri);
void rec_CmdEndRendering(VkCommandBuffer cb);
void rec_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di);
void rec_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v);
void rec_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r);
void rec_CmdSetBlendConstants(VkCommandBuffer cb, const float c[4]);
void rec_CmdSetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope);
void rec_CmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp op);
void rec_CmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 on);
void rec_CmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags face, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail,
                         VkCompareOp cmp);
void rec_CmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v);
void rec_CmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v);
void rec_CmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags face, uint32_t v);
void rec_CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags mode);
void rec_CmdSetFrontFace(VkCommandBuffer cb, VkFrontFace ff);
void rec_CmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology topo);
void rec_CmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 on);
// (dynamic vertex input: exec.c set_vertex_input, after the pipeline bind; the arrays are copied)
void rec_CmdSetVertexInputEXT(VkCommandBuffer cb, uint32_t nb, const VkVertexInputBindingDescription2EXT *b, uint32_t na,
                              const VkVertexInputAttributeDescription2EXT *v);
void rec_CmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r);
void rec_CmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout layout, const VkClearColorValue *c, uint32_t n,
                            const VkImageSubresourceRange *rg);
void rec_CmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout layout, const VkClearDepthStencilValue *c,
                                   uint32_t n, const VkImageSubresourceRange *rg);
void rec_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r);
void rec_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r);
void rec_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r,
                      VkFilter filter);
void rec_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
                         const VkImageResolve *r);
void rec_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n, const VkBufferImageCopy *r);
void rec_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r);
void rec_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs);  // (secondaries, ended, of a pool of the caller's)
void rec_CmdResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t n);
void rec_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query);
