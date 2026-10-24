// Interface between the NVN executor (renderer.m) and the Metal plumbing (metal.m).
#pragma once
#ifdef __OBJC__
#import <Metal/Metal.h>
void renderer_init(id<MTLDevice> dev);
id<MTLCommandQueue> renderer_queue(void);
id<MTLTexture> renderer_texture(NvnTexture *t, id<MTLBlitCommandEncoder> blit, bool for_render);
MTLPixelFormat renderer_pixel_format(uint32_t nvn_format);
bool renderer_format_alpha_one(uint32_t nvn_format);
extern id<MTLBuffer> g_guest_buffer;
extern uint64_t g_guest_buffer_base;
#endif
#include <SDL3/SDL_events.h>

bool renderer_is_gpu_owned(NvnTexture *t);
void renderer_flush(void);  // finish all recorded GPU work
bool renderer_origin_lower_left(void);
const char *renderer_stats(void);
bool renderer_format_is_depth(uint32_t f);
bool renderer_format_has_stencil(uint32_t f);
void renderer_mark_gpu_owned(NvnTexture *t);
void input_handle_event(const SDL_Event *e);
void input_rumble_poll(void);
void input_rumble_stop(void);
void input_led_poll(void);  // controller light: UI thread, after input_rumble_poll
void input_led_stop(void);
bool input_user_blocked(const SDL_Event *e);
void input_cursor_event(const SDL_Event *e);  // hide the mouse cursor on pad / key play, show it on mouse use
// g_settings.bg_input -> SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS (before SDL_Init, and again when the setting
// changes: SDL reads the hint live)
void input_apply_background(void);
void input_load_user_mappings(void);  // after SDL_Init: the data root's gamecontrollerdb.txt, if there is one
