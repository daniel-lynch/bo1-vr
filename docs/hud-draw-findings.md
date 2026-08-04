# BO1 2D HUD draw path — RE findings

Target: `BlackOps.exe` (32-bit PE, image base `0x400000`).
All addresses are **virtual addresses** (image-base-relative).

Clean-room: everything below was derived from this binary only (objdump disassembly, Ghidra
decompiler, PE section/data-table parsing). No external decompilation, PDB or source dump
was consulted. Companion doc: `crosshair-draw-findings.md` (whose `cg_t*` = `0x2FF5354`,
`scrPlace` array and `CG_DrawCrosshair` `0x4100F0` findings are re-used and independently
re-confirmed here).

**Nothing in this document has been checked against a running process.** Every claim is
tagged VERIFIED (directly readable in the binary, re-derived here) or INFERRED (reasoned).

---

## 0. TL;DR — the answer

Black Ops already contains a complete, engine-supported path for rendering 2D HUD content
into an off-screen texture that a world-space surface can sample. It is called **UI3D**.

* Every 2D draw becomes a **render command** in a shared command buffer. Each command carries
  a one-byte **UI3D window tag** (`cmd+3`).
* If the tag is clear, the command is executed in the normal framebuffer 2D pass.
  If the tag is set (`0x80 | window`, window 0..5), the command is executed **only** by
  `RB_UI3D_Render`, which binds `R_RENDERTARGET_UI3D` (`0x14`) instead of the framebuffer.
* The tag comes from a tiny push/pop stack (`0x3E4DE14`) that is pushed by the HUD-element
  draw from the element's own `ui3dWindowId` byte (`hudElem+0x77`), which is a **GSC-visible
  field named `ui3dwindow`**.
* A GSC builtin `ui3dsetwindow(<id>,<x>,<y>,<w>,<h>)` positions the window inside the UI3D
  texture, and a material can sample it via the code image **`$ui3d`** plus the code constant
  `ui3dUVSetup<n>`.

So the mod does **not** need to hook a draw primitive to get the HUD into a texture. It needs
to (a) mark the elements it wants with a UI3D window, (b) put a quad in the world with a
`$ui3d` material. Hooking is only needed for the HUD parts that are **not** hudelems
(ownerdraw menus, crosshair, grenade indicator, chat).

| What | Where |
|---|---|
| Per-frame 2D HUD entry (`CG_Draw2D`-like) | **`0x655A50`** (reached through thunk `0x523EE0`) |
| Virtual→real coordinate mapper | **`0x5CD7B0`** `ScrPlace_ApplyRect` |
| `ScreenPlacement` array (per local client) | **`0xC78DA0`**, stride `0x78` |
| Material quad primitive | **`0x6D5DD0`** `R_AddCmdDrawStretchPic` |
| Text primitive (8 overloads) | `0x6D6280 / 0x6D6460 / 0x6D65C0 / 0x6D6720 / 0x6D6910 / 0x6D6B30 / 0x6D6D50 / 0x6D6FD0` |
| Rotated-quad primitive | **`0x6D73F0`** `R_AddCmdDrawQuadPic` |
| Command buffer | `*(GfxCmdBuf**)`**`0x3B370C0`** |
| Command list executor + UI3D filter | **`0x6EB6A0`** |
| Command handler table (29 entries) | **`0xB4AF28`** |
| UI3D window stack (the tag source) | **`0x3E4DE14`** `{int win[2]; int count;}` |
| UI3D render pass | **`0x6E26A0`** `RB_UI3D_Render` → `R_SetRenderTarget(.., 0x14)` |
| Coordinate space | **640 × 480 virtual**, letterboxed to 4:3 and centred |

---

## 1. The 2D HUD draw path

### 1.1 Entry

VERIFIED: `0x523EE0` is a one-instruction thunk `jmp 0x655A50`. Its only call site is
`0x5C3C5E`, inside the function starting at **`0x5C3420`** (the per-frame client draw
routine — it also computes `refdef` fov from `cg+0x8C110/0x8C114` and calls the scene
submit path). `0x655A50` therefore has exactly one live caller.

### 1.2 `0x655A50` reconstructed

Reconstructed from the Ghidra decompile with every dvar global resolved by an automated
`.text` scan that pairs `push $<name-string>` → `call <Dvar_Register*>` → next
`mov %eax,<global>` (§7). Names in **bold** are the binary's own dvar strings; function names
are my labels.

```c
void CG_Draw2D(int localClientNum)                                    /* 0x655A50 */
{
    cg_t *cg = *(cg_t**)0x2FF5354;
    FUN_00772540();                       /* 0x772540  death-screen fade (cg_deathScreenFadeOutTime) */
    if (cg->[0x0C] != 0) return;

    FUN_007714E0(0, localClientNum);      /* 0x7714E0  "layer 0" 2D pass (see below)      */
    if (cg->[0x8A8E4] || cg->[0xCCDF8])
        FUN_00772AA0();                   /* 0x772AA0  seven-segment counter ("%04d")     */

    if (!cg_draw2D->current.bool) {                       /* 0x2F67CC0 */
        if (cg_drawHUD && hud_drawHUD)  CG_DrawHudElems(localClientNum, 0);   /* 0x656880 */
        FUN_00771D20();                                   /* screen fade (cg_defaultFadeScreenColor) */
        return;
    }
    if (net_showprofile) return;                          /* 0x3023A54 */

    if (cg->connState == 5) FUN_00772A30();
    if (!FUN_004BF450(localClientNum)) {                  /* "is spectating/cinematic?" */
        FUN_00549BD0(lc); FUN_00486580(lc);
        FUN_004BF560(lc);                 /* friendly / player names (cg_drawFriendlyNames) */
        FUN_007724B0();
        ... three timed one-shot overlays (0x4ECF50, 0x4E30C0, 0x4650F0) ...
        FUN_00466170(cg);
        if (cg_debugDrawSafeAreas)  FUN_00771E50();       /* 0x2F67B54 -> safe-area debug boxes */
    }

    if (cg_drawHUD->current.bool && hud_drawHUD->current.bool) {      /* 0x2FF6714, 0x2F67BF4 */
        FUN_004FA510(lc);                 /* damage-direction icons  (cg_hudDamageIcon*) */
        FUN_004D30E0(lc);                 /* players-in-view / overhead names            */
        if (cg->connState < 9) {
            if (!cg_drawFriendlyFireCrosshair || !FUN_00771370())
                CG_DrawCrosshair(lc);     /* 0x4100F0 — see crosshair-draw-findings.md   */
            FUN_004FD660(lc);             /* grenade indicator/pointer (cg_hudGrenade*)  */
            FUN_00772F50();               /* voice menu                                   */
        }
        FUN_007714E0(1, lc);              /* "layer 1" 2D pass                            */
        FUN_007710B0();                   /* chat lines (cg_hudChatPosition, cg_chatTime) */
        CG_DrawHudElems(lc, 0);           /* 0x656880 — script HUD elements, pass 0       */
    }

    Menu_PaintAll(lc, &uiContext[lc]);    /* 0x552D80, uiContext stride 0x21B8 @0x2F67B3C */
    CG_DrawHudElems(lc, 1);               /* 0x656880 — script HUD elements, pass 1       */
    FUN_007714E0(2, lc);                  /* "layer 2" 2D pass                            */

    FUN_0060CEE0(lc, cg_hudSayPosition.x, cg_hudSayPosition.y + 0x18);   /* say/team-say  */
    if (!spectating && cg_drawSpectatorMessages) { FUN_00772820(lc); FUN_00772690(); }
    FUN_0068ABE0(lc);                     /* lagometer (cg_drawLagometer)                 */
    if (!r_reflectionProbeGenerate) { FUN_005BF0F0(lc); FUN_00651A30(); }  /* fps/debug    */
    if (!cg->[0xA9B38] && con_minicon)  FUN_00435A80(lc, 2, 4, 1.0f);
    FUN_005076D0(lc, 2, 300, 1.0f);       /* console / notify area                        */
    FUN_00771D20();                       /* full-screen fade                             */
    if (cl_paused && cg_drawpaused)  CG_DrawPausedHud(lc);   /* 0x4E31F0                  */
}
```

