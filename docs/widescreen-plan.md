# Native widescreen: study and plan

Goal (2026-11-21): on the Vita, show more of the world horizontally instead of stretching the
640x480 image. This document lists, with evidence from `HDKWIN.EXE`, where the game depends on the
640-pixel width, and proposes an implementation through `recomp/image_patches.py` and the port's
DirectDraw layer. Sections 1-8 are the study; the status below tracks the implementation.

## Status

- **Milestone 1 done (2026-11-21)**: `port/hk/wide.c` (`g_hk_ws_w`, hooks), `recomp/image_patches.py` `HOOKS`
  `0x40D1DD`, `0x4220DC`, `0x45219E`, `0x455E16` and the diagnostic `0x42208E`; primary/back buffer and
  `GetDisplayMode` at `g_hk_ws_w x 480` (`port/hk/ddraw.c`; the port's dialogs are drawn on the centred 640
  columns). PC: `HK_WIDE=848`. Vita: `wide_native=1` in `hkport.cfg` (off by default, no pause toggle; the
  848x480 frame uses logical size 848x480, i.e. it fills 960x544 at the 4:3 setting).
- Check (host, `SH3_VTIME=1 HK_NOSOUND=1 HK_KEYS=1300:0d:6 HK_SHOT=600,2200`): without `HK_WIDE` both shots are
  byte-identical to the build before the change (md5 `24db817e7965915aed5a0dac55740cff` frame 600,
  `0c0a272b20dd0a9300eab33361ffa159` frame 2200). With `HK_WIDE=848` frame 2200 shows an 848-wide A0
  playfield, Handkerchief kept at the same offset from the centre of the follow window, no garbage.
- Area sizes logged so far (`[ws] area W=... H=...`): A0 run gives `6400x640` and `9600x960`; none narrower
  than 848 yet (other territories not walked).
- **Milestone 2 done (2026-11-21)**, natives in `port/hk/wide.c`, hooks in `recomp/image_patches.py`:
  - Sky `0x447929`: hook `0x447994` adds 640-period copies through `0x408B7B` until the clip rect is covered.
    Variant `0x447A89` (picture + side fills + two scrolling layers): `0x447AA4` centres the composition,
    `0x447B08` / `0x447B2C` widen the left/right colour fills to the clip rect, `0x447BD1` adds layer copies.
  - Top bar: it is a sprite element (ctor `0x413B64`, draw `0x413DBF`, x = 320, sprite `0x28008005` = bank 10
    entry 5, offset -320) in the scene list `[scene+0x38]` drawn by `0x454908` at `0x454AFB` with origin (0,0).
    The bottom HUD panels are children of the same list, so one hook (`0x454AF7`, x origin) centres both. The
    panel-constructor hook proposed in section 7 is not used (it would double-shift the HUD).
  - Territory title card: list `[scene+0x48]` (drawn at `0x454ADA` while `[scene+0x68] == 0`), hook `0x454AD4`.
  - Other panels created outside those lists: x argument shifted at the calls `0x4556B0`, `0x4567F9`, `0x4570E5`
    (`0x4570B6` panel stored in `[scene+0x68]` by `0x455414`, drawn top-level at `0x454FAB`). Not seen on screen
    yet: unverified.
  - Margins (hook `0x454AFE`): blits from HDKCON00 (bank 10 surface `[0x4AA40C]`, 640x160: top bar rows 0..31,
    HUD background rows 32..159), never from the screen. Top bar: flat left end at the left, plain texture,
    bevelled right end at the right edge; HUD: inner-frame background columns 70.. on both sides. Skipped unless
    bank 10 entry 5 is (0,0,640,32).
  - Default 640: md5 unchanged at frames 600 and 2200. Port dialogs and the Vita pause overlay were already drawn
    on the centred 640 columns (milestone 1).
- Walk with `HK_WIDE=848` (`HK_KEYS=1300:0d:6,1500:27:500,2100:25:400`, shots 1600..2600): A0, a1, b0, d0, j0
  show no gaps, garbage or misplaced UI. Area sizes seen: 1920x960, 3840x1280, 5760x800, 6400x640, 6400x1120,
  7680x960, 9600x960; none narrower than 848 so far. j0's black sky is the same at 640 (night stage). Not seen in
  still shots: enemy pop-in, the vista right edge at the far end of an area, the second scene class `0x4565AA`.
