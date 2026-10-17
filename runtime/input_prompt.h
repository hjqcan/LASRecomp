// Button legends for host overlays, using the game's prompt style and current controller bindings.
// PlayStation face symbols are drawn as geometry so they work with every menu font.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum { INPUT_PROMPT_TEXT, INPUT_PROMPT_CIRCLE, INPUT_PROMPT_CROSS, INPUT_PROMPT_TRIANGLE, INPUT_PROMPT_SQUARE };
typedef struct { int symbol; const char *text; } InputPrompt;
InputPrompt input_button_prompt(int slot);  // BIND_A..BIND_MINUS, as in the game's own button prompts
#ifdef __cplusplus
}
#endif