VERIFIED cross-checks that the dvar mapping is right:

* `0x2F67B54` decodes to `cg_debugDrawSafeAreas`, and the function it gates (`0x771E50`) is
  only called from there.
* `0x2FF68CC`/`0x2FF6844` decode to `cl_paused`/`cg_drawpaused`, matching the independently
  derived table in `crosshair-draw-findings.md`.
* `0x2562940` decodes to `ui_showList`, and `0x552D80` uses it to build a `"menu: %s"` debug
  string — i.e. `0x552D80` really is `Menu_PaintAll`.

### 1.3 The pieces that are *not* hud elements

| addr | what it draws | identifying dvars/strings (VERIFIED) |
|---|---|---|
| `0x772540` | death-screen fade | `cg_deathScreenFadeOutTime` |
| `0x772AA0` | seven-segment digit readout | `seven_segment`, `"%04d"` |
| `0x4BF560` | friendly / enemy name tags | `cg_drawFriendlyNames`, `friendlyNameFontSize`, `"Friend Name"` |
| `0x771E50` | safe-area debug rectangles | gated by `cg_debugDrawSafeAreas` |
| `0x4FA510` | damage-direction indicators | `cg_hudDamageIconWidth/Height/Offset/InScope`, `hit_direction` |
| `0x4D30E0` | players-in-view markers | `cg_playersInViewMinDot` |
| `0x4100F0` | crosshair | see companion doc |
| `0x4FD660` | grenade pointer / indicator | `cg_hudGrenadeIndicator*`, `cg_hudGrenadePointer*` |
| `0x772F50` | voice menu | `"voiceMenu"` |
| `0x7710B0` | chat backlog | `cg_hudChatPosition`, `cg_chatHeight`, `cg_chatTime` |
| `0x60CEE0` | say / team-say prompt | `"EXE_SAY"`, `"EXE_SAYTEAM"` |
| `0x772820` | spectator control hints | `cg_descriptiveText`, `"+attack"`, `"+melee"` … |
| `0x772690` | spectator follow string | `"spectator follow string"`, `zombiemode` |
| `0x68ABE0` | lagometer | `cg_drawLagometer`, `lagometer` |
| `0x5BF0F0` | fps / debug corner | `cg_drawFPS`, `cg_debugInfoCornerOffset` |
| `0x771D20` | full-screen fade | `cg_defaultFadeScreenColor` |
| `0x4E31F0` | paused-HUD redraw | menu names `"Compass" "Health" "weaponinfo" "offhandinfo" "stance" "sprintMeter" "objectiveinfo" "heatinfo"` |

`0x7714E0` (called three times, with `0`, `1`, `2`) is a layered 2D pass that itself calls
`ScrPlace_ApplyRect` and `CG_DrawStretchPic` (`0x540F70`) — INFERRED to be the
overlay/vision/blend layer draw. Not further chased.

### 1.4 Two element systems feed the HUD

**(a) UI menus** — `Menu_PaintAll` `0x552D80`. Items of type *ownerdraw* dispatch into
**`CG_OwnerDraw` `0x5ACCB0`** (VERIFIED: it opens with `if (!cg_drawHUD) return;
if (!hud_drawHUD) return;` and then a 46-entry jump table at `0x5AD56C`, plus explicit
`switch` cases up to `0x186`). The paused-HUD list in `0x4E31F0` names the menus the SP /
zombies HUD is built from: `Compass`, `Health`, `weaponinfo`, `offhandinfo`, `stance`,
`sprintMeter`, `objectiveinfo`, `heatinfo`.

**(b) Script HUD elements** — `0x656880` → `0x77B360` → **`0x77B160`**.
GSC builtins `newhudelem` (`0x6707C0`), `newclienthudelem` (`0x680180`),
`newteamhudelem` (`0x68B0B0`), `newscorehudelem` (`0x5D3BB0`) — VERIFIED from the builtin
name/handler table at `0xB76F3C`+.

---

## 2. The primitive everything bottoms out in

Three layers. Hooking layer B captures the entire HUD (and the entire UI).

### 2.1 Layer A — CG/UI wrappers (input is **virtual 640×480**)

```c
/* 0x540F70 — 67 call sites; the general 2D material quad */
void CG_DrawStretchPic(const ScreenPlacement *scrPlace,
                       float x, float y, float w, float h,      /* virtual 640x480 */
                       int horzAlign, int vertAlign,
                       float s0, float t0, float s1, float t1,
                       const float color[4], Material *material);
/* early-outs if color[3] == 0.0f;  -> ScrPlace_ApplyRect -> 0x49BC20 -> 0x6D5F30 -> 0x6D5DD0 */

/* 0x5F4A90 — 98 call sites; the general 2D text draw */
void CG_DrawText(const ScreenPlacement *scrPlace, const char *text, int maxChars,
                 Font *font, float x, float y, int horzAlign, int vertAlign,
                 float scale, const float color[4], int style);
/* xScale=yScale=0x6D30F0(font,scale); ScrPlace_ApplyRect; x,y rounded to int; -> 0x61C4D0 -> 0x6D65C0 */

/* 0x573ED0 (18 sites) rotated quad   -> 0x65DFA0 -> 0x6D73F0  */
/* 0x55E820 ( 7 sites) rotate-XY pic  -> 0x61C0D0 -> 0x6D5F90  */
/* 0x4DE340 / 0x694830  crosshair-only stretch / rotated pic (3 and 1 sites) */
```

