# Offcut UX design

The finalised interaction design for the layer editor. It replaces the "Layer
editor" section of DESIGN.md where the two differ. Reasoning, precedents,
case-study scripts, the list of changes from the draft and Rapport API
sketches are in [ux-study.md](ux-study.md).

Rapport tags: **[now]** buildable today; **[S]/[M]** needs the enhancement of
that size in section 13.

## 1. The job

A crafter (knows Cricut Design Space, not Photoshop) has pictures and wants a
framed shadowbox: 3–8 sheets of single-colour card, machine-cut, stacked with
spacers. Most pictures are not valid shadowboxes. Offcut gets from picture to a
stack where:

- every piece of card reaches the frame through its own sheet;
- no contact or feature is narrower than **Min. width**;
- sheets overlap enough to hide edges off-axis;
- **support comes from the scene.** The ideal shadowbox has no artificial
  scaffolds. That is an aspiration, not always reachable; the order of
  preference, used by every prompt and every fix list, is:
  1. subjects meet the frame or the ground;
  2. subjects overlap things on the same sheet;
  3. invented scenery or foreground that suits the picture (grass, reeds, a
     fence, foliage, cloud) and reads as scene, not as a support;
  4. recomposing, when that keeps the picture's intent.

  Thin bridges and struts are never acceptable. A drawn support is the last
  resort and is labelled as one.

Offcut never deletes material silently and never accepts SVG from a model. AI
returns rasters; Offcut quantises, validates and traces them locally. With a
provider configured, **Auto** is an AI loop that plans, draws, checks and
refines by itself (section 8); the same Auto runs locally without one.

## 2. Model

**Three stages, each a view of the document at one point in the pipeline.**

| Stage | Canvas shows | You work on |
|---|---|---|
| 1 Arrange | Artwork composed in the frame | Items: images, text, sketches, stock scenery |
| 2 Colours | Each item as flat colours | Regions, and which sheet each colour goes to |
| 3 Sheets | The card sheets | Pieces of card, issues, last-resort supports |

Export is a dialog. Nothing is locked; later stages re-derive live.

**Items, not a layer per operation.** One row per thing the user added. An
item carries its pipeline as settings, re-run live:

```
item = version (original or strokes = 0 | AI result 1..n, as a thread)
     → placement (position, scale, rotation, crop)
     → cut-out (off | local | AI) + touch-ups
     → colours (Tones N | Exact colours | From a drawing | Sketch tone) + region edits
     → colour→sheet mapping
     + Notes for AI
```

| Item type | Version 0 | Notes |
|---|---|---|
| Image | The file | |
| Text | Live lettering | Card or Cut-out style |
| Sketch | The user's strokes | One tone; section 4 |
| Scenery | A stock shape: Ground, Hill, Banner, Inner frame | Parametric |
| AI illustration | The composition or description it was made from | Created by Auto or Describe; sources hidden, not deleted; versions carry their plan |

**Sheets are always derived.** Items compose front to back into one tone map;
`build_sheets` adds frame, hidden overlap and backing. Sheets-stage edits are
stored as overrides in frame coordinates, applied after composition: tone
overrides (Show through, Push back, Remove, brush, eraser), per-sheet hidden-support masks,
and drawn-support objects (vector, re-rasterised each build). Upstream changes
never discard them. If an item moves under them, the guidance bar says so.

**Document settings ("Shadowbox")**: frame preset, size, orientation, borders,
sheet count, spacer gap, Min. width, card colour per sheet and backing, AI
Style, Auto options (until, rounds).

**Undo**: one linear history of edits, parameter changes (one step per drag),
AI results and fixes; not selection, zoom or stage. The Undo button's menu
lists the last 20 steps. [now]

## 3. Window

Top toolbar, canvas, right panel. **No left panel**: at most six tools are
live in a stage and they share the row that tool options need anyway.

```
┌────────────────────────────────────────────────────────────────────────────────┐
│ [ Mochi * ] [ + ]                                                    ─  □  ✕  │
├────────────────────────────────────────────────────────────────────────────────┤
│ New Open Save │ Undo▾ Redo │ ( 1 Arrange • │ 2 Colours │ 3 Sheets ) │ Auto     │
│                                                         ✦ AI ▾   Export…   ⚙  │
├────────────────────────────────────────────────────────────────────────────────┤
│ [V][C][T][K][W][E] │ Sketch: (Pen│Eraser) Size [3 mm] Tone [■▾] │ Use as drawn  │
│                                                            Refine with AI…     │
├─────────────────────────────────────────────────────┬──────────────────────────┤
│        ┌─────────────────────┐                      │ ARTWORK              [+] │
│        │               ░░░   │                      │ ☑ ✎ reeds        sketch  │
│        │    ▓▓▓▓     ░bird░  │ ← red hatch:         │ ☑ ▣ heron.jpg   cut-out ⚠│
│        │   ▓▓▓▓▓▓            │   won't be held      ├──────────────────────────┤
│        │   ▓▓▓▓▓  ⌇ ⌇⌇       │ ← strokes, 70 %      │ PROPERTIES · reeds       │
│        │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│                      │ Tone [■▾] Sheet [2▾]     │
│        └─────────────────────┘                      │ ☑ Fill enclosed          │
│                                                     │ Notes for AI [        ]  │
│                                                     │ Versions ▣0 ▣1⚑ ▣2  ✦    │
├─────────────────────────────────────────────────────┴──────────────────────────┤
│ ● The bird won't be held.  [Move onto branch]         1 issue      − 100% + ⤢ │
└────────────────────────────────────────────────────────────────────────────────┘
```

| Row | Contents | Rapport |
|---|---|---|
| Caption | Document tabs | [now] |
| Main toolbar | New, Open, Save · Undo (menu), Redo · stage switch · **Auto** (split button) · spacer · AI menu · **Export…** · Settings | [now]; stage switch `segmented` [M], interim toggle `tool_button`s |
| Tool row | Stage tools (radio) · active tool's options · spacer · up to three selection actions | hbox of existing controls [now]; radio group [S] |
| Canvas | One `canvas`; handles, outlines, hatches, strokes and brush ring are painted | [now]; paths and dashes [M]/[S] |
| Right panel | List panel over Properties (over Issues in Sheets); thin splitters; 46 ch | [now] |
| Bottom bar | A `stack` of two bars of equal height: **guidance** (state dot · one sentence · one action · issue count · Sheets view switch · zoom) and **review** (section 8) | [now]; tint [S] |

- The tool row and bottom bar never change height.
- The guidance bar replaces the status bar; "Saved" and the like show for
  five seconds. Priority: running job › unsupported card › other issue › next
  missing step › "Ready to export".
