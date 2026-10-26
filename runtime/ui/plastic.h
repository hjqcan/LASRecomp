// The settings menus' look (runtime/ui/plastic.mm): a paper panel in a brown moulded frame, rounded plastic rows, tabs
// and buttons, recessed wells. Each piece is drawn by our own small shader (Metal, or Vulkan: runtime/nvn/vulkan/
// plastic_vk.c) as an ImGui draw callback, under the text drawn after it. Used by the in-game menu and the start
// screen's Settings / game files windows; nothing runs while they're closed.
#pragma once
#include "imgui.h"

struct PlasticTheme {
    ImVec4 panel, inset, frame, frame_inner, row, row_sel, row_mark, row_hot, button, tab, tab_sel;
    ImU32 text, text_dim, text_sel, text_button, section, value, value_changed, warn, title, arrow;
    float dim;    // the game behind the menu: black at this alpha
    float glare;  // over a dark scene the pieces come down by up to this much
};
const PlasticTheme &plastic_theme(void);
static inline ImVec4 plastic_col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

enum PlasticKind { PK_SLAB = 0, PK_INSET = 1, PK_PILL = 2, PK_RING = 3 };
// a piece at [a, b] (pixels), drawn into dl at this point of its commands (so: under what's added after it).
// depth: the bevel's width, and the drop shadow's reach; gloss 0..1; ring: the frame's width (PK_RING)
void plastic_add(ImDrawList *dl, ImVec2 a, ImVec2 b, float radius, ImVec4 col, int kind, float depth, float gloss,
                 ImVec4 col2 = ImVec4(0, 0, 0, 0), float ring = 0);
// a big panel: the paper slab in its frame (the menu window, popups, the start screen's windows)
void plastic_panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float scale);
// a well (recessed) - the panes, text fields
void plastic_well(ImDrawList *dl, ImVec2 a, ImVec2 b, float scale);
// the round close X at the cursor (size: its diameter); true when clicked
bool plastic_close_button(float size, float scale);
// that X in the current window's top right corner, inside the frame; the layout is left as it was
bool plastic_close_corner(float size, float scale);
// While on (the menus' windows): ImGui's own frames (buttons, text fields, headers, check boxes) and window / popup
// backgrounds are drawn as pieces too. The caller turns it off again before anything else is drawn.
void plastic_widgets(bool on);

// The backends: before the draw data is rendered, once per frame the pieces are drawn in. scene: the game image
// behind the menu (its uv rect), for the glare guard; none on the start screen.
#ifdef __APPLE__
#ifdef __OBJC__
#import <Metal/Metal.h>
void plastic_frame_begin(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLTexture> scene,
                         const float scene_uv[4], ImVec2 mouse, float scale);
void plastic_set_encoder(id<MTLRenderCommandEncoder> enc);  // around ImGui_ImplMetal_RenderDrawData
void plastic_perf_note(id<MTLCommandBuffer> cb, bool menu_open);  // (LARECOMP_UI_PERF)
#endif
#else
// Vulkan: inside the window image's rendering (scene: a VkImageView, or NULL)
void plastic_frame_begin_vk(void *scene, const float scene_uv[4], float w, float h, ImVec2 mouse, float scale);
void plastic_perf_note_vk(bool menu_open);  // (LARECOMP_UI_PERF)
#endif