All of the above VERIFIED from the decompiles; the `s/t` ordering in `CG_DrawStretchPic` is
INFERRED from the argument positions surviving unchanged down to `R_AddCmdDrawStretchPic`.

Complete list of the 38 functions that call `ScrPlace_ApplyRect` (i.e. every place virtual
coordinates enter the pipeline) — VERIFIED by a full `.text` call scan:

```
0x410950 0x41B7E0 0x432280 0x47F350 0x4856B0 0x497A30 0x498400 0x4A2F50 0x4DE340 0x4E6AE0
0x540F70 0x544F40 0x54FF50 0x55E820 0x573ED0 0x574420 0x59D9A0 0x5D9640 0x5F4A90 0x5F8D20
0x60FF50 0x639260 0x694830 0x76EB20 0x76FEF0 0x771370 0x7714E0 0x774B60 0x77E210 0x77E6B0
0x79D8A0 0x7A1290 0x8374A0 0x83CB80 0x83D510 0x83E010 0x842080 0x8463E0
```

### 2.2 Layer B — the render-command emitters (input is **real pixels**)

**`0x6D5DD0` is `R_AddCmdDrawStretchPic` — VERIFIED by name**: the function itself contains
the format strings
`"R_AddCmdDrawStretchPic: NOT DRAWING WITH MATERIAL \"%s\", because it has a fogable technique.\n"`
(`0xB57038`) and `"... because it uses the depth buffer. Set materialType to 2d.\n"`
(`0xB57098`), and those two strings are referenced from nowhere else in `.text`.

```c
void R_AddCmdDrawStretchPic(                       /* 0x6D5DD0, cdecl, 11 args */
        float x, float y,        /* real pixels, top-left */
        float z,                 /* depth/scale slot; 1.0f on the normal 2D path */
        float w, float h,        /* real pixels */
        float s0, float t0, float s1, float t1,
        const float color[4],    /* may be NULL -> 0xFFFFFFFF packed */
        Material *material);     /* NULL -> default material at 0x396A1C4 */
```

Emitted command (0x30 bytes, VERIFIED field-by-field from the decompile):

```
 +0x00  u16  size          = 0x30
 +0x02  u8   type          = 10  (RC_STRETCH_PIC)
 +0x03  u8   ui3dTag       = 0, or (0x80 | currentUI3DWindow)      <<<< THE HOOK POINT
 +0x04  ptr  Material*
 +0x08  f32  x
 +0x0C  f32  y
 +0x10  f32  z
 +0x14  f32  w
 +0x18  f32  h
 +0x1C  f32  s0
 +0x20  f32  t0
 +0x24  f32  s1
 +0x28  f32  t1
 +0x2C  u32  packed RGBA   (0xFFFFFFFF when color == NULL; packed by 0x5152D0)
```

`0x6D5F30` is a 10-arg wrapper that inserts `z = 1.0f` — this is what the CG layer actually
calls. `0x4D78B0` is a plain forwarder.

Full emitter table (VERIFIED: each writes its own `movb $<type>` into `cmd+2`):

| emitter | `movb` site | RC type | name |
|---|---|---|---|
| `0x6D5DD0` | `0x6D5E5E` | 10 | `RC_STRETCH_PIC` |
| `0x6D5F90` | `0x6D5FC1` | 12 | `RC_STRETCH_PIC_ROTATE_XY` |
| `0x6D6140` | `0x6D6171` | 13 | `RC_STRETCH_PIC_ROTATE_ST` |
| `0x6D6280` | `0x6D62E7` | 17 | `RC_DRAW_TEXT_2D` |
| **`0x6D6460`** | `0x6D64B8` | 17 | `RC_DRAW_TEXT_2D` (33 call sites) |
| `0x6D65C0` | `0x6D6618` | 17 | `RC_DRAW_TEXT_2D` (the one `CG_DrawText` uses) |
| `0x6D6720` | `0x6D6779` | 17 | `RC_DRAW_TEXT_2D` |
| `0x6D6910` | `0x6D6969` | 17 | `RC_DRAW_TEXT_2D` |
| `0x6D6B30` | `0x6D6B89` | 17 | `RC_DRAW_TEXT_2D` |
| `0x6D6D50` | `0x6D6DA9` | 17 | `RC_DRAW_TEXT_2D` |
| `0x6D6FD0` | `0x6D7019` | 17 | `RC_DRAW_TEXT_2D` |
| `0x6D73F0` | `0x6D7420` | 15 | `RC_DRAW_QUAD_PIC` (`0x6D74E0` = wrapper, z = 1.0f) |
| `0x6D7500` | `0x6D752E` | 1 | `RC_SET_CUSTOM_CONSTANT` |
| `0x6D75A0` | `0x6D75CE` | 7 | `RC_SET_SCISSOR` |
| `0x6D7640` | `0x6D7682` | 9 | `RC_PC_COPY_IMAGE_GEN_MIP` |
| `0x6D7BF0` | `0x6D7C1E` | 5 | `RC_CLEAR_SCREEN` |
| `0x6D7CA0` | `0x6D7CD0` | 27 | `RC_PROJECTION_SET` |
| `0x6D7D40` | `0x6D7D71` | 28 | `RC_DRAW_FRAMED` |

`R_AddCmdDrawQuadPic` (`0x6D73F0`) takes `(const float verts[8], float z, const float color[4],
Material *material)` — four screen-space corners, produced by `0x65DFA0` from
`(scrPlace, x, y, z, w, h, angleDegrees, color, material)`.

The RC enum names come from the binary's own table at **`0xBA58E0`** (29 `const char*`,
index 0 = `RC_END_OF_LIST`). VERIFIED.

Command buffer: `*(struct { char *base; int used; int ?; void *lastCmd; int capacity; } **)`
**`0x3B370C0`**, re-pointed each frame by `0x6D5AC0` to
`frontEndData->[0x16CC34]` (`frontEndData` = `*(void**)0x3B3708C`). An emitter drops the
command silently when fewer than `size + 0x2000` bytes remain. VERIFIED.

### 2.3 Layer C — execution

```c
/* 0x6EB6A0   RB_ExecuteCmdList(int *wantWindow /*EAX*/, GfxCmd *cmd /*ECX*/) */
int want = wantWindow ? *wantWindow : -1;      /* -1 == main framebuffer pass */
while (cmd->type != 0) {                       /* RC_END_OF_LIST */
    signed char tag = cmd->ui3dTag;            /* +3 */
    bool run;
    if (tag >= 0)  run = (want == -1);         /* untagged: main pass only     */
    else           run = (want >= 0) && ((tag & 0x7F) == want);  /* UI3D pass  */
    if (run)  ((void(*)(GfxCmd**))(*(void**)(0xB4AF28 + cmd->type*4)))(&cmd);
    else      cmd = (GfxCmd*)((char*)cmd + cmd->size);           /* +0, u16    */
}
```