- **Pillarbox for 640-only scenes (2026-11-21)**, after the console test of 1aa28c3 (intro logos, title and the
  pause overlay were drawn at the left of the 848 frame): only the stage scene class (vtable `0x4724B8`, seen at
  the bar hook `0x454AF7` from the first A0 frame on) is drawn wide. Every other frame (intro, title = class
  `0x472500` with view `(0,0,640,480)` at `0x4565AA`, menus, game over) has its 640 columns centred with black
  pillars at present time (`hk_ws_flip` / `hk_ws_frame_ox` / `hk_ws_pillarbox` in `port/hk/wide.c`, called by
  `port/hk/ddraw.c`); the decision is taken per Flip and kept for palette/dialog presents. The title hooks
  `0x455E16` (logo centring in `0x455D87`) and `0x4556B0` (menu panel in ctor `0x455548`) were removed, since
  that scene is now pillarboxed as a whole. The Vita pause text block is centred on the frame width.
  Check: default md5 unchanged at frames 60/200/900/2200; `HK_WIDE=848 HK_TRACE_WS=1` logs one switch
  (`frame 1315: wide`, territory card), title and card centred, A0 wide.
- **HUD anchored to the edges (2026-11-21)**: the bottom HUD box (vtable `0x46FAF0`, draw `0x434890`:
  left frame 384 px + filler 136 px + right frame 120 px from HDKCON00 rows 32..159) stays at x 0, the filler is
  repeated over the extra width and the right frame moves to the right edge (hook `0x4349A3`); its children with
  x >= 320 (fish/money panel, x `0x212`) move by the full extra width, the weapon panel and soldier list stay at the
  left (hook `0x4135C8` in the generic list draw `0x4135A0`, active only while `[scene+0x38]` is drawn); the box's
  640-wide child rect (`box+0x44..0x50`, clipped by `0x411BED`) is widened. Top bar and the `(320,160)` element stay
  centred. Check: default md5 unchanged at frames 600/2200; `HK_WIDE=848` frame 2200 shows weapon panel at x 0 and
  money panel ending at 848.
- Former milestone 2 notes: top bar (rows 0..31) and the HUD strip (rows 352..479) stay 640 wide and
  left-aligned, columns 640..847 black there; the sky looked continuous in the A0 shot but the 640-period
  sky draw (`0x447929`) is still unpatched, so a gap is expected at other scroll positions.

Reproduce the disassembly with `objdump -d -Mintel original-data/HDKWIN.EXE` (or `build/hdkwin.asm`).
Function starts come from `build/hk-recomp/re/functions.json` (Ghidra).

## 1. Screen layout (observed)

Run of the recompiled game (host build, `HK_KEYS=1300:0d:6 HK_SHOT=2200`, stage A0 after NEW GAME),
row sampling of the 640x480 frame:

| Rows | Content | Evidence |
|---|---|---|
| 0..31 | top bar ("Handkerchief." title frame) | screenshot; drawer not identified yet |
| 32..351 | playfield, 640x320 | view rect `(0,32)-(640,352)` set at `0x452189..0x452197` (screen size 0) |
| 352..479 | bottom HUD (item box, gauge, MONEY), 640x128 | panels created at `(0,352)` by `0x4345BD` / `0x434810` |

`HDKCON00.BMP` is 640x160 but it is a sprite sheet; the HUD strip itself is 128 rows tall.
All sky bitmaps `HDKSKY01..04.BMP` are 640x320 (exactly the playfield); `HDKSKY00.BMP` is 640x480.

## 2. Camera context (the key structure)

A 0x4C-byte object; methods at `0x4119A2..0x411F7E`. Layout (from the methods below):

| Offset | Meaning |
|---|---|
| `+0x00`, `+0x04` | X, Y: world-to-screen offset (screen = world + X) |
| `+0x08`, `+0x0C` | extra offset (zeroed by the setters) |
| `+0x10..+0x1C` | view rect L,T,R,B in screen pixels (also the clip rect for drawing) |
| `+0x20..+0x2C` | copy of the view rect (used by the anchor query `0x411EA5`) |
| `+0x30..+0x3C` | follow window rect in screen pixels (the target is kept inside it) |
| `+0x40`, `+0x44` | world (area) width and height |
| `+0x48` | step (8 in `0x4119A2`, 0x20 for the scene camera) |

- `0x4119A2(rect)`: init; view = follow = rect, W/H = rect size.
- `0x411A11(view, follow, W, H, step)`: full init (scene constructor `0x451C30`, call `0x451D2C`).
- `0x411B98(view, follow)`: sets view and follow rects and shifts X/Y by the change of the view's L/T.
  Called by `0x452093` (main scene) and `0x4565AA` (second scene class).
