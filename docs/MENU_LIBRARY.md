# Menu library

`ModernGekko::Menu` is a portable, header-only C++20 library under `moderngekko::ui`. `menu.hpp` defines controls; `menu_system.hpp` defines pages, navigation, editing and scrolling. The BT3 adapter supplies the presentation.

## Edit one file

Edit `src/runtime/bt3_menu_definitions.hpp`. Page factory functions define existing controls; `createMenus()` registers new pages. Labels, help, bindings, callbacks, routing and styles live here. Rebuild with `tools/build-native-menu.cmd`, then restart `Jugar BT3 menus nativos.cmd`.

Before `menus.validate()` in `createMenus()`, add:

```cpp
auto& advanced = menus.addMenu("advanced", "Advanced");
advanced.addChoice("quality", {"Low", "High"})
    .label("Quality: {value}").help("Choose rendering quality.")
    .bind(readQuality, writeQuality);
advanced.addSlider("distance", 1, 10)
    .label("Distance: {value}").help("Choose drawing distance.")
    .bind(readDistance, writeDistance);
advanced.addButton("reset").label("Reset settings")
    .onConfirm(resetSettings);
menus.menu("options").addButton("advanced")
    .label("Advanced").help("Open advanced settings.")
    .openPage("advanced");
```

Callbacks must connect to actual services; declaring Quality does not implement a rendering feature. For standard controls the adapter generates the row, text, highlight, entrance/exit motion and destination page. No renderer branch, numeric state or JSON entry is needed.

A missing footer generates Back. `.goBack().asFooter()` declares an explicit one. One footer stays last even when controls are appended after it. New pages may link to other new pages; Back restores the caller's selection. Six visible rows include the pinned footer. Longer lists scroll automatically. Duplicate IDs, multiple footers and unknown destinations fail validation.

## Bindings and editing

Choice bindings read/write zero-based indices. Sliders use inclusive integer ranges. `beginEdit()` reads a draft; `adjust()` wraps choices and clamps sliders; `confirm()` writes and then calls `onConfirm`; `cancel()` discards the draft. A failed write keeps the draft editable. Captured services must remain alive. Item references survive appending controls, but not menu replacement or destruction.

`MenuSession` handles the page stack and generic editing; the input adapter maps directions, A and B to it. Two-choice popups use opposing side motions and centered captions. Longer choices display a selection window; sliders display an editable integer. Callback-only buttons remain visible after confirmation.

Existing BT3 services preserve specific rules: live volume preview, screen-position rollback, camera preview, per-player controls and asynchronous storage. Their compatibility tags are declared alongside the controls; guest addresses stay inside the adapter. A completely new service still needs its implementation.

## Presentation

```cpp
menu.addButton("extra").label("Extra").onConfirm(callback)
    .withRowStyle({.xOffset=12, .yOffset=0, .fontSize=25});
menu.addChoice("toggle", {"ON", "OFF"}).label("Toggle")
    .bind(readToggle, writeToggle)
    .withChoiceButtons({.textAlignment=ui::TextAlignment::Center,
                        .width=168, .height=32, .fontSize=27});
```

`RowStyle::normal` and `selected` expose top/bottom RGBA, outline RGBA and outline width. Styles follow outgoing rows. Paired incoming/outgoing banks are generated from visible row definitions, including additional rows; Graphics has no special clone or label path. Playback advances on CPU composition, never renderer redraw.

The camera uses `withPreview` for fade duration, title/accent styles and arrow dimensions. `PreviewCrossFade` supports retargeting from the current mixture. Arrows follow the original 40-frame FOD curve. `withPositionGuide` controls Screen Position text and guide opacity; Start opens reset with No selected.

All captions, headings, help, values and dialogs are TrueType text. Art, backgrounds, actors, frames and plates remain local textures. Exact original font metrics, full phase parity and 16:9 are not certified. This adapter supplies the Options visual family, not an arbitrary animation editor.

## Optional translations

`menus/options.json` version 3 disables overrides by default. Enable `use_text_overrides` for translations by stable ID, or `use_theme_overrides` for font/layout overrides. JSON order never chooses actions or row order. Stale IDs are ignored so old translations cannot block a C++ edit; duplicate translations and invalid recognized entries fail. Versions 1 and 2 retain legacy override behavior.

`defaultMessages()` and `menuTheme()` in the same C++ file define shared instructions, confirmation text and fonts. Generic `{value}` displays the bound choice or integer. PC templates also accept `{width}`, `{height}`, `{scale}` and `{seconds}`. Storage bodies and BGM names come from original data as strings.

## Build and verify

Consumers link `ModernGekko::Menu`. Portable tests work without Dolphin:

```sh
cmake -S lib/ModernGekko -B build/menu-library -DMODERNGEKKO_ENABLE_DOLPHIN_RUNTIME=OFF -DBUILD_TESTING=ON
cmake --build build/menu-library --target moderngekko_menu_library_test
ctest --test-dir build/menu-library -R moderngekko.menu_library --output-on-failure
```