VERIFIED instruction-for-instruction at `0x6EB6F9`–`0x6EB759`. This is a **strict partition**:
a tagged command never reaches the framebuffer, and an untagged command never reaches UI3D.

Handler table at **`0xB4AF28`**, 29 entries, VERIFIED (entry 0 is NULL, entry 29 is 0):

```
 1 SET_CUSTOM_CONSTANT 0x6E7FD0   2 SET_MATERIAL_COLOR 0x6E8020   3/4 SAVE_SCREEN[_SECTION] 0x6E6E40
 5 CLEAR_SCREEN 0x6E6D50          6 SET_VIEWPORT 0x6E8080         7 SET_SCISSOR 0x6E80D0
 8 RESOLVE_COMPOSITE 0x6E8150     9 PC_COPY_IMAGE_GEN_MIP 0x6E8220
10 STRETCH_PIC 0x6E5260          11 STRETCH_PIC_FLIP_ST 0x6E52F0  12 STRETCH_PIC_ROTATE_XY 0x6E5350
13 STRETCH_PIC_ROTATE_ST 0x6E56D0 14 STRETCH_RAW 0x6E62A0         15 DRAW_QUAD_PIC 0x6E5A00
16 DRAW_FULL_SCREEN_COLORED_QUAD 0x6E6140                        17 DRAW_TEXT_2D 0x6EAE20
18 DRAW_TEXT_3D 0x6EAF00        19/20 BLEND_SAVED_SCREEN_* 0x6E6E40
21 DRAW_POINTS 0x6E7450         22 DRAW_LINES 0x6E7D50           23 DRAW_TRIANGLES 0x6E7F70
24 DRAW_QUADLIST_2D 0x6E5C10    25 DRAW_EMBLEM_LAYER 0x6E5E70    26 STRETCH_COMPOSITE 0x6E51C0
27 PROJECTION_SET 0x6EAF40      28 DRAW_FRAMED 0x6E6380
```

Call sites of `0x6EB6A0` (VERIFIED): `0x6D03C8`, `0x6D08BA`, `0x6D20D6`, `0x6EBA14`,
`0x740CEF` all pass `xor %eax,%eax` (main pass); only `0x6E294B`, inside `RB_UI3D_Render`
`0x6E26A0`, passes a pointer to a window index.

---

## 3. Coordinate space — VERIFIED

### 3.1 Virtual canvas is **640 × 480**

Read out of `.rdata` at the exact addresses the code references:

| VA | value | used by |
|---|---|---|
| `0x9E5AE4` | `640.0f` | `0x4B8880`, `0x6E21B0`, `0x7A1C90`, `0x552D80` |
| `0xA0E290` | `480.0f` | same |
| `0xA13D5C` | `0.0015625f` = 1/640 | `0x4B8880`, `0x6E21B0` |
| `0x9F09C0` | `0.00208333f` = 1/480 | same |
| `0x9C3FA4` | `1.3333334f` = 4/3 | `0x4B8880` |

(Independently corroborated: `crosshair-draw-findings.md` already found `-320`/`-240`
half-screen constants and a `240.0f` half-height in the crosshair spread maths.)

### 3.2 `ScreenPlacement` (0x78 bytes = 30 floats)

`ScrPlace_ApplyRect` at **`0x5CD7B0`**:

```c
void ScrPlace_ApplyRect(const ScreenPlacement *p,
                        float *x, float *y, float *w, float *h,
                        int horzAlign, int vertAlign);
```

Eleven horizontal and eleven vertical alignment modes, each a `case` in one of two jump
tables (horizontal table at `0x5CDA44`). The default (mode 0) is

```
*x = *x * p[0] + p[0x1C];      *w *= p[0];
*y = *y * p[1] + p[0x1D];      *h *= p[1];
```

VERIFIED float-index meanings, from the two places that *build* the struct
(`0x4B8880` for the view placement, `0x6E21B0` for UI3D windows):

| index (float) | byte off | meaning |
|---|---|---|
| `[0] [1]` | `0x00 0x04` | `scaleVirtualToReal`  (`safeW/640`, `realH/480`) |
| `[2] [3]` | `0x08 0x0C` | `scaleVirtualToFull`  (`realW/640`, `realH/480`) |
| `[4] [5]` | `0x10 0x14` | `scaleRealToVirtual`  (`640/safeW`, `480/realH`) |
| `[6]..[9]` | `0x18..0x24` | viewable min rect (from `0x7A1C90`) |
| `[10]..[13]` | `0x28..0x34` | viewable max rect (from `0x7A1C90`) |
| `[0x0E] [0x0F]` | `0x38 0x3C` | real viewport origin (0,0) |
| `[0x10] [0x11]` | `0x40 0x44` | real viewport size (`realW`, `realH`) |
| `[0x14]..[0x1B]` | `0x50..0x6C` | the alignment anchors used by modes 1,3,7,8,9,10 |
| `[0x1C] [0x1D]` | `0x70 0x74` | **origin offset** — `(realW - safeW) * 0.5`, `0` |

### 3.3 Where the placement is built — the "safe area"

```c
/* 0x4B8880, reached through the wrapper 0x44F440 (its only caller) */
void ScrPlace_SetupViewport(ScreenPlacement *p, ?, ?, float realW, float realH)
{
    p[0x0E] = 0;  p[0x0F] = 0;  p[0x10] = realW;  p[0x11] = realH;

    float safeW = (realH / pixelAspect /*0x2EE7530*/) * 1.3333334f;   /* a 4:3 box */
    if (realW < safeW) safeW = realW;

    ScrPlace_CalcViewable(..., &p[6],  &p[8]);      /* 0x7A1C90 */
    ScrPlace_CalcViewable(..., &p[10], &p[12]);     /* 0x7A1C90 */

    p[0]  = safeW / 640.0f;      p[1] = realH / 480.0f;
    p[2]  = realW / 640.0f;      p[3] = realH / 480.0f;
    p[4]  = 640.0f / safeW;      p[5] = 480.0f / realH;
    p[0x1C] = (realW - safeW) * 0.5f;
    p[0x1D] = 0.0f;
}
```

**So on a 16:9 display the whole 640×480 virtual HUD is squeezed into a centred 4:3 box, and
`p[0x1C]` is the left pillar-box width.** That is the entire "safe area" for the *default*
alignment mode. VERIFIED.

`0x7A1C90` additionally insets a rect by a fraction of the real screen size, using the screen
dimensions at `0x2EE7514` / `0x2EE7518` (ints read as unsigned then converted to float). This
produces the `viewableMin/Max` sub-rects used by alignment modes 2 and 4. `0x4B8880` passes
`1.0f, 1.0f` as the two fractions in this build, i.e. **no additional safe-area inset**.
The fraction path itself is VERIFIED; that it is always 1.0 is INFERRED (only one caller was
found).

