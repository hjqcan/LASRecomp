// Photo-mode state shared by the overlay and both renderers. A complete scene is held at present, then
// framing crops that retained image; the game simulation is not serialized or paused as a save state.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool active, frozen, hide_hud, framing;
    float zoom, pan_x, pan_y, exposure, saturation;
    float before_zoom, before_pan_x, before_pan_y;  // the view restored when controller framing is cancelled
    uint64_t armed, clean_frame;
} PhotoState;
void photo_state_reset(PhotoState *s);
void photo_state_enter(PhotoState *s, uint64_t frame);
void photo_state_refresh(PhotoState *s, uint64_t frame);
void photo_state_presented(PhotoState *s, uint64_t frame);
bool photo_state_skip_draw(PhotoState *s, uint64_t frame, bool screen_target, bool composite);
bool photo_scene_composite(unsigned target_w, unsigned target_h, unsigned source_w, unsigned source_h,
                           bool fullscreen, bool blended, int textures);
void photo_state_pan(PhotoState *s, float dx, float dy);  // image drag as fractions of the displayed photo
void photo_state_zoom(PhotoState *s, float steps);
void photo_state_begin_framing(PhotoState *s);
void photo_state_end_framing(PhotoState *s, bool accept);
void photo_state_crop(const PhotoState *s, float crop[4]);

bool photo_mode_active(void);
bool photo_mode_available(void);
void photo_mode_toggle(void);
void photo_mode_exit(void);
void photo_mode_capture(void);
void photo_mode_toggle_controls(void);
bool photo_mode_pad_button(int host, bool down);  // remapped framing actions; true when the photo consumes the event
void photo_mode_frame(uint64_t frame); // render thread: before presenting this frame
void photo_mode_presented(uint64_t frame); // render thread: the completed frame plus one
bool photo_mode_hold(void); // atomic snapshot for the game thread's present hold loop
bool photo_mode_skip_draw(uint64_t frame, bool screen_target, bool composite);
void photo_mode_compose(float crop[4], double width, double height, double *x, double *y, double *w, double *h);
void photo_mode_color(float color[4]); // exposure multiplier, saturation, reserved, reserved
const char *photo_mode_capture_path(void);
void photo_mode_capture_done(void);
bool photo_mode_suppress_overlay(void);
void photo_mode_draw(float scale);
#ifdef __cplusplus
}
#endif