- Jobs never block the window: the bar shows the job and its current step
  with **Cancel**, the item shows a spinner, everything else stays editable.

## 4. Tools

One global order, V C T K W G B E P; a stage shows only its own. Pan with
Space-drag or middle-drag, zoom with the wheel; neither is a tool.

| Key | Tool | Arrange | Colours | Sheets |
|---|---|---|---|---|
| V | Move | Move, scale, rotate items | Select item | Select, edit drawn supports |
| C | Crop | Crop an image | – | – |
| T | Text | Create, edit lettering | – | – |
| K | Sketch | Draw on a sketch item | – | Same, started by Sketch scenery |
| W | Wand | Select by tolerance | Select one exact region | Select one piece |
| G | Fill | – | Set region to current colour | – |
| B | Brush | – | Paint current colour | Add card to current sheet |
| E | Eraser | Make transparent | Make transparent | Remove card |
| P | Support | – | – | Draw a Branch or Shape (fallback) |

A key for a tool the stage lacks only tells you where it lives ("Support works
in Sheets, Ctrl+3"). A key never switches stage. Shift adds or constrains, Alt
subtracts, everywhere.

| Tool | Options | Selection actions |
|---|---|---|
| Move (Arrange) | Snap to opening ☑ · Lock aspect ☑ | Touch bottom · Fit · Reset |
| Crop | Aspect: Free / Original / Opening | Apply · Cancel |
| Text | Font ▾ · Size mm · Bold · Spacing · Arc % · (Card│Cut-out) | Tighten letters · Add banner |
| Sketch | (Pen│Eraser) · Size mm · Smoothing · Tone ▾ | Use as drawn · Refine with AI… |
| Wand (Arrange) | (New│Add│Subtract) · Tolerance · Contiguous | Make transparent · Keep only this · Extract to item |
| Wand (Colours) | (New│Add│Subtract) | Delete · Merge · Recolour ▾ · Extract to item |
| Fill, Brush (Colours) | Colour swatches · Size mm | – |
| Eraser | Size mm | – |
| Wand (Sheets) | – | The piece's first two fixes (section 6) · More ▾ |
| Brush (Sheets) | Size mm (not below Min. width) · Sheet ▾ | – |
| Support | (Branch│Shape) · Width mm · Taper ☑ · Sheet ▾ · (Add│Cut away) | Finish · Cancel |
| Mark (transient, section 8) | "Drag around what should change" | Clear · Done |

### Sketch

Freehand drawing on its own item: how the user shows intent no source image
has ("reeds in front of the paws", "a fence along here", a name in her own
hand).

- K, or `[+]` ▸ Sketch, creates a Sketch item above the selection and starts
  drawing. Strokes follow the pointer with light smoothing, take pen pressure
  where the device gives it [S], and draw in the chosen tone (default
  darkest). Shift draws straight. The item shows over the scene at 70 % so
  the user draws in place. Strokes stay editable (Eraser mode; undo per stroke).
- **Use as drawn** (local): strokes widen to at least Min. width; Fill
  enclosed ☑ fills closed loops. The item then maps to a sheet like any other.
- **Refine with AI…**: sends the strokes and the scene around them (the
  composition without the sketch). The model turns the sketch into clean flat
  shapes in the scene's style, keeping placement, extent and intent, joined to
  the frame or to shapes it overlaps. The result is version 1 of the item,
  with the strokes kept as version 0, and goes through review and iteration
  like any AI result.

### Support (fallback)

Through-point curves (centripetal Catmull-Rom), no Bézier handles. Alt-click
makes a corner, Backspace removes a point, Enter finishes, Esc cancels.

- **Branch**: a clicked spine becomes card of the set width (never below
  Min. width), optionally tapered; ends snap onto card or frame and overlap by
  Min. width. **Shape**: a closed outline adds its area. **Cut away** subtracts.
- Supports stay editable with Move and list under their sheet as "Drawn
  supports". Its tooltip reads "Last resort: prefer scenery".
- Path fill needs [M]; interim: flatten to `polygon`.

## 5. Right panel

### List panel

One `list` with painted cells [now]; eye and disclosure clicks need
`on_cell_click` [S]. Selecting or ticking never reorders or scrolls it.

**Arrange, Colours: "Artwork"**, front to back. Row: tick · type icon ·
thumbnail · name · state (photo, cut-out, n colours, sketch, "to review") ·
issue mark · spinner. Drag reorders (occlusion). `[+]`: Image…, Paste, Text,
Sketch, Scenery ▸ Ground, Hill, Banner, Inner frame. Right-click offers every
operation valid for the item in any stage. F2 focuses the Name field.

**Sheets: "Sheets"**, front to back; a header row per sheet, parts indented.

```
☑ ■ Sheet 1 · front        ●2
     heron · darkest
     Drawn supports (1)    ⌁
☑ ■ Sheet 2
     reeds
☑ □ Backing
```

Header: tick · card colour · name · issue count. Selecting it targets the
brush and Support tool, and shows it in Single view. Dragging a part onto
another header reassigns that colour (`on_cell_drag`) [now]. `[+]` adds a
sheet. ⌁ is the quiet "supported by scaffold" marker (section 6).

### Properties

A scrolling column of `form` sections [now] holding parameters and the
operations that own them; each applies live through a debounced job.

| Selection | Sections |
|---|---|
| Nothing | **Shadowbox**: frame, orientation, borders, sheets, spacer gap, Min. width, card palette · **AI style** (text) |
| Image | Name · Placement (X Y W H mm, rotation, Touch buttons, Fit, Fill) · Crop · **Background** (Keep│Remove; Local│AI) · **Notes for AI** · **Versions**. In Colours: **Reduce** (Tones│Exact colours│From a drawing; N; Smoothing; Min. region) · **Palette** |
| Text | Text · Font · Size · Weight · Spacing · Arc · Style (Card│Cut-out) · Sheet · Stencil gap |
| Sketch | Tone · Sheet · Fill enclosed · Notes for AI · Versions |
| Scenery | Kind · size · Sheet |
| AI illustration | Notes for AI · Versions · **Show plan** · Palette (Colours) |
| Region (Colours) | Area · colour · Delete · Merge · Recolour |
| Sheet | Name · Card colour · piece count · narrowest contact |
| Piece (Sheets) | Area · contact width · how it is held · fixes in order |
| Drawn support | Width · Taper · Sheet · Add/Cut away |

**Palette**: a `list`, one row per colour, dark to light: swatch · share ·
sheet drop-down, whose choices end with Backing: a colour mapped to a deeper
sheet or the backing is a **hole** through the sheets in front, which cannot
float and costs no sheet. Drag rows to change depth order [now]; depth need
not follow lightness. "Sheets from [a] to [b]" spreads an item across a range.

**How material becomes sheets: by colour within an item, by item across
the scene.**

1. By colour (automatic). An item's colours map to sheets through its
   Palette: dark nearer the front by default, cumulative as `build_sheets`
   does today. A single flat illustration needs nothing more.
2. By region (spatial), when depth doesn't follow colour: a dark bush behind
   a dark cat, or a sky that should sit behind everything whatever its tone.
   Select with the wand by clicking seed points (Shift adds a point, Alt
   subtracts). It works exactly on flat regions in Colours, and with
   Tolerance and Contiguous on the image in Arrange, where it acts before
   reduction. Then choose **Extract to item** (Ctrl+J). The selected pixels
   move to a new item ("cat — extracted"):
   - it lands at the same position, so nothing shifts;
   - it is cut out of the source item, which keeps a transparent hole;
   - its own Palette and sheet range let it sit at a different depth;
   - it sits directly above the source in the list.

   The new item records where it came from, so Undo or "Merge back" reverses
   it.

So items give the coarse depth order (list order is occlusion) and colours
give the steps within an item. The usual photo flow: Arrange, remove the
background, Extract the subject from the scenery, reduce each, set each
item's sheet range in Colours.

**Versions**: the item's thread, a painted `canvas` strip in a horizontal
`scroll` [now]:

```
▣ 0        ▣ 1 ●7      ▣ 2 ●2 ✓     ▣ 3 ●4 ▲      [✦ Refine]
original   reimagine   "bird on     try again
                        the branch"
           └ ▣ 1a ●3  "simpler fur"
```

Each thumbnail has its instruction as caption, an issue badge, ✓ on the
current version, ⚑ "to review", and ▲ when it has more issues than its parent
(drift guard). An Auto run is one group ("Auto · 3 rounds") showing its best
round; the disclosure shows the others. Click makes it current; the whole pipeline follows. Refining
an older version starts a branch row. Right-click: Show prompt, Refine, Try
again, Discard.

## 6. Issues and fixes

Validation runs after every edit (background, debounced 300 ms).

**Unsupported card is computed per pixel**: for each sheet, flood-fill its
card from the frame; every card pixel not reached is unsupported and is
hatched red. In Sheets this is exact. In Arrange and Colours the same hatch is
a **forecast**, built from each item's current colours and mapping (default
Reduce settings for items not yet reduced), so a floating subject shows while
it is still being placed.

