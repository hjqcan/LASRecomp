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
void photo_state_frame(const PhotoState *s, float crop[4], double rect[4]) {
    if (!s->active || !isfinite(rect[2]) || !isfinite(rect[3]) || rect[2] <= 0 || rect[3] <= 0) return;
    static const double ratios[] = {0, 16.0/9, 4.0/3, 1, 4.0/5, 2.39};
    double ratio = s->aspect > 0 && s->aspect < 6 ? ratios[s->aspect] : rect[2]/rect[3];
    double w = rect[2], h = rect[3];
    if (w/h > ratio) w = h * ratio; else h = w / ratio;
    float zoom = bounded(s->zoom, 1, 4);
    float cw = crop[2] * (float)(w/rect[2]) / zoom, ch = crop[3] * (float)(h/rect[3]) / zoom;
    crop[0] += (crop[2]-cw) * (bounded(s->pan_x,-1,1)+1) * 0.5f;
    crop[1] += (crop[3]-ch) * (bounded(s->pan_y,-1,1)+1) * 0.5f;
    crop[2] = cw; crop[3] = ch;
    rect[0] += (rect[2]-w)*0.5; rect[1] += (rect[3]-h)*0.5;
    rect[2] = w; rect[3] = h;
}
void photo_state_color(const PhotoState *s, float color[4]) {
    color[0] = s->active ? exp2f(bounded(s->exposure,-2,2)) : 1;
    color[1] = s->active ? bounded(s->saturation,0,2) : 1;
    color[2] = s->active ? bounded(s->contrast,-0.5f,1) : 0;
    color[3] = s->active ? bounded(s->vignette,0,1) : 0;
}
