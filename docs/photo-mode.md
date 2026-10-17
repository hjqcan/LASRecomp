# Photo mode

During gameplay press **F2**. The next complete scene is frozen. **F2** or **Escape** resumes; **Tab** hides the controls; **F12** saves a PNG under `photos/` in the application's data directory. The panel also has Save and Resume buttons.

Drag the photo with the mouse to pan; use the scroll wheel or **+ / -** (including the numeric keypad) to zoom between 1x and 4x. Panning stays inside the rendered image. Exposure, saturation and **Hide game interface** remain in the panel.

With a controller, select **Frame photo**. The panel fades away and the lower-right legend shows the controls: **left stick** pans and **right stick up/down** zooms. The game's mapped confirm button takes a photo and returns to the panel; its mapped cancel button restores the previous view. The legend follows **Button prompts**, including Auto controller detection and remapping: PlayStation symbols, Xbox letters or Nintendo letters. Focus returns to Frame photo. Mouse and keyboard framing also work while the controls are hidden with Tab.

The saved message shows only the file name. The adjacent **Show in Finder** / **Show in Explorer** / **Open folder** button reveals the photo (or opens its folder on Linux).

 Photos use the current window resolution and exclude the photo controls and FPS counter. Zoom crops the rendered scene: this is not a free 3D camera, depth-of-field editor or supersampling renderer. Interface hiding recognizes the final scene composite; if a scene lacks that pass, the panel reports that hiding is unavailable. Changing the interface setting renders a fresh frame before freezing again. Save before entering if preserving an exact game state matters; photo mode is not a save-state system.

The presentation and color paths are implemented for Vulkan and Metal.

## Validation

Run `python tools/test_photo.py` for asset-free state/crop/composite and real ImGui input/focus checks using `CC` / `CXX` (Clang by default). The prompt checks cover Auto, explicit styles and remapped buttons/triggers, including their actual framing actions. For gameplay testing, enter a scene, press F2, drag/zoom with the mouse and keyboard, exercise controller framing and return, adjust each remaining control, save twice without changing the scene, and verify matching images with no overlay. Check that the saved message contains no account path and the folder button selects the saved file where supported. Press F2 on a screen where photo mode is unavailable and verify the overlay and any game binding still work. Resume and verify movement. Check window resizing, UI visibility and a scene without a recognized final composite. A frozen presentation does not constitute a save state or a complete simulation pause.

`python tools/photo_smoke.py --exe PATH --game EXTRACTED_GAME --output NEW_DIRECTORY` automates the opening-room freeze, two matching captures and resume with isolated settings/saves. The game library must already be compiled. Optional `--save-copy SAVE_MOUNT` copies a version-compatible save without changing the source. Inspect the images and `runtime.log` even if `result.json` reports success; adjust scripted input timing when a machine reaches title/name-entry screens at a different frame.