Placement array: **`0xC78DA0 + localClientNum * 0x78`** — VERIFIED, it is what
`CG_DrawHudElem`, `Menu_PaintAll` and `CG_DrawGrenadeIndicator` all index. 244 references in
`.text`.

### 3.4 Safe-area / HUD-placement dvars

Mapped by the automated register-scan (§7). **`current` is at dvar+0x18** (`.x`/`.y` of a
vec2 at `+0x18`/`+0x1C`).

| dvar | register site | type | dvar-pointer global | read anywhere? |
|---|---|---|---|---|
| `cg_draw2D` | `0x4A3B58` | Bool | `0x2F67CC0` | yes — `0x655A91`, `0x4075DD`, `0x6C2BD5`, `0x6C58F0`, `0x77107F` |
| `cg_drawHUD` | `0x4A3B79` | Bool | `0x2FF6714` | yes — `0x655AA0`, `0x655B9B`, `0x4CBF70`, `0x5ACCB0` |
| `hud_drawHUD` | `0x4A65E2` | Bool | `0x2F67BF4` | yes — same sites + `0x490A80` |
| `cg_drawHealth` | `0x4A3BF8` | Bool | `0x2F67B94` | yes — `0x780460`, `0x7807C0` |
| `cg_debugDrawSafeAreas` | `0x4A45E4` | Bool | `0x2F67B54` | yes — `0x655A50` |
| `cg_hudChatPosition` | `0x4A4417` | Vec2 (`0x4372D0`) | `0x2F67C54` | yes — `0x7710CB` |
| `cg_hudSayPosition` | `0x4A444E` | Vec2 (`0x4372D0`) | `0x2F67B38` | yes — `0x655A50` |
| `cg_hudTopSemimajor` | `0x4A4525` | Float | `0x2FF5308` | **no — dead** |
| `cg_hudTopSemiminor` | `0x4A4576` | Float | `0x2FF67D0` | **no — dead** |
| `cg_hudBottomSemimajor` | `0x4A455E` | Float | `0x2F67BFC` | **no — dead** |
| `cg_hudBottomSemiminor` | `0x4A45AF` | Float | `0x2F67C18` | **no — dead** |
| `r_ui3d_debug_display` | `0x6CE686` | Bool | `0x3B1FDF8` | |
| `r_ui3d_use_debug_values` | `0x6CE6A5` | (`0x651910`) | `0x3B1FDAC` | |
| `r_ui3d_x` / `_y` / `_w` / `_h` | `0x6CE6D8/706/734/762` | Float | `0x3B20004` / `0x3B1FCE4` / `0x3B1FDF4` / `0x3B1FD64` | |
| `ui3d_hudelem_maxscale` | `0x54A375` | Float | `0xC19234` | `0x77AEF0` |
| `ui3d_hudelem_minscale` | `0x54A3AD` | Float | `0xC1923C` | `0x77AEF0` |
| `ui3d_hudelem_maxheight` | `0x54A3E5` | Float | `0xC191DC` | `0x77AEF0` |
| `ui3d_hudelem_minheight` | `0x54A41D` | Float | `0xC19238` | `0x77AEF0` |

VERIFIED. **The four elliptical `cg_hudTop*/cg_hudBottom*` dvars are registered but never
read in `.text`** — do not try to move the HUD with them.

There is no `cg_viewsize` and no `safeArea_horizontal`/`safeArea_vertical` string in this
binary (VERIFIED by string search). `center_safearea` exists only as a `.rdata` string at
`0x9C0354` and is not a registered dvar.

### 3.5 One extra global transform to be aware of

The tail of `ScrPlace_ApplyRect` applies, when `*(int*)0x396EE10 != 0`:

```c
x *= s; y *= s; w *= s; h *= s;                 /* s = (float)*(int*)0x396EE14 */
x -= (float)(unsigned)*(int*)0x2EE7514 * *(float*)0x396EE18;
y -= (float)(unsigned)*(int*)0x2EE7518 * *(float*)0x396EE1C;
```

The same flag is read in 14 places including `0x62D4E0` (`return flag ? scale : 1.0f`), which
`R_AddCmdDrawQuadPic`'s vertex builder divides by. INFERRED: this is the tiled
high-resolution-screenshot path; it is inert in normal play (`0x396EE10 == 0`). Worth knowing
because a VR mod that renders the frame more than once must not trip it.

---

## 4. POINTS / ammo / health

### 4.1 Points (zombies) — a **script HUD element**

VERIFIED: GSC builtin **`newscorehudelem`** exists, handler `0x5D3BB0`, from the builtin
name/handler table (name string `0xA1FFE4`, table entry `0xB76F6C`). Also present:
`newhudelem` `0x6707C0`, `newclienthudelem` `0x680180`, `newteamhudelem` `0x68B0B0`.

INFERRED (strong): the zombies points readout is created by script through one of these and
is therefore drawn by `CG_DrawHudElem` `0x77B160`. Supporting evidence: there is no
points/score draw anywhere in the `CG_Draw2D` tree, no score-related ownerdraw case, and no
"points"/"score" HUD string in the client code — the only `*_points` strings in the binary
are per-map *stat* names (`zombie_moon_points`, `zombie_temple_points`, …). Not proven,
because the script itself lives in the fastfiles, not the exe.

**Consequence: points is exactly the case the UI3D path handles for free** (§5).

### 4.2 Health — C code behind a menu ownerdraw

VERIFIED: `CG_OwnerDraw` `0x5ACCB0` case `0x4F` → `0x780460`, case `0x62` → `0x7807C0`. Both
read `cg_drawHealth` (`0x2F67B94`), `hud_fade_healthbar`, and `0x7807C0` also reads
`hud_health_pulserate_critical/injured` and `hud_health_startpulse_critical/injured`. These
are reached from the `"Health"` menu, painted by `Menu_PaintAll` `0x552D80`.

### 4.3 Ammo — also a menu ownerdraw

INFERRED: the ammo counter is the `"weaponinfo"` menu (named in the paused-HUD list in
`0x4E31F0`), drawn through `CG_OwnerDraw`. The weapon-file fields `ammoCounter`,
`ammoCounterClip`, `ammoCounterIcon`, `ammoCounterIconRatio`, `ammoCounterHide` exist
(`ammoCounterHide` is also registered as a Bool dvar at `0x4DF5FC`, pointer `0xBE3B7C`, read
only in `0x4DF5F0`). I did not isolate the specific ownerdraw case number for ammo — the 46
jump-table cases at `0x5AD56C` and the explicit `switch` cases are unlabelled integers and
matching them to menu files requires the fastfiles.

**This is the honest weak point of this document:** health is pinned exactly; ammo and points
are placed in the right *system* but the exact per-element identifiers were not proven.

---

## 5. Render target — the important answer

### 5.1 The HUD is drawn to the framebuffer *by default*, but the engine can retarget it per element