| Kind | Level | Detected as | Mark |
|---|---|---|---|
| Unsupported card | Error | Not reached by the per-sheet flood fill | Red hatch |
| Empty sheet | Error | Only its frame | Red frame outline |
| Weak contact | Warning | Disconnects when eroded by Min. width / 2 | Orange ring |
| Too thin | Warning | Removed by opening at Min. width | Orange outline |
| Speck / pinhole | Note | Under Min. region | Grey dot |
| Held by scaffold | Quiet | Reaches the frame only through a drawn support | ⌁ on the piece and its row |
| Over mat size | Warning (Export) | Sheet exceeds the mat | – |

The same data shows as canvas marks, as the guidance sentence with the first
applicable fix, and (in Sheets) as the **Issues list** under Properties: level
· sheet · kind · size. Selecting a row zooms to it; N / Shift+N step. [now]

**Fixes for unsupported card, always offered in this order**:

| # | Fix | Does | Offered when |
|---|---|---|---|
| 1 | **Extend to frame** / **Move onto…** | Moves or scales the item so it meets the border, the ground or a same-sheet neighbour, by Min. width | An item-level move of ≤ 10 mm would hold the piece |
| 2 | **Push back** / **Show through ▸** | Moves the piece to the nearest sheet behind that holds it, or cuts it as a hole to a chosen sheet or the backing | Always; hover previews the colour it will read as |
| 3 | **Fix with AI…** | The director critiques the current version: local ops where they suffice, else a marked refinement | A provider is configured |
| 4 | **Sketch scenery** | Starts Sketch on that sheet with the area highlighted; Refine with AI includes the issue so the scenery reaches it | Always |
| 5 | **Draw support** (fallback) | Switches to Support on that sheet | Always, last, labelled "fallback" |
| – | Remove · Accept | Merge into surroundings · keep and stop reporting (it will be glued) | Under More ▾ |

Other fixes: **Thicken** (weak contact, too thin: grows to Min. width only
where short); **Tighten letters**, **Add banner**, **Sit on border** (loose
lettering); for Cut-out lettering whose counters would fall out, **Use a
stencil font** first, then **Card style**, then **Stencil gaps** (a typographic
tie at least Min. width wide: the one sanctioned tie, never used on pictures).

Every fix previews on hover and is one undo step. **Fix all** applies Push
back, removes specks and thickens, then reports "Fixed 14: 9 pushed back, 3
removed, 2 thickened. Undo". It never draws supports.

Sheets with an issue selected:

```
│ [V][K][W][B][E][P] │ Wand │                 Push back   Fix with AI…   More ▾   │
├─────────────────────────────────────────────────────┬──────────────────────────┤
│        ┌─────────────────────┐                      │ SHEETS               [+] │
│        │    ▓▓▓▓      ░░░    │                      │ ☑ ■ Sheet 1 · front  ●1  │
│        │   ▓▓▓▓▓▓            │                      │ ☑ ■ Sheet 2              │
│        │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│                      ├──────────────────────────┤
│        └─────────────────────┘                      │ PROPERTIES · piece       │
│                                                     │ 38 mm² · not held        │
│                                                     │ 2 Push back  3 Fix with  │
│                                                     │ AI…  4 Sketch scenery    │
│                                                     │ 5 Draw support (fallback)│
│                                                     ├──────────────────────────┤
│                                                     │ ISSUES (1)     [Fix all] │
│                                                     │ ● 1 Unsupported  38 mm²  │
├─────────────────────────────────────────────────────┴──────────────────────────┤
│ ● Sheet 1: the bird won't be held. [Push back]  1 issue (Assembled│Single│Tilt) │
└────────────────────────────────────────────────────────────────────────────────┘
```

## 7. Views

Stage is the view. Only Sheets has sub-views, switched in the guidance bar.

