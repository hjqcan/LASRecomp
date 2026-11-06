# Vulkan per-GPU defaults

Some Vulkan settings are on by default only for certain GPUs or drivers, because they were measured or needed there.
Each default was tested on one driver, so other drivers with the same vendor get it untested. If a report comes from
one of these GPUs and looks like a rendering or speed bug, turn the default off with its switch (an environment
variable, `=0` off, `=1` on) and see if the problem goes away. The log names each choice and whether it came from the
switch or the default for that GPU (`[vk] ...` lines near the start).

The defaults are set in `runtime/nvn/vulkan/exec.c` (`vk_exec_init` and nearby) and `runtime/nvn/vulkan/device.c`.

Vendor IDs: 0x13B5 Arm (Mali), 0x5143 Qualcomm (Adreno), 0x8086 Intel, 0x1002 AMD.

| Default | Where it's on | Tested on | Added | Switch |
|---|---|---|---|---|
| Structured shaders (static branch stacks, no jump loop) | Mali, Adreno | PanVK (RK3588), Turnip | #562 | `LARECOMP_VK_STRUCTURE` |
| c1 inlined, hot constant buffer words as push constants | Mali, Adreno | PanVK; Turnip (Adreno 643) | #563, #579 | `LARECOMP_VK_PUSH_CB`, `LARECOMP_VK_INLINE_C1` |
| Split pass barriers | Mali | PanVK | | `LARECOMP_VK_SPLIT_BARRIER` |
| Branch stacks as variables instead of arrays | Intel | | #462 | `LARECOMP_VK_STACK_ARRAYS=1` gives arrays |
| Placeholder colour attachment for depth-only passes | Intel | UHD 630 on Windows (a driver crash without it, #8, #46) | | `LARECOMP_VK_PLACEHOLDER_COLOR` |
| Guest heap as driver memory | RADV, PanVK | PanVK (Mali-G610), RADV | #564, #483 | `LARECOMP_VK_HEAP_DMABUF` |
| Shader memory through copies (no heap import) | drivers without `VK_EXT_external_memory_host` | PanVK | #556 | none (automatic) |

Notes:

- The Adreno defaults were measured on Turnip (Mesa) only. Qualcomm's own Windows and Android drivers are untested.
  Signs to watch for there: wrong lighting or colours, black or flickering objects, a crash while building
  pipelines. Try `LARECOMP_VK_PUSH_CB=0` first.
- The shader and pipeline caches key on the shader text, so switching a shader default rebuilds those shaders once.

## Adding a default

Key it by vendor ID (or driver ID when one vendor has several drivers), give it a `LARECOMP_VK_*` switch that works
both ways, log the choice, say in a comment which driver it was measured on, and add a row here.