VERIFIED chain:

1. Every `R_AddCmd*` writes `cmd+3` as:
   ```c
   if (ui3dStack.count == 0) { tag = 0; }
   else { int w = ui3dStack.win[count-1]; tag = (w < 0) ? 0 : (byte)(w | 0x80); }
   ```
   reading `count` from **`0x3E4DE1C`** and the entry from `0x3E4DE10 + count*4`
   (= `&win[count-1]`, since the object base is `0x3E4DE14`).
   It also increments a per-window draw counter at `0x3E58EC8 + w*0x9C`.

2. The stack object is `struct { int win[2]; int count; }` at **`0x3E4DE14`**, with
   accessors — VERIFIED, each is a handful of instructions:
   * `0x6E1CB0` `R_GetUI3DWindowStack()` → `0x3E4DE14`
   * `0x6E1CC0` `push(stack, window)` — **max depth 2**, silently ignores overflow
   * `0x6E1CE0` `pop(stack)`
   * `0x6E1CF0` `top(stack)` → `-1` if empty
   * `0x6E1F00` `reset()` — zeroes `count` and all six draw counters

3. `0x6EB6A0` partitions execution by that tag (§2.3).

4. **`RB_UI3D_Render` `0x6E26A0`** does
   `R_SetRenderTarget(*(void**)0xB47880, *(void**)0xB47884, 0x14)` and then, for each active
   window, sets viewport/scissor from the window's integer rect and calls
   `0x6EB6A0(&windowIndex, cmds)`. Afterwards it runs `0x73A450(blurRadius, 0x14)` and
   `0x73A450(blurRadius, 0x15)`.
   `0x14` = `R_RENDERTARGET_UI3D`, `0x15` = `R_RENDERTARGET_UI3D_PING_PONG`. VERIFIED against
   the `R_RENDERTARGET_*` string block, anchored by the project's already-confirmed
   `R_RENDERTARGET_8BIT_SWAPCHAIN_BACKBUFFER == 0x12`.

### 5.2 The UI3D window table

`R_SetUI3DWindow(unsigned window, float x, float y, float w, float h)` — **`0x6E21B0`**,
normalized `0..1`, clamped. Writes `0x3E58E30 + window*0x9C`:

| byte offset in entry | contents |
|---|---|
| `+0x00 .. +0x0F` | int viewport `x, y, w, h` in real pixels (`screenW*x`, …) |
| `+0x10 .. +0x87` | a full **`ScreenPlacement`** (the same 0x78-byte struct as `0xC78DA0`) |
| `+0x88 .. +0x97` | the normalized `x, y, w, h` you passed in |
| `+0x98` | draw counter, incremented by every emitter, zeroed by `0x6E1F00` |

`R_GetUI3DWindowPlacement(i)` = **`0x6E1D10`** → `0x3E58E40 + i*0x9C`. Six windows
(`window < 6` guard). VERIFIED.

Screen size for the conversion comes from `0x4643FA6` / `0x4643FA8` (u16 width/height).

### 5.3 It is already wired to script and to materials

* **GSC builtin `ui3dsetwindow`** — handler **`0x787750`**, table entry `0xB724AC`.
  VERIFIED, including its own usage string
  `"USAGE: ui3dsetwindow(<window id>, <x>, <y>, <width>, <height>)"`. It reads 5 script
  args and calls `R_SetUI3DWindow`.
* **GSC hudelem field `ui3dwindow`** — VERIFIED from the engine's own hudelem field table at
  `0xA53268`, stride 32, `{const char *name; int offset; int type; …; getter; setter}`:

  | GSC field | hudElem offset | type |
  |---|---|---|
  | `x` `y` `z` | `0x00` `0x04` `0x08` | float |
  | `fontscale` | `0x0C` | float |
  | `color` / `alpha` | `0x18` | vec / float |
  | `sort` | `0x40` | float |
  | `glowcolor` / `glowalpha` | `0x44` | |
  | `label` | `0x52` | string |
  | `foreground`, `hidewhendead`, `hidewheninkillcam`, `hidewhenindemo`, `overrridewhenindemo`, `hidewhileremotecontrolling`, `hidewheninmenu`, `fadewhentargeted`, `fontstyle3d`, `font3duseglowcolor` | `0x6C` | flag bits |
  | `font` | `0x6F` | byte |
  | `alignx` / `aligny` | `0x70` | byte |
  | `horzalign` / `vertalign` | `0x71` | byte |
  | **`ui3dwindow`** | **`0x77`** | byte (getter `0x7DE280`, setter `0x7DE2A0`) |

  (The table is immediately followed by the hudelem *method* table: `settext`, `setshader`,
  `setvalue`, `settimer`, `fadeovertime`, `moveovertime`, `destroy`, …)

* **`CG_DrawHudElem` `0x77B160` honours it** — VERIFIED, this is the whole prologue:

  ```c
  /* EAX = hudElem_t*, ESI = localClientNum */
  if (elem->type /*+0x6E*/ == 0) return;
  int win = (signed char)elem->ui3dWindowId;              /* +0x77 */
  ScreenPlacement *p = (win < 0) ? (ScreenPlacement*)(0xC78DA0 + localClientNum*0x78)
                                 : R_GetUI3DWindowPlacement(win);   /* 0x6E1D10 */
  void *stk = R_GetUI3DWindowStack();                     /* 0x6E1CB0 */
  UI3DStack_Push(stk, win);                               /* 0x6E1CC0 */
      ... type-specific draw (0x77A3A0 waypoint / 0x77AEF0 / 0x779F60 / 0x779C10 / 0x779980 / 0x779CA0) ...
  UI3DStack_Pop(stk);                                     /* 0x6E1CE0 */
  ```

  Note `elem->ui3dWindowId` is read as a **signed** byte: `-1` (`0xFF`) means "normal HUD".
  Element type 4 (`0x77AEF0`) is the one that consumes `ui3d_hudelem_min/maxscale` and
  `ui3d_hudelem_min/maxheight`.

* **Menus honour it too** — `Menu_Paint` `0x40F380` does the same
  `R_GetUI3DWindowPlacement` / push / pop dance (call sites `0x40F3AB`, `0x40F3CF`,
  `0x40F3DE`, `0x40F424`, `0x40F485`, `0x40F7A1`). The `.menu` keyword is `ui3dWindow`
  (field table entry at `0xA5D380`, offset `0x77`) and there is a menu-item property
  `ui3dWindowId` in the item keyword tables at `0xA5DFD0` / `0xA5E2E0`. VERIFIED.
  **So an entire `.menu` — e.g. `"weaponinfo"` — can be sent to a UI3D window with one field.**

