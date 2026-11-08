// Touch screens in the ImGui pages (the start screen, the menu): a finger is the mouse, but a list scrolls under a
// dragged finger as on a phone - a drag past a few pixels scrolls (and clicks nothing), a tap clicks where it lifts,
// and a flick keeps the page going for a moment. ImGui alone would press on touch-down and scroll only by its thin
// scrollbar: long pages (Settings) were out of reach and a drag that started on a row pressed it.
// Driven from the thread that renders the page: event() for each finger event, frame() once before each ImGui frame.
// The drag goes out as one scroll per frame: ImGui takes queued input one wheel event a frame, and a finger reports
// several moves a frame - the page fell further and further behind it.
// A finger's highlight waits a moment, as UIScrollView's delaysContentTouches: a scroll that starts on a row doesn't
// light it up first.
#pragma once
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>

#include "imgui_internal.h"

struct TouchUi {
    SDL_FingerID id = 0;
    bool active = false, drag = false, flinging = false;
    int tap_step = 0;  // a tap, spread over frames: the mouse at the spot, pressed a frame later, released the next
    float x0 = 0, y0 = 0, last_x = 0, last_y = 0, pending = 0, vel = 0;  // vel: px / s (the flick)
    uint64_t last_t = 0, hover_at = 0;  // hover_at: when a resting finger starts to highlight what is under it
    bool hovering = false;
    int axis = 1;  // what a drag scrolls: 0 across (the tab bar), 1 down
    ImGuiID scroll_win = 0;  // the window under the finger when it came down: what a drag scrolls (touch_scroll_apply)
    static constexpr uint64_t kHoverDelay = 150000000ull;  // (ns: UIScrollView's delay)

    // pos(x, y), button(down): what the finger means, in window pixels (w x h)
    template <typename Pos, typename Button>
    void event(const SDL_Event *e, float w, float h, Pos pos, Button button) {
        float x = e->tfinger.x * w, y = e->tfinger.y * h;
        uint64_t t = e->tfinger.timestamp;
        if (e->type == SDL_EVENT_FINGER_DOWN) {
            if (active) return;  // (a second finger: ignored)
            id = e->tfinger.fingerID, active = true, drag = false, flinging = false, pending = 0, vel = 0;
            x0 = last_x = x, y0 = last_y = y, last_t = t;
            hover_at = SDL_GetTicksNS() + kHoverDelay, hovering = false;
            if (tap_step == 0) pos(-FLT_MAX, -FLT_MAX);  // (a flick's hover, if any, goes)
            return;
        }
        if (!active || e->tfinger.fingerID != id) return;
        if (e->type == SDL_EVENT_FINGER_MOTION) {
            float slop = h * 0.015f;  // (~16 px on a 1080-line screen)
            if (!drag && (fabsf(y - y0) > slop || fabsf(x - x0) > slop)) {
                // a drag: the window under the finger is what scrolls from now on, and the hover goes away - with
                // one, ImGui's navigation moved its focus (and the scroll, back to the focused row) to each row
                // passing under the finger
                axis = fabsf(x - x0) > fabsf(y - y0) ? 0 : 1;
                scroll_win = scroll_window_id(x0, y0, axis);
                if (!scroll_win && axis == 0) axis = 1, scroll_win = scroll_window_id(x0, y0, 1);  // (nothing scrolls across)
                drag = true, last_x = x, last_y = y;
                if (hovering) pos(-FLT_MAX, -FLT_MAX);
                hovering = false;
            }
            if (drag) {
                float d = axis ? y - last_y : x - last_x;
                pending += d;
                if (t > last_t) {
                    float v = d / ((t - last_t) / 1e9f);
                    vel = vel * 0.6f + v * 0.4f;  // (smoothed over the last few moves)
                }
                last_y = y, last_x = x, last_t = t;
            }
            return;
        }
        if (e->type == SDL_EVENT_FINGER_UP && !drag) {  // a tap: a click where it lifted
            // (the mouse there now, pressed next frame, released the one after: ImGui learns what is under the mouse
            // while it draws a frame, so a press queued with the move - before any frame at that spot - hit nothing)
            pos(x, y);
            tap_step = 3;
        }
        active = false;
        // a flick: the page keeps going
        flinging = drag && e->type == SDL_EVENT_FINGER_UP && fabsf(vel) > 300 && SDL_GetTicksNS() - last_t < 80000000ull;
        if (!flinging && !tap_step) pos(-FLT_MAX, -FLT_MAX);  // (no hover left behind; a tap: after its click)
    }