- `0x411C60(target rect)`: follow; then clamps `X <= L` and `X + W >= R` (`X = R - W`), same for Y.
- `0x411D1B` / `0x411D9D`: follow the centre of the follow rect (`(+0x30 + +0x38)/2`), the second one
  limited to +-8 px per call; same clamp.
- `0x42206D`: sets camera W/H (`+0x40/+0x44`) from the area rect `[[[scene+0x28]+0x3C]+0x6C]+0x14`
  (writes at `0x422088`, `0x42208B`). This is the only writer of W/H found by a static scan.

Consequence: the clamp, the follow logic and the anchor points are all expressed through the view
rect, not through the literal 640. Widening the view rect widens the camera.

### Screen size option (already in the game)

`0x452093` reads CConfig `+0x14` (screen size, see `port/hk/dialog.c`) and calls `0x411B98`:

| Size | View rect | Follow rect | Sites |
|---|---|---|---|
| 0 normal | `(0,32)-(640,352)` | `(270,168)-(370,256)` | `0x452169..0x452197` |
| 1 middle | `(32,64)-(608,320)` | same | `0x452112..0x45214F` |
| 2 small | `(64,96)-(576,288)` | same | `0x4520C3..0x452100` |

The engine already runs with view widths of 640, 576 and 512 and the follow rect stays centred at
x = 320. A wider view is an extension of an existing, tested mechanism.

## 3. Sites that assume 640 (or 480)

A full scan for the immediates `0x280` (640) and `0x1E0` (480) gives 17 and 12 hits. All are listed.

| Address | Bytes | Role | Widescreen action |
|---|---|---|---|
| `0x40D1CF`, `0x40D1D6` | `c7 45 f8 80 02 00 00`, `c7 45 fc e0 01 00 00` | static init of the global camera context `0x4AA330` with `(0,0,640,480)`. Its view rect `0x4AA340` is the screen/clip rect used by full-screen code (`0x40D7D4` clear, `0x455E18` overlay clip, `0x455E89`/`0x455EE1` text centred at `[0x4AA348]/2`, and as clip at `0x40DD9E`, `0x40F3A6`, `0x413F06`, `0x435750`) | must become `(0,0,WS_W,480)` |
| `0x4220CD`, `0x4220D5` | `c7 45 f0 80 02 00 00`, ... | `0x422098`: colour-fill `(0,0,640,480)` clipped to `0x4AA340` (back buffer clear, counter `[esi+0x3C]`) | WS_W |
| `0x452190` | `c7 45 f8 80 02 00 00` | screen size 0: view rect right = 640 | WS_W |
| `0x452169`, `0x452177` | `c7 45 e0 0e 01 00 00`, `c7 45 e8 72 01 00 00` | follow rect L=270, R=370 (shared by the 3 sizes) | +(WS_W-640)/2 (only for size 0, otherwise sizes 1/2 become off-centre) |
| `0x4565D0..0x4565FF` | `0x12C,0xF6,0x190,0x152` / `0x280,0x1E0` | `0x4565AA`: second scene class (vtable `0x472500`, ctor `0x455548`) sets view `(0,0,640,480)` and follow `(300,246)-(400,338)`; identity of the scene not verified | WS_W and +offset, or keep 640 and centre (pillarbox) |
| `0x434643..0x434685` | `0x280`, `0x1E0` at `0x43466D`, `0x434674` | panel base constructor `0x434629(x, y, w, h)`: x in `+0x14`, y in `+0x18`, own camera context at `+0x24` init `(0,0,640,480)` then `(0,0,w,h)` | no change; x is the hook point for centring (see 5) |
| `0x4345CA`, `0x434818` | `mov edi/eax,0x280` | bottom panels `(0,352,608,96)` growing to 640x128, and `(0,352,640,128)` | centred through the panel x hook |
| `0x455E11` | `b8 80 02 00 00` | `0x455D87` (main scene draw): overlay sprite `[0x4ABE3C]` at x = (640 - w)/2, y = 100 - h/2, clipped to `0x4AA340` | WS_W |
| `0x44797A` | `81 eb 80 02 00 00` | sky draw `0x447929` (class vtable `0x46EC78`, `HdkSKY%02d.BMP`): blits the 640-wide sky at x and x-640 | needs a third copy at x+640 (code) |
| `0x447A42`, `0x447A54` | `3d 80 02 00 00`, `05 80 02 00 00` | sky scroll update `0x4479B7`: wraps the scroll position into [0,640] (bitmap period) | keep (period of the bitmap, not of the screen) |
| `0x447B1D`, `0x447B94` | `8d 87 80 02 00 00`, `b8 80 02 00 00` | `0x447A89`, sky variant vtable `0x46ECB8`: clip/fill rect 640 wide | needs analysis, probably WS_W |
| `0x40D52E`, `0x40D535`, `0x40D56D`, `0x40D57E`, `0x40D73A`, `0x40D744` | | windowed path of the display init `0x40D499` (window size/centring, 640x480 offscreen back buffer) | unused by the port (log: `SetCooperativeLevel 0x11`, `SetDisplayMode 640x480x8`, primary caps `0x218` with 1 back buffer) |
| `0x40D66D`, `0x40D672` | `68 e0 01 00 00`, `68 80 02 00 00` | `SetDisplayMode(640,480,8)` (fullscreen path, used) | port side: the port sizes the primary/back buffer itself |
| `0x40DC86`, `0x40DCF4` | | `0x40DBD8`: window placement against the desktop height | irrelevant on the port |
| `0x41A043` | `3d e0 01 00 00` | `0x419C4F` (actor movement, see `docs/hdk-format.md`): `abs(path.x - obj.x) > 480` clears `[obj+0x8C]` | world-space threshold, not the view; keep |
| `0x4573AC` | `8d 83 e0 01 00 00` | `0x457345`: text/sprite at x = base + 480 inside a panel | panel-relative; keep |