Runtime audits use disposable profiles, minimized windows, muted audio and normal priority:

```sh
python tools/audit_native_options_pages.py --library --output build/library-check
python tools/audit_native_options_pages.py --screen --compact --output build/screen-check
python tools/audit_original_pc_menu.py --native-replacement --output build/pc-check
```

`--library` enables test-only pages declared in the same C++ file. It checks choice cancellation/commit, slider commit, callback execution, nested navigation, 24 scrolling buttons and caller selection. The counters are test data, not shipping graphics features. Ordinary launches do not include these pages.

## Optional button offsets

Use `.offset(63)` to move a button right, `.offset(-20)` to move it left, or `.offset(20, 5)` for horizontal and vertical displacement. Omitting it keeps the authored default position. Offsets are logical menu pixels on the 512-wide canvas, so they scale with the window. They move the entire row: plate, caption, selection indicator and both incoming/outgoing animations. They preserve the other style fields. This is an explicit per-control choice; adding rows does not silently shift them away from Dende.

```cpp
menu.addButton("extra").label("Extra").openPage("extra_page")
    .offset(63);
```

## Full-screen fades and original option widgets

Only the Main/Options boundary fades from/to black, covering scenery, actors, title and controls. Graphics, Sound, Screen, Controller and their dialogs retain authored internal transitions without a black fade. Configure the Options menu for the boundary duration, in logic ticks rather than render frames:

```cpp
menus.menu("options").withFade({.enterTicks=20, .exitTicks=20});
menu.item("volume").withVolumePanel({.titleFontSize=25, .valueFontSize=26});
menu.item("bgm_test").withList({.fontSize=25,
    .scrollbar={.trackHeight=240, .minimumThumbHeight=12}});
menu.item("control_type").withPlayerSettings({.inactiveBrightness=0.55f});
```

The BGM adapter retains the original backdrop and complete right-hand track. The cyan thumb uses the visible-row count, total track count and first visible index; it fills the track for a short or empty list. White and selected gold captions both render actual strings, never the original label atlas. Source artwork still supplies the note icon, plate, track and thumb.

Controller selectors load Normal/Simple and rumble independently for 1P and 2P. The inactive tier stays selected but dimmed. These bindings use original game flags, rather than PC input remapping. Volume uses editable Music Volume/Voice Volume headings and numeric text over the original panel artwork.

`python tools/audit_native_options_pages.py --widgets --output build/widgets-check` checks selectors, volume commits, BGM scrolling, player independence and full-screen entry opacity using a disposable profile. `--profile-source`, `--scale` and `--interpolation` select test conditions without writing the source profile.

## Confirmation placement

The Graphics Apply control owns its confirmation presentation. `withChoiceButtons` controls text alignment, dimensions, font size and group offsets for Keep/Revert. `xOffset`/`yOffset` also apply to authored choice plates and generic choice popups. Omitting them keeps the default position.

```cpp
menu.item("apply").withChoiceButtons({
    .textAlignment=ui::TextAlignment::Center,
    .width=168, .height=32, .fontSize=27, .xOffset=44});
```

Current low Controller/Apply rows use `.offset(100)`, Graphics uses `.offset(120)`, and footer buttons use `.offset(140)`, to clear Dende. Sound Change Voice and the Screen/Sound/Controller Default footers have corresponding explicit offsets. This changes complete rows and both transition banks; it does not automatically reposition future controls.

## Native visual editor

The standalone `bt3-native-menu-editor` target compiles the same `NativeOptionsMenu` canvas and `NativeMenuScene` playback with a D3D11 texture uploader. `PrepareGraphics` loads resources before the UI frame, while `DrawTo(drawList, x, y, width, height)` supports a clipped embedded canvas. The game continues using `Draw()` with its existing texture backend. `SetDefinitions` and `SetMessages` update the preview without reconstructing fonts/textures.

`Menu::reorderItems` preserves callbacks and requires each ID exactly once; `removeItem` supports authoring operations. Generated project definitions live in `menus/generated/native_menu_project.hpp` only after explicit application. `createMenus(false)` and `defaultMessages(false)` construct the handwritten base for validation/editor authoring. `BT3_NATIVE_MENU_PROJECT_HEADER` can point to an isolated generated fixture for compilation tests.

Editor projects protect original service controls and preserve their callbacks. Added buttons can navigate to any declared screen; unconfigured callbacks need service code. See the repository `menus/NATIVE_EDITOR.md` guide.

## Groups, plate orientation and screen anchors