| View | Key | Shows |
|---|---|---|
| Assembled | 0 | Ticked sheets in card colours, shadowed by the gap |
| Single | 1–9, PgUp/PgDn | One sheet on mat grey, others ghosted |
| Tilt | Y | Parallax: drag up to 45°; double-click recentres [now, `on_frame`] |
| Overlap | X | Hatches hidden card under sheets in front |
| Original | hold ` | The Arrange composition, from any stage |

Card palette: Tonal, All white (shaded by depth), or per-sheet `color_button`.

## 8. AI

AI is **Auto with a provider configured**: one action, a visible plan, and
knobs. Three rules hold everywhere:

- Nothing uploads without consent (one consent per run).
- Every AI raster re-enters the normal pipeline (palette snap, validation,
  issue badge) and is never trusted as valid.
- Image models follow concrete scene directions and ignore abstract rules.
  First runs showed it: "a grassy hill running off both sides" was drawn;
  "exactly 4 colours, no outlines, nothing floating" was not. So Offcut never
  relies on rules in an image prompt. A **director** turns the rules into
  directions for this picture.

**Depth is the picture.** Everything the AI does is framed as designing a
physical shadowbox, not as restyling a 2D image. The finished piece is seen in
depth, so every request, plan and critique reasons in depth planes:

- **The source is a subject reference only.** Its line work, outlines,
  shading, gradients and style are not to be copied. The models are told
  that plainly, because the first runs showed them following the source's
  cartoon line art.
- **Sheets are depth planes, near to far.** The plan names what lives on
  each plane, and the order must make physical sense. The frame and nearest
  foreground (grass, foliage, a branch reaching in) are at the front, the
  subject in the middle depths, distant scenery behind it, and sky or backing
  last. Something can't sit in front of what it rests on, or behind what it
  rises out of. A shape's edge is the edge of its card, and you see that edge
  because something lies further back behind it.
- **Image requests describe the result as a layered paper diorama seen
  straight on.** The model draws the flat design for one, with each depth
  one flat colour and nearer planes overlapping further ones. Requests list
  the planes in order ("front: a frame of tall grass; behind it the
  tortoise; behind that a low hill; back: plain sky"). Concrete depth
  directions are what image models follow.
- **The critic checks depth, not just 2D validity.** Does the stacking read
  correctly? Does each feature sit on a plausible plane? Is a near thing
  accidentally further back than what it overlaps? Are planes too similar to
  read as separate depths? Its verdict can reorder planes or move a feature
  to another plane.
- The Overlap and Tilt views are part of review, so the user judges depth
  the way it will be seen.
- **Hidden geometry.** A flat picture only shows what's visible, but every
  sheet is a whole card. Its geometry continues behind the planes in front:
  overlap for parallax, a hill running on behind the tortoise, a support
  strip nobody sees. Offcut handles it at three levels, cheapest first:
  1. **Underlap (local, automatic).** Each sheet extends under the nearer
     sheets by the layer gap, so edges never open a sliver at an angle. This
     is what `build_sheets` does today.
  2. **Hidden supports (local, automatic).** A piece that floats in the
     visible picture can still be held by card hidden behind a nearer plane.
     Offcut routes a support strip of at least Min. width from the piece to
     the frame or its sheet's card, entirely inside the region covered by
     nearer sheets, along the shortest such path. It's invisible from the
     front, so it isn't an artificial scaffold. Push back's first move is to
     try this. Hidden supports show dashed in the Overlap view, and pieces
     held this way count as supported.
  3. **Amodal completion (AI, when it shows).** Where parallax will reveal
     more of a far plane than the flat picture contains (the Tilt view shows
     how much), the far plane is completed behind the near ones. That means
     a masked edit of that plane alone, with nearer planes masked out: "the
     hill continues behind the tortoise".
  The director's plan records this per feature (`extends_behind`: which
  nearer features it continues under, and `hidden_support`: allowed or
  not). The critic checks it in the Tilt view.
- **The visible picture is locked; back-fill only goes where it can't be
  seen.** The order is always: the picture the user approved, then split
  into sheets by visible region, then back-fill. For each sheet k:
  - `V(k)`, what is visible of sheet k in the approved picture, is fixed.
    Splitting never moves a visible edge.
  - The sheet's card is `V(k) ∪ H(k)`, where the hidden part `H(k)` must lie
    inside the area covered by nearer sheets' card. Underlap, hidden
    supports and AI completion may only write into that occluded area.
  - AI completion is a masked edit of one plane. Whatever the model returns,
    Offcut keeps only the pixels inside the occluded mask and discards the
    rest, so the model can't shift an edge or redraw a visible area.
  - An invariant checks every build: compositing the sheets front to back,
    each in its palette colour, must reproduce the approved picture exactly
    (after flat fill). A mismatch is a bug, reported as an error, never
    shipped. Tests assert it for every sheet-building path.
  - Edits that change the visible picture (Push back to a deeper colour,
    Show through, Thicken, recolour) change the approved picture first,
    which then becomes the new lock. They never patch a sheet behind its
    back.
- **Drawing by planes (an alternative illustrator mode).** For hard scenes,
  or when the single drawing keeps fusing planes, the director can ask for
  each plane on its own, back to front. Each request is a separate image of
  one plane, complete including its hidden parts, given the planes already
  drawn behind it as context. Nearer planes come back transparent
  (`background: transparent` where supported, a key colour otherwise).
  Offcut composites them. It costs one call per plane, and it's the most
  direct way to say "each layer is a layer in its own right". The plan
  chooses `route: "generate"` (one picture) or `"planes"`.
- **Rendered depth cues are allowed.** Some models will draw a lit paper
  diorama, with drop shadows, ambient occlusion, or a slightly darker band
  where a near plane overlaps a far one. That's fine if it helps the model
  reason about its own layering, and the prompt neither asks for nor forbids
  it. What matters is that Offcut separates regions to their true edges:
  - **Shadow-aware separation.** Clustering weights hue and chroma over
    lightness, so a shadowed part of a plane still matches its plane. A
    darker band along a boundary that matches the far side's hue at lower
    lightness belongs to the far plane: it's the near plane's shadow falling
    on it. A soft gradient inside a region is shading, not a new tone.
  - **The wand follows the same rule.** On AI results the default is "Ignore
    shading ☑": it selects by hue and chroma, absorbs shadows and occlusion
    into the region they fall on, and stops at real colour edges. That gives
    exact regions for Extract to item and Palette mapping.
  - Separated regions are then re-filled flat, so no shading reaches the
    sheets. The rendered version stays in the Versions strip as the
    preview of how the piece should look lit.

### Roles

| Role | What | Configured as |
|---|---|---|
| **Director** | A vision chat model (Claude, GPT, Gemini, a Pollinations text model, a ChatGPT subscription) that analyses, plans, critiques. Never draws | Settings ▸ AI providers ▸ "Plans and reviews with" |
| **Illustrator** | The image model. Draws from compiled scene directions | Settings ▸ AI providers ▸ "Draws with" |
| **Offcut** | Measures, compiles, snaps, validates, scores, applies local fixes, orchestrates. Decides pass or fail | Local |

The two roles are separate choices and usually separate entries, for
example a ChatGPT subscription to plan and Pollinations' free MAI Image 2.7
to draw. With no reasoning model, Auto uses template prompts (the brief plus task
file), makes one image and goes straight to review. With no provider at all,
Auto is the local pipeline.

### The loop

| Step | Runs | Does |
|---|---|---|
| 1 Measure | Local | Composes, reduces with defaults, builds the forecast. Emits measurements (below) and an overlay image with numbered issues |
| 2 Plan | Director, 1 call | Reads composition, references, overlay, measurements, brief, Style, Notes, instructions. Returns the **plan** (JSON) |
| 3 Compile | Local | Plan → image prompt (scene directions, palette swatches, a short output spec) and local settings (sheet count, card colours, Palette mapping, ops) |
| 4 Draw | Illustrator, 1 call | Generates, or edits the previous round |
| 5 Check | Local | Snaps to the plan's palette, applies the plan's mapping and ops, builds sheets, measures, scores |
| 6 Critique | Director, 1 call | Reads result, overlay, findings, plan. Returns a **verdict**: pass, local fixes, a concrete refinement, or a changed plan |
| 7 Fix | Local | Applies the verdict's local ops and re-checks. If a refinement remains, back to 4 |

`route: "local"` in the plan skips 4: for flat art that only needs
arranging, the director plans and Offcut applies, with no image call.

**Stop** when: no errors and the director passes it; or max rounds (default 3
drawings, 1–5); or the score fails to improve for two rounds; or the user
cancels. Auto then presents the best-scoring round, not the last.
Score = unsupported area, then thin and weak contacts, then tone surplus.

**Cost and time.** A round is one drawing (about 50 s in the first runs) and
up to two reasoning calls (seconds). The consent dialog states the ceiling
("up to 3 drawings and 7 reasoning calls, about 1–4 min"); each version
records actual seconds and token usage per role; the review bar totals them.
Money is shown only where the provider reports it.

### The plan

Stored on the version, shown by **Show plan**, editable. Measurement ids
(`m…`) tie each decision to what Offcut found.

```json
{ "plan_version": 1,
  "route": "generate | planes | edit | local",
  "summary": "string",
  "style": { "notes": "string" },
  "sheets": [ { "id": "s1", "depth": 1, "card": "#hex", "role": "front | mid | backing", "holds": "string" } ],
  "features": [ {
      "id": "string", "label": "string", "source": "reference | invented",
      "keep": "keep | simplify | drop", "simplify_to": "string?",
      "arrangement": { "kind": "sheet | hole | merge", "sheet": "sheet id shown",
                       "through": ["sheet ids cut"], "merge_with": "feature id?" },
      "support": { "by": "frame | ground | overlap | hidden | scenery | not_needed",
                   "with": "feature id or edge", "contact_mm": 0 },
      "extends_behind": ["feature ids"], "hidden_support": true,
      "evidence": ["m ids"], "attention": "string?" } ],
  "scene_directions": ["string"], "avoid": ["string"],
  "local": { "sheet_count": 0, "palette": [ { "colour": "#hex", "sheet": "id" } ], "ops": [] },
  "splits": [ { "label": "string", "seeds": [[0.0, 0.0]], "sheets": ["id"] } ],
  "risks": ["string"] }