Other screen-centre constants (320 = `0x140`) to triage when they show up wrong:
`0x453CFA` (sprite centred at 320 - w/2, `0x453C7B`), `0x454581` (screen object at x = 320,
`0x454261`), `0x45281A` / `0x454524` (objects created at (320,160)), `0x4557BA` (`0x455548`),
`0x426A88` (`0x426A11`, 320 used as a distance, probably sound attenuation).
No `0x27F`, `0x13F` or `0x1DF` immediates exist.

## 4. Other assumptions checked

- **Drawing and clipping**: blits go through `0x408C75` / `0x408B7B` (BltFast with a clip rect) and
  the colour fill `0x408EA3` (Blt `DDBLT_COLORFILL|DDBLT_WAIT`, clipped with `0x4012B2`); the clip is
  the camera view rect, so tiles, `STAGE_0`, `FRONT_0` and actors are clipped to the wider view
  without changes.
- **VISTA_0 parallax** (`0x42309E`): `X_vista = X - trunc((X - L)/3)` is generic. Risk: at the far
  right of an area the vista must cover `WS_W + (W - WS_W)*2/3` instead of `640 + (W - 640)*2/3`,
  i.e. about `(WS_W - 640)/3` (about 70 px for 848) more on the right; the authored elements may stop
  short.
- **Sky** (`0x424AF8`, stage draw): with View Sky on, a copy of the camera is passed to the sky draw
  (vtable slot `+0x14` = `0x401BE5 -> 0x447929`); with View Sky off, the view rect is filled with
  colour `0xF8` (`0x411F5F`), which already scales.
- **Actor activation**: the area update `0x4232E4` (called from `0x424C5F` with the camera) updates
  every object list each frame and does not read its camera argument in its own body. No view-based
  activation window was found at the area level (hypothesis: actors use world-space distances such as
  `0x41A043`; individual enemy classes were not all traced).
- **Fades**: the game runs in 8-bit palette mode; fades are expected to be palette changes
  (resolution independent). Not traced; check in milestone 2.
- **Speech balloons / text**: text is drawn through `0x410B49` (`0x403314`) with positions computed
  from the global rect (`[0x4AA348]/2`) or from panel coordinates; balloons not traced.
- **Port side**: `port/hk/ddraw.c` forces the primary to 640x480 (lines 184-185), checks the display
  mode (222), reports 640x480 in `GetDisplayMode` (232) and converts a fixed `frame[480][640]`
  (261-272); `port/vita/main.c` uses 640x480 in `plat_width/height`, the SDL logical size, the font
  overlay, the pause dim and the FPS buffer.

## 5. Target width

The Vita screen is 960x544. At the game's height of 480 the same aspect gives 847 px, so
**WS_W = 848** (multiple of 16; scale 1.1333 maps it to 961x544, one column cropped) fills the screen
without bars. Extra width: 208 px; centring offset for 640-wide UI: 104 px. (854 would be exact 16:9
but the Vita is not exactly 16:9.)

