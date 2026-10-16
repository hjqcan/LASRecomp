# Photo mode

During gameplay press **F2**, or open **Settings → Photo mode → Enter photo mode**. The next complete scene is frozen. **F2** or **Escape** resumes; **Tab** hides the controls; **F12** saves a PNG under `photos/` in the application's data directory. The panel also has Save and Resume buttons.

Adjust zoom (1–4x), horizontal/vertical framing, exposure and saturation. Photos use the current window resolution and exclude the photo controls and FPS counter. Zoom crops the rendered scene: this is not a free 3D camera, depth-of-field editor or supersampling renderer. Interface hiding recognizes the final scene composite; if a scene lacks that pass, the panel reports that hiding is unavailable. Changing the interface setting renders a fresh frame before freezing again. Save before entering if preserving an exact game state matters; photo mode is not a save-state system.

The presentation and color paths are implemented for Vulkan and Metal. Windows Vulkan has been exercised in game; native Metal rendering still needs hardware validation.

## Validation

Run `python tools/test_photo.py` for asset-free state/crop/composite checks using `CC` (Clang by default). For gameplay testing, enter a scene, press F2, adjust each control, save twice without changing the scene, and verify matching images with no overlay. Resume and verify movement. Check window resizing, UI visibility and a scene without a recognized final composite. A frozen presentation does not constitute a save state or a complete simulation pause.

`python tools/photo_smoke.py --exe PATH --game EXTRACTED_GAME --output NEW_DIRECTORY` automates the opening-room freeze, two matching captures and resume with isolated settings/saves. The game library must already be compiled. Optional `--save-copy SAVE_MOUNT` copies a version-compatible save without changing the source. Inspect the images and `runtime.log` even if `result.json` reports success; adjust scripted input timing when a machine reaches title/name-entry screens at a different frame.