* **Materials can sample it** — `$ui3d` is a **code image** (name table at `0xBA5590`,
  neighbours `$ui3d_ping_pong`, `$8bit_swapchain`, `$savedscreen`, …). The shader side has a
  code sampler `ui3dSampler` (`0xB57DE0`, installed at `0x6EC2C9`) and six code constants
  `ui3dUVSetup0..5` (table at `0xB49E00`, constant ids `0xAC`..`0xB1`) which supply each
  window's UV sub-rect. VERIFIED.

**Bottom line for question 5: the HUD is drawn into the same command list as everything else
2D, and by default into the framebuffer — but the engine already has a first-class,
per-element mechanism for redirecting selected 2D draws into a dedicated render target
(`R_RENDERTARGET_UI3D`) that is exposed to materials as `$ui3d`. That is by a wide margin the
cheapest and most robust route for world-space HUD in VR.**

---

## 6. Recommendation for the mod

### 6a. Suppressing selected HUD elements

Cheapest to most surgical:

1. **Everything at once** — clear either dvar's `current` byte:
   `*(unsigned char*)(*(void**)0x2FF6714 + 0x18) = 0;`  (`cg_drawHUD`)
   or `*(unsigned char*)(*(void**)0x2F67BF4 + 0x18) = 0;` (`hud_drawHUD`).
   VERIFIED to gate the whole HUD block in `CG_Draw2D` **and** the top of `CG_OwnerDraw`
   (so it also kills the menu-driven ammo/health/compass). It does **not** kill
   `Menu_PaintAll`'s non-HUD menus, hudelems drawn in pass 1, chat, or the fade.
   `cg_draw2D` (`0x2F67CC0`) is a bigger hammer that still leaves hudelems.

2. **Individual C-drawn overlays** — `ret`-patch or detour the specific function from the
   table in §1.3. They are all `cdecl` with `localClientNum` as the first stack argument.
   Crosshair: use the `cg_drawCrosshair` route already documented in the companion doc.

3. **Individual hudelems** — from GSC, `elem.alpha = 0` or `elem destroy()`. From native
   code, a detour on `CG_DrawHudElem` `0x77B160` can filter by `elem->type` (`+0x6E`),
   `elem->label` (`+0x52`) or the flags word (`+0x6C`) and return early. Note the
   non-standard convention: **`EAX` = `hudElem_t*`, `ESI` = `localClientNum`**, no stack args.

4. **A whole menu** (ammo / health / compass) — the per-menu `ui3dWindow` field means you can
   *move* rather than suppress; see below.

### 6b. Capturing the HUD into a texture — **the recommended route**

**Use the engine's UI3D path. Do not hook a draw primitive.**

Minimal native implementation, no rendering code of your own:

```c
typedef void  (__cdecl *R_SetUI3DWindow_t)(unsigned win, float x, float y, float w, float h);
typedef void *(__cdecl *R_GetUI3DWindowStack_t)(void);
typedef void  (__cdecl *UI3DStack_Push_t)(void *stack, int win);
typedef void  (__cdecl *UI3DStack_Pop_t)(void *stack);
typedef void *(__cdecl *R_GetUI3DWindowPlacement_t)(int win);

#define R_SetUI3DWindow          ((R_SetUI3DWindow_t)         0x6E21B0)
#define R_GetUI3DWindowStack     ((R_GetUI3DWindowStack_t)    0x6E1CB0)
#define UI3DStack_Push           ((UI3DStack_Push_t)          0x6E1CC0)
#define UI3DStack_Pop            ((UI3DStack_Pop_t)           0x6E1CE0)
#define R_GetUI3DWindowPlacement ((R_GetUI3DWindowPlacement_t)0x6E1D10)
```

Three ways to use it, in increasing order of effort:

* **(i) Script only — zero native code, and it covers *points*.**
  Once per level: `ui3dsetwindow(0, 0, 0, 1, 1);`
  Then on each element you want moved: `elem.ui3dwindow = 0;`
  Everything else is unchanged. This is almost certainly all that is needed for the user's
  immediate complaint, because the zombies points readout is a script hudelem (§4.1).
  Cost: it requires shipping a modified `.gsc`, and it needs a world surface whose material
  samples `$ui3d`.

* **(ii) Native, per-element, no script changes.** Detour `CG_DrawHudElem` `0x77B160`
  (preserving `EAX`/`ESI`), and before the original runs, overwrite `elem->ui3dWindowId`
  (`+0x77`) with your window index for the elements you want relocated. The engine then does
  the placement swap and the push/pop for you. This is the single cleanest hook in the whole
  system: **one function, one byte.**

* **(iii) Native, whole-HUD.** Wrap the *call sites* inside `CG_Draw2D` rather than the
  primitives: detour `0x655A50`, and around the region you want captured do
  `UI3DStack_Push(R_GetUI3DWindowStack(), win)` … `UI3DStack_Pop(...)`. Every emitter between
  those two points tags its commands automatically, so the crosshair, grenade indicator,
  chat, `Menu_PaintAll` output — all of it — lands in the UI3D texture instead of the
  framebuffer. **Caveat (VERIFIED):** the stack is only 2 deep and `CG_DrawHudElem` /
  `Menu_Paint` push their own entry inside, so a wrapping push leaves exactly one slot; a
  third nested push is silently dropped. And elements with `ui3dWindowId == -1` will push
  `-1`, which makes the emitters treat the command as **untagged** — i.e. hudelems that
  haven't opted in will still escape to the framebuffer even inside your wrapper. If you go
  this route, combine it with (ii).

  Also note `Menu_PaintAll` and `CG_DrawHudElems` are called *outside* the
  `cg_drawHUD && hud_drawHUD` block, so a wrapper placed only inside that block will miss
  the ammo/health menus and the pass-1 hudelems.

**Coordinate consequence.** When an element renders into UI3D window `w`, the placement it is
laid out against is `R_GetUI3DWindowPlacement(w)` — a `ScreenPlacement` built by
`R_SetUI3DWindow` from the *window's* pixel rect, with `p[0x1C] = p[0x1D] = 0`. It has **no
4:3 letterbox and no origin offset**, unlike the view placement. So the same virtual
`(0..640, 0..480)` coordinates fill the window rect exactly and get a different aspect than
they had on screen. Either size the window to 4:3 (`ui3dsetwindow(0, 0, 0, 0.75, 1.0)` on a
square target, etc.) or re-lay-out the elements.

### 6c. The fallback, if UI3D turns out not to work at runtime

Hook **`R_AddCmdDrawStretchPic` `0x6D5DD0`** plus the eight `RC_DRAW_TEXT_2D` emitters and
`R_AddCmdDrawQuadPic` `0x6D73F0`. Between them they carry 100 % of the HUD. They are all
plain `cdecl`, they receive **real pixel** coordinates, and they are the last point before
the command is committed. Either rewrite the coordinates in place or set `cmd+3` yourself.