## 6. Risks

1. **Areas narrower than WS_W**: the clamp (`0x411C60` etc.) sets `X = L` when `X > L`, else
   `X = R - W` when `X + W < R`. With `W < WS_W` the first test wins or alternates; the area is shown
   left-aligned with garbage/sky on the right. Mitigation: per area, use a view of `min(WS_W, W)`
   centred in the screen (pillarbox), applied when W/H is set (`0x42206D`). Area widths are not known
   yet (measure in milestone 1).
2. **Sky**: 640-wide bitmaps drawn twice; a gap appears on the right without a third copy.
3. **Parallax edge** (see 4): possible empty strip at the far right of some areas.
4. **Objects woken early / seen too early**: the player sees 104 px further on each side; enemies or
   events triggered by world position may be visible before they "spawn", and scripted scenes framed
   for 640 may show actors waiting off-screen.
5. **UI**: panels, menus and dialogs are positioned in 640 space (left-aligned in a wider screen);
   the top bar drawer is unknown; the HUD strip needs side fill.
6. **Fixed patches vs. option**: `IMAGE_PATCHES` are fixed at recompile time. A runtime 4:3/wide
   switch needs `HOOKS` that overwrite the stack locals after the original `mov` (e.g. a hook at
   `0x452197` writing `[ebp-0x8] = g_view_w`).
7. **Second scene class** (`0x4565AA`, full 640x480 view): unknown role; widening may show unintended
   content. Default: keep 640 and centre it.

## 7. Implementation plan

Use hooks reading a port global `g_ws_w` (640 = original behaviour, 848 = wide), so the original 4:3
path stays the default and is testable.

**Milestone 1 (small, verifiable): wide playfield in A0, screenshot evidence.**
- Port: `g_ws_w` from an env var (`HK_WIDE=848`) on PC; primary/back buffer `g_ws_w x 480` in
  `dd_create_surface`, `GetDisplayMode`, the conversion buffer and `plat_present_xrgb`.
- `HOOKS`: after `0x40D1D6` (global context rect right = `g_ws_w`, i.e. hook at `0x40D1DD` writing
  `[ebp-0x8]`); after `0x4220D5` (hook at `0x4220DC`, `[ebp-0x10]`); after `0x452197` (hook at
  `0x45219E`, reached only by screen size 0: `[ebp-0x8] = g_ws_w`; the follow rect was already
  copied to the outgoing arguments at `0x452185..0x452188`, so add `(g_ws_w-640)/2` to the stack
  copy `[esp+0x0]` and `[esp+0x8]`, not to `[ebp-0x20]`/`[ebp-0x18]`); `0x455E11` overlay centring (hook at `0x455E16`, `eax = g_ws_w`).
- Diagnostic hook at `0x42208E` logging area W/H once per area (`[ws] area W=... H=...`).
- Check: `HK_WIDE=848 HK_KEYS=1300:0d:6 HK_SHOT=2200` shows an 848-wide A0 playfield, camera
  following Handkerchief centred, no clamp jump at the area edges; `HK_WIDE` unset gives a frame
  byte-identical to the current one. Expected defects: sky gap on the right, HUD left-aligned.

**Milestone 2: sky and HUD.**
- `ADDR_WRAPS` for `0x447929` (draw the third sky copy at x+640 through `0x4014EC`); analyse and
  fix `0x447A89`.
- Hook in the panel constructor at `0x43464C` (`eax` = x): `eax += (g_ws_w-640)/2` centres the HUD,
  dialogs and menus (5 callers: `0x4345DF`, `0x434828`, `0x4556B0`, `0x4567F9`, `0x4570E5`); then
  find the top-bar drawer and fill both HUD side strips (black or tiled from `HDKCON00.BMP`).

**Milestone 3: narrow areas and edge cases.**
- Pillarbox in the `0x42208E` hook when `W < g_ws_w` (shrink view/follow rects, centred).
- Walk all territories (HK_STAGE) checking the vista right edge, scripted scenes and the 320-centred
  sites of section 3; decide for `0x4565AA`.

**Milestone 4: Vita.** 848x480 texture scaled to 960x544; option in the port's pause/config screen.

## 8. Open questions

- Who draws the top bar (rows 0..31)?
- Area widths (all territories) and whether any is narrower than 848.
- Role of the scene class with vtable `0x472500` and of the sky variant `0x46ECB8`.
- Whether enemy classes test visibility against the camera anywhere (only the area-level update was
  checked).