```cpp
auto& group = menu.addGroup("main_buttons");
group.xOffset = 60;
group.anchor = ui::ScreenAnchor::CenterRight;
menu.item("save").inGroup("main_buttons");
menu.item("screen").inGroup("main_buttons");
menu.item("sound").inGroup("main_buttons");
menu.item("controller").inGroup("main_buttons");
menu.item("graphics").inGroup("main_buttons");
// The rounded cap faces right, without changing callback or footer identity.
menus.menu("graphics").item("apply").withBarEnd(ui::BarEnd::Right);
menu.item("screen").anchorTo(ui::ScreenAnchor::TopLeft); // override group anchor
menu.element("dende").anchor = ui::ScreenAnchor::BottomLeft;
menu.element("help").group = "main_buttons";
```

`BarEnd::Automatic` retains the original row/footer cap direction. `Left` and `Right` mirror plate geometry around the row caption region; glyphs retain their orientation and the marker follows the cap. The same calculation applies to incoming and outgoing banks.

`ScreenAnchor` contains `Original` plus the nine combinations `TopLeft`, `TopCenter`, `TopRight`, `CenterLeft`, `Center`, `CenterRight`, `BottomLeft`, `BottomCenter`, `BottomRight`. It preserves the authored reference position and adds the corresponding displacement of the actual screen edge from the centered 512×448 canvas. It does not snap elements to the edge. The renderer handles its separate horizontal/vertical artwork scale. Defaults preserve letterboxing; explicit anchors may occupy the letterbox region.

Groups are local to a menu. Group and member offsets add. An `Original` member anchor inherits its group's anchor; an explicit member anchor overrides it. An ungrouped Original item retains authored coordinates. Unknown groups and invalid anchors/offsets are rejected by `MenuSystem::validate`. `resolvedStyle` and `resolvedElement` expose the resolved values without baking them back into the source definitions. No nested groups are supported.

Named visual elements are `title`, `icon`, `cloud_left`, `cloud_right`, `title_line`, `dende`, `piccolo`, `help`, and `background`. `header` is a compatibility fallback for older projects; individually declared header components override it. They have per-screen offsets, optional groups and anchors; defaults are zero/Original. Background art retains the authored 4:3 canvas by default. `element("background").coverScreen=true` cover-scales original scenery or targets the full screen with a full PNG override. This does not automatically rearrange controls.

## Artwork and custom decorations

```cpp
auto& graphics = menus.menu("graphics");
graphics.element("icon").scale = 1.2f;
graphics.element("cloud_left").yOffset = 12;
graphics.element("cloud_right").opacity = 0.75f;
graphics.element("dende").facialAnimation = false; // static replacement actor
graphics.images["22-0"] = {"mods/menus/native/images/my_actor.rgba", 256, 256};
graphics.images["background_full"] = {"mods/menus/native/images/my_background.rgba", 1280, 720};
graphics.element("background").coverScreen = true;
graphics.element("background").imageFit = ui::ImageFit::Cover; // also Contain or Stretch

graphics.decorations["subtitle"].text = "Custom settings";
graphics.decorations["subtitle"].fontSize = 22;
graphics.decorations["subtitle"].zIndex = 5;
graphics.element("subtitle").xOffset = 250;
graphics.element("subtitle").yOffset = 80;
graphics.element("subtitle").anchor = ui::ScreenAnchor::TopCenter;
```

`ElementLayout` exposes uniform `scale`, `opacity`, `visible`, offsets, anchor and group. Multipart actors and help frames share a transform pivot; text follows frame position/scale. Facial animation can be disabled for a static Dende replacement without stopping the underlying game/audio services. Cloud motion still comes from the original independent FOD clips.

`ImageReplacement` holds a raw RGBA8 file and dimensions. The editor decodes PNGs with WIC at their original resolution (up to 4096×4096), bypasses scaling when dimensions match, copies immutable RGBA data into the project image directory and emits matching declarations. Relative paths are resolved from the repository containing `menus/options.json`. Texture replacements are per menu, cached separately from original artwork and restored by removing the override. Original resources are not overwritten. A missing/invalid file logs a warning and falls back to original art, or hides a custom image.

`Decoration` is a noninteractive image or real text, with width/height, font size, text style, foreground/background band and `zIndex`. Its ID indexes `Menu.elements` for layout/group settings. Image decorations use a key present in `Menu.images`. Custom layers sort by zIndex within their band. Original FOD internal depths remain authored. Custom decorations render before the full-screen black fade and cannot add settings behavior; read/write services remain C++ controls.

### Help panel edge

`menu.element("help").helpEdge` accepts `ui::HelpPanelEdge::Original`, `ExtendLeft`, or `RoundedLeft`. Original preserves the authored frame images. ExtendLeft fills to the actual screen's left edge; RoundedLeft uses native rounded left corners at the element's transformed position. `cornerRadius` is measured in logical menu units (default 24, accepted 0–224). Both native shapes follow the frame's CPU playback and element layout. For example:

```cpp
menu.element("help").helpEdge = ui::HelpPanelEdge::RoundedLeft;
menu.element("help").cornerRadius = 30;
```