```

**Arrangement** is the reasoning the owner asked for, per feature:

| Kind | Means | Chosen when |
|---|---|---|
| `sheet` | The feature is card on its own or a shared sheet | It is large, or must read as nearer than its surroundings |
| `hole` | No card: a hole cut `through` front sheets so a deeper sheet or the backing colour shows. Cannot float, costs no sheet | A small or enclosed feature lighter or brighter than its surround (a sun, an eye highlight, shell spots), if the deeper card is the right colour |
| `merge` | Becomes part of a neighbour's sheet and colour | Too small or thin to matter, or only a tonal variation |

Invented scenery is a feature with `source: "invented"`, named ("grassy
bank", "alder branch"), with its own arrangement. Sheet count is the length
of `sheets`: the director may use fewer than the document allows.

From the taco tortoise plan (run 05 measured nine tones, bold contours and
26 floating pieces), two of its seven features:

```json
{ "id": "contours", "label": "outlines", "source": "reference", "keep": "drop",
  "attention": "dissolve every outline into the fill on either side; shapes meet edge to edge",
  "evidence": ["m1"] },
{ "id": "spots", "label": "shell spots", "source": "reference", "keep": "simplify",
  "simplify_to": "six spots, each over 4 mm",
  "arrangement": { "kind": "hole", "sheet": "s4", "through": ["s3"] },
  "support": { "by": "not_needed", "with": "", "contact_mm": 0 }, "evidence": ["m3"] }
```

It compiles to directions such as "The tortoise stands on a grassy bank that
fills the bottom third and runs off both sides", "Plain cream sky", "The
shell's spots are cream, not dark". The full taco plan and a pet-photo plan
are in ux-study.md section 4.9.


**Measurements** (Offcut → director), all with ids: frame, opening and Min.
width in mm and pixels; tones found (colour, share, component count) against
tones wanted; contour network (share, median width, connected or not); each
unsupported piece (sheet, box, area, what encloses it, distance and direction
to the nearest same-sheet card and frame edge); thin features and weak
contacts; empty sheets.

**Verdict** (director → Offcut):

```json
{ "verdict": "pass | fix_local | refine | replan",
  "explain": "one sentence for the user",
  "local_ops": [ { "op": "show_through", "target": "m7", "sheet": "s4" } ],
  "refine": { "mode": "edit | redraw", "instruction": "string", "marks": ["m ids"] },
  "plan_patch": [] }