    // The window under the finger's touch-down that scrolls along axis: the one there, or its first parent that does
    // (the last frame's windows: the mouse isn't at the finger yet, so ImGui's hovered window isn't this one)
    static ImGuiID scroll_window_id(float x, float y, int axis) {
        ImGuiWindow *w = NULL, *under_moving = NULL;
        ImGui::FindHoveredWindowEx(ImVec2(x, y), true, &w, &under_moving);
        // a popup up (a dialog, a picker): only it scrolls, not the page behind its dimmed backdrop - as ImGui itself
        // sends no mouse there (FindHoveredWindowEx doesn't know popups)
        if (GImGui->OpenPopupStack.Size > 0) {
            ImGuiWindow *popup = GImGui->OpenPopupStack.back().Window;
            if (!popup || !w || !ImGui::IsWindowWithinBeginStackOf(w, popup)) return 0;
        }
        // (across: only a strip made to scroll that way - a page a pixel too wide moved whole under a sideways drag)
        auto ok = [axis](ImGuiWindow *w) {
            return w->ScrollMax[axis] > 0 && (axis || (w->Flags & ImGuiWindowFlags_HorizontalScrollbar));
        };
        while (w && !ok(w) && (w->Flags & ImGuiWindowFlags_ChildWindow) && w->ParentWindow) w = w->ParentWindow;
        return w && ok(w) ? w->ID : 0;
    }

    // once a frame: the scroll to apply now (pixels, + = content down), the flick carried on, a tap's release
    template <typename Pos, typename Button>
    float frame(float dt, Pos pos, Button button) {
        if (active && !drag && !hovering && !tap_step && SDL_GetTicksNS() >= hover_at) {  // (a resting finger: highlight)
            GImGui->NavHighlightItemUnderNav = false;
            pos(x0, y0);
            hovering = true;
        }
        if (tap_step == 3) {  // (this frame: the mouse at the spot, hovered)
            // a controller / keyboard focus (the page's first row at start) turns ImGui's mouse hover off until the mouse
            // moves - a finger appears without moving, so its first tap found nothing under it
            GImGui->NavHighlightItemUnderNav = false;
            tap_step = 2;
        }
        else if (tap_step == 2) { tap_step = 1; button(true); }
        else if (tap_step == 1) { tap_step = 0; button(false); if (!active) pos(-FLT_MAX, -FLT_MAX); }
        float px = pending;
        pending = 0;
        if (flinging) {
            px += vel * dt;
            vel *= expf(-dt * 4.0f);  // (~a quarter of a second to settle)
            if (fabsf(vel) < 60) flinging = false, pos(-FLT_MAX, -FLT_MAX);
        }
        return px;
    }

    // After ImGui::NewFrame: the frame's scroll (frame()) straight onto the window the drag started in, pixel for
    // pixel - as wheel steps it went through ImGui's step size (5 lines of whatever font the page has) and the page
    // drifted ahead of or behind the finger.
    void scroll_apply(float px) const {
        if (px == 0 || !scroll_win) return;
        ImGuiWindow *w = ImGui::FindWindowByID(scroll_win);
        if (!w || w->ScrollMax[axis] <= 0) return;
        if (axis) ImGui::SetScrollY(w, ImClamp(w->Scroll.y - px, 0.0f, w->ScrollMax.y));
        else ImGui::SetScrollX(w, ImClamp(w->Scroll.x - px, 0.0f, w->ScrollMax.x));
    }
};
