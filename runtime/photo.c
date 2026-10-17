// Photo-mode frame and crop bookkeeping, independent of the graphics backend. Wait for a whole clean frame
// before holding it, keep framing inside the source image, and leave the ordinary presentation crop untouched.
#include "photo.h"
#include <math.h>
#include <string.h>

static float bounded(float value, float low, float high) {
    if (!isfinite(value)) return low;
    return value < low ? low : value > high ? high : value;
}
void photo_state_reset(PhotoState *s) {
    memset(s, 0, sizeof *s);
    s->zoom = s->saturation = 1;
    s->hide_hud = true;
    s->clean_frame = UINT64_MAX;
}
void photo_state_enter(PhotoState *s, uint64_t frame) {
    photo_state_reset(s);
    s->active = true;
    s->armed = frame;
}
void photo_state_refresh(PhotoState *s, uint64_t frame) {
    s->frozen = false;
    s->armed = frame;
    s->clean_frame = UINT64_MAX;
}
void photo_state_presented(PhotoState *s, uint64_t frame) {
    // Activation happens after the current frame was drawn. Allow a whole clean frame first.
    if (s->active && frame > s->armed + 1) s->frozen = true;
}
bool photo_state_skip_draw(PhotoState *s, uint64_t frame, bool screen_target, bool composite) {
    if (!s->active || !s->hide_hud || !screen_target) return false;
    if (composite) { s->clean_frame = frame; return false; }
    return s->clean_frame == frame;
}
bool photo_scene_composite(unsigned target_w, unsigned target_h, unsigned source_w, unsigned source_h,
                           bool fullscreen, bool blended, int textures) {
    if (!fullscreen || blended || textures != 1 || !target_w || !target_h || source_w < 256 || source_h < 144) return false;
    // The final scene can be internally downscaled (e.g. 1280x720 into a 1920x1080 window image).
    double a = (double)source_w * target_h, b = (double)source_h * target_w;
    return fabs(a-b) <= 0.01 * b;
}
void photo_state_crop(const PhotoState *s, float crop[4]) {
    if (!s->active) return;
    float zoom = bounded(s->zoom, 1, 4), width = crop[2] / zoom, height = crop[3] / zoom;
    crop[0] += (crop[2] - width) * (bounded(s->pan_x, -1, 1) + 1) * 0.5f;
    crop[1] += (crop[3] - height) * (bounded(s->pan_y, -1, 1) + 1) * 0.5f;
    crop[2] = width; crop[3] = height;
}

void photo_state_pan(PhotoState *s, float dx, float dy) {
    if (!s->active || !s->frozen || s->zoom <= 1) return;
    // Dragging the image right moves the source crop left. The available margin shrinks near 1x zoom.
    float margin = s->zoom - 1;
    s->pan_x = bounded(s->pan_x - 2 * dx / margin, -1, 1);
    s->pan_y = bounded(s->pan_y - 2 * dy / margin, -1, 1);
}
void photo_state_zoom(PhotoState *s, float steps) {
    if (!s->active || !s->frozen || !isfinite(steps)) return;
    s->zoom = bounded(s->zoom * powf(1.15f, bounded(steps, -20, 20)), 1, 4);
    if (s->zoom == 1) s->pan_x = s->pan_y = 0;
}
void photo_state_begin_framing(PhotoState *s) {
    if (!s->active || !s->frozen || s->framing) return;
    s->before_zoom = s->zoom; s->before_pan_x = s->pan_x; s->before_pan_y = s->pan_y;
    s->framing = true;
}
void photo_state_end_framing(PhotoState *s, bool accept) {
    if (!s->framing) return;
    if (!accept) { s->zoom = s->before_zoom; s->pan_x = s->before_pan_x; s->pan_y = s->before_pan_y; }
    s->framing = false;
}