```

Local when shapes need only reassigning, removing or widening; AI when
something must be redrawn or moved. Local ops apply automatically, grouped as
one undo step per round: `show_through`, `push_back`, `merge_colours`,
`map_colour`, `remove`, `thicken`, `set_sheet_count`, `set_card`,
`extend_to_frame`, `extract`.

### Using it

**Auto** (Ctrl+G) is a split button: Auto · ▾ Local only · Plan only… ·
Options….

1. **Consent**, once per run:

```
┌ Auto with AI ──────────────────────────────────────────────────┐
│ Plans and reviews  ChatGPT (signed in) · chatgpt.com           │
│              sees: your pictures, each drawing, issue overlays │
│ Draws        Pollinations · MAI Image 2.7 · pollinations.ai    │
│              sees: your pictures, the prompt                   │
│ ┌──────┐ ┌──────┐  2 images, 412 KB                            │
│ └──────┘ └──────┘                                              │
│ Your instructions [ plain sky; she is called Taco          ▾ ] │
│ Until  [Auto until good ▾]   Rounds [3]   ▸ Brief and prompts  │
│ Up to 3 drawings, 7 reasoning calls, about 1–4 min.            │
│ ☐ Don't ask again for this project and these providers         │
│                                          [ Start ] [ Cancel ]  │
└────────────────────────────────────────────────────────────────┘
```

2. **Progress** in the guidance bar, with **Cancel** and **Show plan**:
   "Auto · Planning…", "Drawing 1 of 3…", "Checking…", "Fixing 4 things
   here…", "Refining: join the pupil to the eyelid…". Each round's version
   appears in the Versions strip as it lands.
3. **Review bar** at the end (and after each round with "Ask after each
   round"):

```
Auto · best of 3 · 0 issues · 2 min 40   "Sun cut as a hole to the backing; numerals dropped."
[Use this] [Try again] [Support ▾] [Likeness ▾] [Detail ▾] [Colours ▾] [Write a review…] [Show plan] [Discard]
```

| Knob | Where | Values |
|---|---|---|
| AI style | Shadowbox | Text, whole project |
| Notes for AI | Item | Text, per item |
| Your instructions | Consent dialog, review bar | Text, this run; history of ten |
| Until | Consent dialog; Options | Auto until good · Ask after each round · Plan only |
| Rounds | Same | 1–5, default 3 |

User text reaches the director as preferences that cannot break the cutting
constraints; the director folds them into scene directions.

**Review responses.** Use this · Try again (same plan, new drawing) ·
Discard · chips · Write a review… (modeless: text, **Mark**, Send). Chips:
Support ▾ (Looks added on · Recompose instead · Contacts too thin · Pieces
still float · Use…), Likeness ▾ (Not like the subject · Lost a feature… ·
Changed the pose), Detail ▾ (Too detailed · Too simple · Lines too thin),
Colours ▾ (Wrong number · Wrong depth order · Gradients or noise). Chosen
chips show as a pending line with **Send**. They go to the director as
critique input with the current findings, and it writes the concrete
refinement or local ops; without a director each chip sends its line from
`review-chips.txt`. A review resumes the loop for one round. Answers are
recorded on the version. **Mark** is a transient canvas mode: drag an outline
round what should change.

**Show plan** opens a modeless dialog [now]: the summary; a sheets table
(card `color_button`, role, holds); a features `list` (label · keep · shown
as · held by · ⚠ when its evidence is still failing) with a form for the
selected feature: Keep (Keep│Simplify│Drop), Shown as (Own sheet│Hole
to ▾│Merged with ▾), Sheet ▾, Held by ▾, scenery name, attention. Buttons:
**Re-run from plan** (compile and draw, no re-analysis), **Apply here only**
(local settings and ops, no upload), Copy.

**As built (2026-11-24)**, where the build differs from the above
(`src/auto_loop.*`, `src/window_ai.cpp`):
- Auto routes by what is configured: director and illustrator, the loop;
  director only, Arrange for me (plan, then its local part); illustrator
  only, one template drawing through Send to AI, as before; nothing, the
  local pipeline. The ▾ menu is Auto · Local only · Plan only… · Arrange
  for me… · Options…. Arrange for me is there, not on Colours.
- "Don't ask again" stores the planner's and drawer's entry ids and the mode
  in the project file; any other combination asks again.
- One undo step per run (and per Continue), not per round.
- At most one local fix per round. `set_card` is applied by Offcut (the
  sheet's colour and the version's palette); `set_sheet_count` is proposed,
  not applied; `extract` and `extend_to_frame` are said as "Left for you".
  Local ops get the version as the import snapped it, composited from its
  sheets at the frame's working size.
- A pass while Offcut still finds unsupported card becomes a refinement
  from the findings. Errors are unsupported card only.
- Refinements are always sent as an edit of the last version (mode "redraw"
  is not distinguished yet); marks go as words with a rough position ("2
  small loose pieces near the lower left").
- Sheet count: reduced when the plan has fewer sheets and its summary or a
  risk names sheets, planes, layers or colours together with a reason word
  (fewer, limit, trade, simplify…); otherwise proposed in the result.
- Ask after each round shows that round's latest version with Continue ·
  Stop · Show plan; Stop presents the best round.
- Show plan edits a feature's Shown as (own sheet, hole, merged) and sheet,
  and the scene directions. Keep, Held by, scenery name and attention are
  shown, not edited; there is no Apply here only yet (Arrange for me does
  the local part from a fresh plan). ⚠ for failing evidence is not shown.
- The consent dialog shows each entry's real host (a ChatGPT entry shows
  api.openai.com, where its calls go).

### Touchpoints

All go through the same loop, entered at a different step.

| Touchpoint | Where | Enters at | Returns |
|---|---|---|---|
| Auto / Reimagine | Auto; AI menu | 1 | "AI illustration" item, sources hidden |
| Describe a scene | Empty canvas; New ▾ | 2, from text (no measurements); four first drawings shown 2×2, pick one, loop continues on it | New item; the text becomes its Notes |
| Describe an element | `[+]` ▸ Describe…; drag a box or skip; text; Sheet ▾ | 2, with the scene and the box; plan is one feature with arrangement and support | New item in place, four variants, Enter keeps one |
| Refine sketch | Sketch ▸ Refine with AI… | 2, strokes as the feature's extent | Version 1 of the sketch |
| Fix with AI | Fix lists | 6, on the current version | Local ops and/or next version |
| Arrange for me | Colours; flat art | 2 with `route: "local"` | Palette mapping, holes, merges, sheet count |
| Suggest splits | Wand ▸ Extract to item ▸ Suggest… | 2; returns `splits` | Dashed outlines from wand seeds, "Extract all" or per split |
| Remove background, Flatten | Image properties | 4 only (no director) | Version |
| Manual route | AI menu ▸ Use my own AI app | Compiled prompt copied or exported; result imported at 5 | Version |

### Provider mechanics

| | Director | Illustrator |
|---|---|---|
| Any OpenAI-compatible entry (OpenAI, Gemini, Pollinations, local) | Chat with image inputs; JSON-schema response format where offered | `/images/edits`, stateless: previous round as input, original as reference |
| Anthropic entry (new type) | Messages API: base64 image blocks, `output_config.format` JSON schema; check `stop_reason` for refusal | – |
| OpenAI API key, same entry for both | Responses API: one conversation, `previous_response_id`; the plan is still its own turn and Offcut still checks between turns | `image_generation` tool, `action: "edit"`; GPT Image 2.5+ `markup` for marks, else `mask`, else words |
| **Sign in with ChatGPT** (new type; preview) | `POST /v1/responses` only, `store: false`, `stream: true`, image input. No `previous_response_id`: every call carries its own context | Not available: no image generation or Images API on this route |

- Every director call is stateless by design (plan, findings and images are
  resent), so any entry can serve, and the single-conversation mode is only
  an optimisation.
- Sign in with ChatGPT: OAuth authorization code with PKCE through a
  `127.0.0.1` loopback redirect and dynamic client registration; the token
  goes to Credential Manager. At the plan's limit it returns 429
  `subscription_sharing_usage_limit_exceeded` and never bills: Auto stops,
  keeps the best round so far, and says "ChatGPT usage limit reached. Resume
  later or choose another model to plan with".
- No schema support: ask for JSON, validate locally against the schema, one
  repair turn, then fall back to templates for that run.
- Illustrator defaults: GPT Image 2.5/2.6 **sunburst**; **flare** for
  drafts. gpt-image-1 (shut down 23 October 2026) never appears.
- Prompts live in `data/prompts/`: `brief.txt` (now read by the director; a
  three-line form goes to the illustrator), `director-plan.txt`,
  `director-critique.txt`, `compile-*.txt`, the task templates (fallback),
  `review-chips.txt`. Each version records their hashes.
- Each version records plan, verdicts, measurements, score, applied ops,
  prompts, provider, model, seconds and usage per role. Never keys.

### Settings ▸ AI providers

- The list gains a **Can** column: "Plan · review", "Draw", or "Draw · plan
  · review". Under it, the selected entry's Name (with Duplicate and Remove),
  **Model** (the drawing model; disabled, reading "Can't draw", for entries
  that can't draw) and **Reasoning model** (typed, or chosen from the listed
  models for a ChatGPT entry; empty means the entry doesn't plan).
- Two defaults replace "Make default", as two drop-downs on one row under
  those fields: **Draws with** ▾ and **Plans and reviews with** ▾ (whose
  first item is "Nothing: Auto uses template prompts"), each listing only
  capable entries. The row menu has the same as "Draw with this" and "Plan
  and review with this". The first entry given a reasoning model becomes the
  planner.
- Add provider gains **OpenAI (Sign in with ChatGPT)**, right after OpenAI
  (API key). It swaps the key field for **Sign in with ChatGPT…** and the
  note "Uses your ChatGPT Plus or Pro plan for planning and reviewing. It
  can't draw: choose another provider to draw with." (two pages of one stack,
  so the group's height doesn't change). While the browser is open the
  status reads "Waiting for the browser… approve Offcut there", with Stop.
  Success adds the entry as "ChatGPT (<account>)" with its listed models.
  There is no separate Sign out: **Remove** on a ChatGPT entry signs out,
  after confirming.
- Add provider gains **Anthropic** (key) for reasoning only.
- A line under the defaults always says what Auto will do: "Auto plans and
  reviews with A, and draws with B", "Auto will draw once from a template,
  without planning…", or "Auto can plan and arrange, but not draw…".

### What local Offcut must expose

- `measure(document) → measurements + overlay` (adds: enclosing tone, nearest
  support distance, contour-network detection, tone surplus).
- `score(measurements)`; `apply(plan.local)`; `apply(ops)` as one undo group.
- **Show through** as a first-class region operation: cut a region through
  to a chosen sheet or the backing (Push back is "show through to the next
  sheet that holds"). Offered in Palette (a colour's sheet may be Backing),
  on pieces, and in fix lists.
- Card colour per sheet including the backing, and depth order independent
  of luminance (both already in Palette).
- Snap to a given palette (the plan's).
- Dissolve outlines: dark strokes narrower than about 3 × Min. width that
  separate two fills are reassigned to the fills on either side (split down
  the middle), so shapes meet edge to edge. Runs on every AI result and on
  cartoon sources before Reduce.
- Wand from seed points (for `splits`).

## 9. Export

**Export…** (Ctrl+E), a dialog [now]:

- **Preflight**: issues by sheet, each a link back. Errors do not block; the
  button reads "Export anyway". Sheets held by scaffold are listed quietly.
- **Cut list**: per sheet thumbnail, card colour, size, piece count, changed
  since last export ☑.
- **Options**: SVG per sheet │ one layered SVG · PNG references · assembly
  notes · Only changed sheets · Mat size.
- Notes give the exact size to enter in Design Space, order, spacer thickness
  and card colours.

## 10. Empty states and first run

- **Empty document**: the canvas paints "Drop a picture here" with **Choose
  images…**, **Paste**, **Describe a scene…**, **Start with a sketch**,
  **Open project…** and recent projects. Describe a scene is a field on the
  canvas ("A fox asleep under birch trees, snow falling") with frame and
  sheet count beside it; Generate opens the consent dialog, which says "text
  only, no images uploaded" (section 8).
- Drop or paste creates the project with the last-used frame (first run
  8×10 in, orientation from the image); no setup dialog. Ctrl+N opens the
  small frame dialog.
- **After the first image**: Touch bottom if the background is plain or
  transparent, else Fit. The forecast hatch is already live. Guidance:
  "Arrange it, then **Auto**". Auto's tooltip says what it will use: "AI
  director and illustrator", "AI illustrator", or "on this computer".
- **No provider**: Auto runs locally; the result's guidance line ends "…or
  set up AI for better results" (a link to Settings), shown once per project.
- **Stage with nothing to show**: Colours paints the composition dimmed with
  **Reduce**; Sheets offers **Auto**.
- **No tour.** The guidance bar teaches a step at a time; tooltips carry the
  shortcut and one sentence.

## 11. Keyboard

| Keys | Action |
|---|---|
| Ctrl+N / O / S / Shift+S / W / T | New, open, save, save as, close tab, new tab |
| Ctrl+I, Ctrl+V | Add images, paste image |
| Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z | Undo, redo |
| Ctrl+1 / 2 / 3 | Arrange, Colours, Sheets |
| Ctrl+G, Ctrl+E, Ctrl+, | Auto, Export, Settings |
| V C T K W G B E P | Tools (section 4) |
| Space-drag, wheel, Ctrl+0 / = / − | Pan, zoom, fit, in, out |
| [ and ] | Brush, sketch and Branch size |
| Enter, Esc, Backspace | Commit; cancel; remove last point |
| Delete, M | Delete selection; merge region |
| Ctrl+] / Ctrl+[ , Ctrl+D, F2 | Item forward / back, duplicate, rename |
| 0, 1–9, PgUp/PgDn, Y, X, hold ` | Views (section 7) |
| N / Shift+N, F, Shift+F | Next / previous issue, first fix, Fix all |