An even smaller hook exists: replace entries in the **handler table at `0xB4AF28`** (29
function pointers, writable `.data`). Swapping `[10]`, `[15]` and `[17]` gives you a
back-end-side intercept of every stretch-pic, quad-pic and text draw, with the full command
struct in hand — and unlike a code detour it needs no trampoline. This is the best
"capture everything, including what UI3D can't reach" option.

### 6d. Explicitly *not* recommended

* `cg_hudTopSemimajor` / `cg_hudTopSemiminor` / `cg_hudBottomSemimajor` /
  `cg_hudBottomSemiminor` — **registered but never read** (VERIFIED). They will do nothing.
* Rewriting the `ScreenPlacement` at `0xC78DA0` to shrink the HUD. It "works" but it also
  moves the crosshair, the compass, the menus and the console, and `0x44F440`/`0x4B8880`
  rebuild it whenever the viewport changes.

---

## 7. Method note — how the dvar→global map was produced

Same hazard as documented in `crosshair-draw-findings.md`: MSVC schedules the
`mov %eax,<global>` that stores a `Dvar_Register*` result **after the argument pushes for the
next registration call**. The scan used here therefore starts at the `call` (not at the name
push) and takes the *first* `mov %eax,<abs32>` that follows it, which lands on the correct
global. It reproduced `cl_paused → 0x2FF68CC` and `cg_drawpaused → 0x2FF6844` from the
companion doc, and `ui_showList → 0x2562940` cross-checks against `Menu_PaintAll`'s own use.
2892 globals were mapped this way.

---

## 8. VERIFIED vs INFERRED — explicit list

### VERIFIED (re-derived from the binary in this pass)

* `0x523EE0` is `jmp 0x655A50`; its only caller is `0x5C3C5E` inside `0x5C3420`.
* `0x6D5DD0` is `R_AddCmdDrawStretchPic`, by its own two format strings at `0xB57038` /
  `0xB57098`, which are referenced nowhere else.
* The RC command enum (29 names) from the table at `0xBA58E0`.
* Every emitter→command-type pairing in §2.2, from the `movb $<type>,cmd+2` in each body.
* The 0x30-byte `RC_STRETCH_PIC` layout and the `RC_DRAW_QUAD_PIC` layout.
* The command-buffer pointer `0x3B370C0` and its re-point in `0x6D5AC0` from
  `frontEndData(0x3B3708C)+0x16CC34`.
* The executor `0x6EB6A0` and its UI3D tag filter, instruction by instruction.
* The handler table at `0xB4AF28`: 29 entries, `[0] == NULL`, `[29] == 0`.
* `RB_UI3D_Render 0x6E26A0` calls `R_SetRenderTarget(..., 0x14)`, and post-processes `0x14`
  and `0x15`.
* The UI3D window stack at `0x3E4DE14` and its four accessors (`0x6E1CB0/C0/E0/F0`), the
  reset at `0x6E1F00`, and the fact that every emitter reads it.
* `R_SetUI3DWindow 0x6E21B0` and the `0x9C`-stride window table at `0x3E58E30`;
  `R_GetUI3DWindowPlacement 0x6E1D10` returns `+0x10` into it.
* `CG_DrawHudElem 0x77B160`'s placement swap and push/pop on `elem+0x77`.
* The hudelem GSC field table at `0xA53268` (27 rows, stride 32) including
  `ui3dwindow → +0x77`, and the method table that follows it.
* GSC builtins `ui3dsetwindow → 0x787750`, `newhudelem → 0x6707C0`,
  `newclienthudelem → 0x680180`, `newteamhudelem → 0x68B0B0`,
  `newscorehudelem → 0x5D3BB0`, from the table at `0xB76F3C`+.
* `$ui3d` in the code-image name table at `0xBA5590`; `ui3dSampler`; `ui3dUVSetup0..5` in the
  code-constant table at `0xB49E00`.
* `ScrPlace_ApplyRect 0x5CD7B0` body and its two 11-case jump tables.
* `ScrPlace_SetupViewport 0x4B8880`, the 4:3 clamp, and every index it writes.
* The virtual-screen constants 640 / 480 / 1/640 / 1/480 / 4/3 read out of `.rdata`.
* The 38 functions that call `ScrPlace_ApplyRect`.
* `CG_DrawStretchPic 0x540F70` and `CG_DrawText 0x5F4A90` bodies and their chains down to
  `0x6D5DD0` / `0x6D65C0`.
* `CG_OwnerDraw 0x5ACCB0`'s `cg_drawHUD`/`hud_drawHUD` guard, its 46-entry table at
  `0x5AD56C`, and health at cases `0x4F`→`0x780460`, `0x62`→`0x7807C0`.
* Every dvar row in §3.4, and the fact that the four `cg_hud*Semi*` globals are never read.

### INFERRED (reasoned, not proven)

* **All function names** except `R_AddCmdDrawStretchPic` (proven by its own string) and the
  four names already published in `address-map.md` (`RB_UI3D_Render`, `R_SetUI3DWindow`,
  `R_SetRenderTarget`, `R_SetRenderTargetSize`). `CG_Draw2D`, `ScrPlace_ApplyRect`,
  `CG_DrawHudElem`, `Menu_PaintAll`, `CG_OwnerDraw`, `R_AddCmdDrawQuadPic` etc. are my labels.
* The `s0/t0/s1/t1` argument ordering of `CG_DrawStretchPic` (positional inference).
* `0x2EE7514`/`0x2EE7518` are the real screen width/height; `0x2EE7530` is the pixel aspect.
* The `0x396EE10` block being the tiled-screenshot transform (inert in play).
* `ScrPlace_SetupViewport` always being called with safe-area fractions of 1.0 — only one
  caller (`0x44F440`) was found, and I did not chase *its* callers.
* Ammo being the `"weaponinfo"` menu, and its ownerdraw case number.
* Zombies points being a script hudelem (see §4.1 for the supporting negatives).
* `0x7714E0` being an overlay/blend layer pass.
* The `ScreenPlacement` index names in §3.2 beyond `[0..5]`, `[0x0E..0x11]` and `[0x1C/0x1D]`.

### NOT DONE

* **No runtime verification of anything in this document.**
* I did not confirm that any shipped material in the zombies fastfiles actually samples
  `$ui3d`, nor that a suitable world surface exists — that is fastfile content, not exe
  content. If no such material exists, one has to be added (or an existing in-world screen
  material reused).
* I did not determine whether `RB_UI3D_Render` is skipped entirely when no window has draws
  (`0x6E25F0` enumerates windows with non-zero counters and both dimensions > 0, so a window
  with a zero-size rect will be silently skipped — set the rect *before* tagging anything).
* I did not map the 46 `CG_OwnerDraw` cases to menu ownerdraw names.
* The second UI3D-window setter path (`0x6E1D80`, `0x6E1DE0` ← `0x62D980`) and the
  `r_ui3d_x/y/w/h` debug override were not chased.