Single keys act only when no text field has focus (`window::on_key` routes
this way) [now]. No key sends to AI. Hold-keys need key releases (verify) [S].

## 12. Rules from the case studies

| Case | Insight | Rule |
|---|---|---|
| Pet photo | Photos fail on isolation and speckle first | Forecast hatch in Arrange; Smoothing defaults high; offer Reimagine |
| Scanned sketch | Lines are too thin to be card and enclose the real shapes | From a drawing mode turns the drawing into flat regions: lines become boundaries, enclosed areas are filled, then the lines are dissolved into the fills. No line survives as card |
| Multi-image scene | Depth is per item, not per luminance | Sheet range per item; list order is occlusion |
| Lettering | Card letters must touch; cut-out letters must not enclose | Two text styles; stencil font before stencil gaps |
| Flat logo | It already has its colours | Exact colours at ≤ 8 flat colours; user owns depth order |
| AI-first | Image models draw directions and ignore rules; results vary | Director plans per picture; local checks decide; best round is presented |
| Arrangement | A floating light detail is often better as a hole than as card | Per feature: own sheet, hole, or merge; Show through is a local op |
| Sketch scenery | The user knows what should hold the piece but cannot draw it cleanly | Sketch states intent; AI makes it card; strokes kept as version 0 |
| Repair after a test cut | Changes are small and local | Min. width re-validates live; export only changed sheets |

## 13. Rapport gaps

| # | Size | Need | Used by | Interim |
|---|---|---|---|---|
| 1 | M | Segmented control with per-segment badge | Stage and view switches, mode toggles | Exclusive toggle `tool_button`s |
| 2 | M | Painter paths: fill/stroke with curves, holes, even-odd | Outlines, supports, sketch strokes, Mark | `polygon`; CPU-rasterised overlays |
| 3 | S | Stroke dashes and joins | Marching ants, Mark, overlap | Solid two-tone outline |
| 4 | S | Painter transform push/pop | Rotated items | CPU-rotate |
| 5 | S | `list::on_cell_click` in painted cells | Eye, disclosure, swatches | Checkbox column |
| 6 | S | `tool_button` radio group; checkable `button` (chip) | Tool row; review chips | Manual `set_checked`; checkable menu items |
| 7 | S | Pointer pressure and pen/eraser flag; pointer capture; key release (verify) | Sketch, drags leaving the canvas, hold-keys | Constant width; toggle keys |
| 8 | S | Canvas file drop with position | Drop onto canvas | Window drop |
| 9 | S | Tinted bar surface; `spin` suffix; `drop_down` item icons | Bars, mm fields, tone and sheet pickers | Dot icon; label text |
| 10 | M | Offscreen painter and font list | Text to raster | DirectWrite in Offcut |
| 11 | M | Inline cell edit in `list` | Rename | Name field + F2 |
| 12 | M | Popover anchored to a widget | Review text, hover previews | Modeless dialog; not required |

The review bar, version strip, consent dialog and plan dialog need nothing
new: `stack`, `button`, `menu_button` with checkable items, `combo`, `list`
with a `form` beside it, `color_button`, a painted `canvas` in a horizontal
`scroll`, modeless `dialog`. The Auto split button is a `tool_button` beside
a `menu_button` [now].

## 14. Phases

Each phase ends with a usable picture-to-SVG flow. AI comes first because
testing with real providers has started, and its first results set phase 2.

| Phase | Delivers |
|---|---|
| 1 Shell and AI loop (in progress) | Stage switch, tool row (Move, Crop), Artwork list, Shadowbox properties, guidance bar, undo, local Auto. Per-pixel unsupported hatch in every stage; Push back, Remove, Fix all; nothing removed silently, the AI import included. Export dialog. **AI**: `data/prompts`, Style / Notes / instructions, consent dialog, one-shot Reimagine from templates, review bar, Versions strip with badges and drift mark, stateless refine. Built so phase 2 slots in (ux-study.md section 8). |
| 2 Director | `measure`, `score`, snap to a given palette, dissolve outlines, Show through, ops with grouped undo. Provider roles with two defaults; Sign in with ChatGPT and Anthropic entry types. Plan and verdict schemas, `director-*.txt`, compile. The loop with stop rules, progress, Auto groups in the strip, Show plan (read, edit, Re-run, Apply here only), chips routed through the director, Fix with AI as critique. Arrange for me (`route: local`). |
| 3 Iteration, describe and sketch | Responses API single conversation; Mark with markup/mask; branching. Describe a scene and an element. Sketch items and tool, Refine sketch, Sketch scenery. Extend to frame / Move onto. Suggest splits with Extract to item. |
| 4 Colours | Reduce (Tones, Exact colours), Palette and sheet mapping, wand, Fill, brush, eraser, specks, local background removal |
| 5 Contacts and fallbacks | Weak-contact and too-thin checks, Thicken, Issues list, Support tool, scaffold marker, Accept, Overlap and Tilt views |
| 6 Lettering, scenery, finish | Text items and fixes, stock scenery, From a drawing mode; AI background and flatten, only-changed export, mat check, curve-fitted SVG, segmented control |

Gaps 5 and 6 help phase 1; 2, 3 and 7 help phase 3; 10 is needed for phase 6.
All have interims. The director adds no Rapport gaps.

## 15. Open questions

Decided by the owner (2026-11-24):
- The consent dialog offers "Don't ask again for this project and these providers". Any change of provider, role or project asks again.
- Pictures going to two providers when roles are split is acceptable by default; the consent dialog still lists both.
- Auto may reduce the sheet count itself when the director is confident (it says so in the plan, with its reason), and otherwise proposes it.
- 3 rounds is the default. No spend limit for now.
- No outlines, ever. Shadowboxes get their look from depth: each shape's
  edge is the edge of its card, so a separate sheet of lacy contour lines
  would sit at a different depth from the shapes it outlines and break the
  3D effect. The brief asks for flat shapes meeting edge to edge, and Offcut
  dissolves any outlines a model draws anyway into the neighbouring fills.
- Sign in with ChatGPT is an alternative way to add OpenAI, beside an API key. It is a different registration: a browser sign-in that stores a refresh token, not a pasted key. It can plan and review but not draw, so a plan-only OpenAI entry shows "Can: plan · review" and needs a separate "Draws with" entry.

Still open:
1. Should Accept mark a piece "glue on" and export it separately with a placement outline?
2. Stencil gaps on cut-out lettering are the one tie this design allows. Keep, or require a stencil font?
3. Card palette default (Tonal or All white), and which cutting mats matter besides 12×12 in?
