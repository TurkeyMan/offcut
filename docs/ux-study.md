# Offcut UX study

Supporting analysis for [ux-design.md](ux-design.md): what the precedents do,
eight worked interaction scripts, what Rapport can express, and the
alternatives that were rejected. Reference material; the design doc is the
specification.

Revised after the product owner's decisions on the shadowbox brief, support
philosophy, prompt additions, result review, iteration threads,
unsupported-pixel detection and sketch items. Model names and API parameters
in both documents are as supplied, not verified here.

Sources read: DESIGN.md (the "Layer editor" draft), README.md,
`src/window.cpp`, `src/transform.*`, `src/project.h`, the two screenshots, and
Rapport's `layout.h`, `widgets.h`, `rapport.h`, `backend.h` and
`docs/design-review.md`. Precedent notes are from knowledge of those products,
not from fresh testing.

## 1. The problem, restated

The user is a crafter with a cutting machine. She can drag, resize and weld in
Cricut Design Space; she has not used a pen tool and should not have to. What
makes this hard is not image editing, it is that the picture she starts from
is usually not a physically possible object:

- A subject on a plain background touches nothing. Today's pipeline yields
  frame-only sheets and a status-bar sentence.
- A photo quantises into speckle; hundreds of floating crumbs are removed
  silently, and the user sees a design that has changed without being told how.
- Features that share a colour share a sheet, so an eye, a collar and a
  letter all have to find their way to the frame through the same card.
- Thin bridges would make it "valid" and she rejects them: they tear, droop
  and look like an apology. Her standard is higher than "broad supports": a
  really good shadowbox integrates what holds it up into the subjects of the
  scene, and the ideal has no artificial scaffolds at all. Where that cannot
  be reached, invented scenery or foreground detail takes the scaffold's
  place.

So the tool is a validity editor with a picture attached. The interaction
model follows from three consequences:

1. Problems must be visible where they are and when they arise, with a named
   fix. The earlier the stage, the cheaper the fix (moving the cat down 12 mm
   beats drawing six supports).
2. Support is a composition problem before it is a drawing problem. Fixes
   are ranked: meet the frame or ground, overlap a same-sheet neighbour,
   invent scenery that suits the picture, recompose; a drawn support is the
   labelled last resort. The same ranking is the heart of the brief every AI
   prompt opens with, because models do not know what a shadowbox is. Where
   tools do add card they make good card by construction: minimum
   widths clamped, ends overlapped, broad shapes cheap.
3. Everything downstream must re-derive, because the fix for a Sheets problem
   is often upstream (reposition, recolour, re-prompt).

## 2. Precedents

### Raster editors: Photoshop, Affinity Photo, Krita, GIMP, Photopea, Procreate

| Pattern | Verdict |
|---|---|
| Vertical tool palette, single-key shortcuts, options bar across the top | Borrow the keys (V, C, T, W, G, B, E, P are near-universal) and the options bar. Do not borrow the palette column: these apps have 20–60 tools; Offcut has eight, five live at a time. |
| Layers panel: eye, thumbnail, name, drag to reorder, front at top | Borrow exactly. It is also what Design Space shows. |
| Adjustment layers, smart objects and filter layers as non-destructive derivations | Borrow the principle (parameters stay live), not the presentation. A stack of derived layers per image is a history the user must manage. Offcut's pipeline per item is fixed, so it can be a fixed set of settings, like Lightroom's Develop, not free layers. |
| Magic wand: tolerance, contiguous, add/subtract by Shift/Alt, marching ants | Borrow all of it. On flat colour regions, tolerance disappears (exact region), which is the draft's good idea. |
| Photoshop's contextual task bar and Properties "quick actions" (Remove background) | Borrow: operations offered beside the selection and in Properties, not in a menu bar Offcut does not have. |
| Generative fill variations: a strip of results, pick one, originals kept | Borrow directly as the Versions strip. It is the right shape for "iterate on the AI result". |
| Procreate: few tools, brush size always at hand, gestures | Borrow restraint and the always-visible size control. |
| GIMP: mode-heavy dialogs, operations that commit destructively | Avoid. |
| Krita's colourise mask / lazy brush: fill enclosed line-art regions | Borrow the idea for From a drawing mode: fill regions bounded by lines. |
| Photopea: the whole Photoshop model in one window | Confirms users cope with the layout; nothing new to take. |

### Vector tools: Illustrator, Inkscape, Affinity Designer, Figma

| Pattern | Verdict |
|---|---|
| Bézier pen with handles | Avoid. It is the least learnable tool in graphics software and neither scenery nor fallback supports need its precision. |
| Curvature pen (Illustrator), spiro and BSpline modes (Inkscape): curves through or near clicked points | Borrow. Through-point curves (centripetal Catmull-Rom, which does not loop or overshoot between uneven points) with Alt-click corners. |
| Width tool / variable-width strokes; Inkscape's "shape: triangle in" taper; Procreate's tapered brushes | Borrow as the Branch mode: a spine with a width and a taper is a branch, a stem, a blade of grass, a ribbon. The width is a number that can be clamped to Min. width. An outline tool cannot promise that. |
| Illustrator Image Trace: presets, a live panel with Colours/Paths/Noise sliders, "Expand" as the destructive step | Borrow the live panel with few sliders (N, Smoothing, Min. region) and the presets idea (Tones, Exact colours, From a drawing). Avoid "Expand": Offcut never needs the destructive step because tracing happens at export. |
| Illustrator Live Paint: click to fill regions formed by intersecting paths; gap detection | Borrow for drawings: Fill tool on thresholded line drawings, with "close gaps under N mm". |
| Figma: properties follow selection; canvas-first; contextual toolbar at the top of the canvas | Borrow selection-driven Properties. The Sheets/Artwork panel swap is the same idea applied to the list. |
| Affinity Personas | See stages, below. |
| Text on a path | Illustrator and Inkscape need a path first, then text attached to it, then offset handles. Too many concepts. Canva and Design Space offer one "Curve" slider, which covers the real cases (a name arched over or under a subject). Borrow the slider: straight, arc, full circle. Arbitrary paths are an open question. |

### Workflow shells: Lightroom modules, Affinity Personas, slicers

- **Lightroom**: Library, Develop, Print are views of one catalogue at
  different stages, always switchable, with panels that change per module.
  Edits are parameters, never pixels. Both ideas are taken: stages as views,
  per-item settings instead of derived layers. Lightroom's "before/after" key
  (\\) becomes hold-` for Original.
- **Affinity Personas**: same window, different tool sets. The useful part is
  that a persona changes what clicking the canvas *means*. The weak part is
  that personas hide tools users then hunt for. Offcut keeps tool keys
  constant across stages (W is always "select the thing under the pointer"),
  and the object changes: pixels, regions, pieces.
- **3D-print slicers** (PrusaSlicer, Cura): Prepare, then Preview, with
  supports generated and painted on, and a list of warnings that each select
  the offending object. This is the closest analogue of all: a validity
  editor for a physical object. Borrow the warnings-select-geometry pattern,
  and "paint-on supports" as a first-class tool.

Why three stages and not five: a stage earns its place when the canvas shows
something different and clicking means something different. Arrange (items),
Colours (regions) and Sheets (card) pass. "Separate" is a button. "Export"
has nothing to click on the canvas.

### Cutting tools: Cricut Design Space, Silhouette Studio, LightBurn, Canva

| Tool | Does well | Borrow | Avoid |
|---|---|---|---|
| Design Space | Layers panel grouped by colour, since colour means mat; Weld, Attach, Contour as plain-language operations; "Make" shows mats before cutting | Colour means sheet. Plain verbs: Push back, Thicken, Remove. The Export dialog as "Make": a cut list with thumbnails | Its upload flow rescales SVGs unpredictably: export notes must state the exact size to type. Its modal, slow steps |
| Silhouette Studio | Trace panel with threshold and a live yellow preview over the image; cut-line preview in red | Live preview over the source; cut preview in Export | Panels hidden behind an icon strip |
| LightBurn | Layers are colours with per-layer settings; Preview window; warnings for open shapes and out-of-bounds before sending; "select open shapes" | Preflight that selects the offender; mat-size check | Engineering-tool density |
| Canva | Empty states that are a call to action; one-click background remover; "Magic" AI actions offered in context on the selected image; text "Curve" slider | Drop-first empty state; AI offered on the selection, not in a separate mode | Unreviewed uploads; this user's images may be family photos |

### AI generation and iteration: Midjourney, ChatGPT images, Firefly, generative fill

| Pattern | Verdict |
|---|---|
| A grid or strip of results, each a starting point for "vary" | Borrow as the Versions strip: a thread per item, branching allowed, captions from the instruction |
| "Vary region" and generative fill: lasso an area, describe the change | Borrow as Mark. It maps onto a provider `markup` overlay or `mask`, and it is how a non-writer says "this bit" |
| Conversational editing that keeps context ("now make the sky darker") | Borrow the mechanism (Responses API with `previous_response_id`), not the chat window. A one-line instruction on the version is enough |
| Thumbs up/down with canned reasons | Borrow, sharpened: the canned reasons are this domain's real failure modes (support looks added on, likeness lost, too detailed, wrong tones) and each becomes a follow-up instruction, so judging a result and asking for the next one are the same click |
| A free-text prompt box as the only control | Avoid. The user's words are preferences at three scopes (project Style, item Notes, this request); the cutting constraints are Offcut's and are not editable in place |
| Results silently replacing the work | Avoid. A result is a preview until answered in the review bar |
| Sketch-to-image (structure references, scribble control, Krita's AI plugins) | Borrow as Sketch items: the user draws rough intent in place and the model renders it in the scene's style. The strokes stay as version 0 |

### Validation and preflight

- **InDesign preflight**: a persistent dot (green/red) with an error count in
  the status bar, opening a panel of errors grouped by kind, each a link to
  the object. Borrow the persistent count in the bar and the linked list.
- **IDE problems panels**: next/previous problem keys, quick-fix on the
  problem. Borrow N / Shift+N and F for the default fix.
- **Slicers**: issues drawn on the geometry. Borrow.
- **Spell-check**: "Ignore" is a legitimate answer. Borrow as Accept.

The model in the design doc is the union: marks on the geometry, one sentence
with one button in the bar, a list for the rest, a default fix per kind, bulk
fix with a summary, and accept.

### Undo

Photoshop's History panel is powerful and rarely understood; Lightroom's
history list is linear and clear; Figma has plain undo. Offcut: linear undo,
named steps in the Undo drop-down, parameter drags coalesced. Because sheets
are derived, undoing an upstream change never leaves stale output, which is
what the current `stale` flag works around.

### Onboarding and empty states

Tours are skipped. What works is a canvas that says what to drop on it
(Canva, Figma), and a product that states the next step in place (Design
Space's flow does this by force; Offcut does it by suggestion). The guidance
bar is the whole onboarding.

## 3. What Rapport can express

Read from `core/include/rapport/layout.h` and `widgets.h`.

**Available now and used by the design**

- Caption document tabs, `base_surface`, `toolbar`, `tool_button` (toggle,
  drawn icons, tooltip), `menu_button`, `button` (icon, flat), `label`
  (link, icon, ellipsize), `separator`, `spacer`.
- `splitter` (thin, sizes saved), `scroll`, `stack`, `tabs` (sidebar),
  `group`, `form` with aligned label columns across forms.
- `spin`, `slider` (marks, snapping, non-linear maps), `drop_down`, `combo`,
  `check`, `entry` (actions, placeholder), `color_button`, `text_view`,
  `progress_bar`.
- `list`: virtual model, painted cells with `painter.image` for thumbnails,
  multi-selection, checkboxes, row reorder, cell drag onto rows, per-cell
  context menus and tooltips, fixed `row_height`.
- `tree`: icons, checks, lazy children, context menu. Single selection, no
  drag, no painted cells, so the layer lists use `list` with painted
  indentation instead.
- `canvas`: paint, pointer (buttons, double-click, modifiers), scroll, keys,
  per-frame callback, cursors (including `hidden`, so a brush ring can be
  painted), CPU image present.
- `painter`: lines, cubic curves (stroke only), circles, arcs, rounded
  rects, closed polygons (fill and stroke), images with alpha and filter,
  clip, text and measurement, theme colours.
- Dialogs (modal and modeless), message boxes, file dialogs, clipboard text
  and images, window-level file drop, timers, `ui.post`, window `on_key`
  that leaves typing to focused fields.

**Missing, and what that pushed**

| Missing | Consequence in the design |
|---|---|
| Widgets over a canvas | No floating view overlay; the view switch is in the guidance bar. Canvas handles are painted and hit-tested by Offcut |
| Popovers | No pop-up panels; parameterised operations are Properties sections, which is also the better interaction |
| Segmented control | Proposed (gap 1); toggles in the interim |
| Path fill with curves and holes, dashes, transforms | Proposed (gaps 2–4); polygons and CPU rasters in the interim |
| Click events in painted list cells | Proposed (gap 5) |
| Inline rename, drag in trees | Avoided: Name field and a flat list |
| Offscreen text rendering, font list | Proposed (gap 10) or DirectWrite inside Offcut |
| Pen pressure in `pointer_event` | Proposed (gap 7); constant-width strokes in the interim |
| Toolbar overflow | The tool row is limited to six tools and three actions so it fits at 1100 px |

Sibling apps (Tortilla's settings and Accounts page, already mirrored by
Offcut's Settings; IfritView's painted canvas overlays; Expedition's tab
lifecycle) set the tone: native controls, aligned forms, no menu bar, changes
that apply at once on list pages. Nothing here departs from that.

## 4. Case studies

Each script names the control used. "Bar" is the guidance bar.

### 4.1 Pet portrait from a photograph

Photo of a spaniel on a sofa, 4:3, busy background. Goal: 8×10 in portrait,
five sheets.

1. Drop `biscuit.jpg` on the empty canvas. Project created, 8×10 in portrait.
   Canvas: photo fitted in the opening, handles on. Bar: "Arrange it, then
   **Auto** makes sheets, or go on to Colours."
2. Click **Auto** to see the worst case. Lands in Sheets, Assembled. Canvas:
   sofa texture as confetti; red hatch everywhere. Bar: "63 floating pieces.
   **Fix all**". The user sees that the photo itself is the problem. Ctrl+Z.
3. Properties ▸ Background ▸ **Remove**, method Local. The flood from the
   edges fails on the sofa (similar colour to the fur). Canvas: half the
   sofa remains.
4. Switch method to **AI**. The consent dialog: one thumbnail (the photo),
   provider "OpenAI (work)", **Send**. Row shows a spinner; Bar: "Waiting for
   GPT Image 2.6 sunburst… Cancel". Result arrives as version 1 with alpha, shown over a
   checkerboard. Review bar: "Result 1 · Does this work?" **Use this**.
5. The whole dog is hatched red (the forecast: nothing would hold it). Bar:
   "biscuit.jpg won't be held. **Extend to frame**". Click it; the chest sinks 10 mm into the bottom border.
   Scale up with a corner handle so the ears nearly reach the sides.
6. Ctrl+2. Canvas: "Not reduced yet. **Reduce**". Click. Properties ▸ Reduce:
   Mode Tones, Colours 4, Smoothing 3, Min. region 12 mm². Canvas updates
   live. Fur highlights are still noisy; raise Smoothing to 5 and Min. region
   to 25.
7. W, click the stray light patch on the back; Shift-click two more; **Merge**
   (M). Click the bright eye highlight; Recolour ▾ ▸ lightest.
8. Palette list shows colours 1–4 on sheets 1–4, Backing on 5. Fine.
9. Ctrl+3. Bar: "Sheet 1: nose and both eyes float. **Push back**". Canvas:
   three red-hatched pieces. Press N to step through: hovering **Push back**
   previews the nose on sheet 2 (still floats, the muzzle is lighter), then
   settles on sheet 3. Take it for the nose (F).
10. The eyes matter. With an eye selected, a drawn support would look wrong
    on a face, and the fix list puts it last for that reason. Better: Ctrl+2, W on the dark eye-surround region, Recolour it
    to colour 1 so eyes join the dark ear mass through the brow. Back in
    Sheets the eyes are supported. (Fix upstream, re-derive.)
11. Remaining: "Weak contact, 1.1 mm" at the collar. **Thicken**. Bar: "Ready to
    export."
12. Y, drag to tilt and check the overlap; Ctrl+E.

Problems and resolutions: busy background (AI cut-out); isolation (Touch
bottom, flagged in Arrange); speckle (Smoothing, Min. region); facial
features floating (recolour upstream so they join a mass, not a bridge).

Alternative: at step 3, AI ▾ ▸ **Reimagine as shadowbox** does steps 3–8 in
one request (see 4.6).

Insight: photos fail on isolation and speckle first. Rules: forecast unsupported card
in Arrange; Smoothing defaults high for photographic sources; the bar
offers Reimagine for photos.

### 4.2 Scanned pencil sketch

A phone photo of a pencil drawing of a fox among ferns. Grey paper, uneven
light, lines 0.3–0.8 mm at print size.

1. Drop `fox.jpg`. C, crop away the desk; Enter.
2. Ctrl+2, **Reduce**. Mode detects low saturation and thin dark strokes and
   preselects **From a drawing**. Properties: Threshold (slider, live),
   Close gaps under 1.5 mm. The lines are only boundaries here: they will
   not become card.
3. Canvas: the line drawing, cleaned. Uneven lighting leaves a grey smear in
   one corner: E, erase it. Raise Threshold until faint fern veins either
   appear cleanly or vanish.
4. Bar: "**Fill** the shapes between the lines." Every enclosed area is
   unassigned (shows as backing).
5. G. Tool row shows swatches for colours 1–4. Pick colour 1 (front), click
   the ferns; colour 2, the fox's body, tail and head; colour 3, the ground
   line's hills. Sky stays backing.
6. A click on the tail floods the whole background: a gap in the line. Ctrl+Z,
   raise Close gaps to 2.5 mm, or B to draw the gap shut. Fill again.
7. Apply: the lines dissolve into the fills on either side, so the fox and
   ferns become flat shapes meeting edge to edge. No line survives as card.
8. Ctrl+3. Issues: "Unsupported" for the fox's eye dot; the ferns' fronds
   are flagged too thin. The fox floats above the ground. Fixes in order:
   Move onto (Ctrl+1, drag the fox down until its feet overlap the hills),
   then Thicken for the fronds.
9. Eye dot: **Show through** makes it a hole to the backing, which reads as
   a highlight. Whisker marks were never filled, so nothing to remove.
10. Overlap view to check the depth; Export.

Problems: lighting gradient (threshold, eraser); open contours (Close gaps,
brush); a subject that floats (Move onto); fine fronds (Thicken).

Insight: a sketch's lines describe shapes; they are never the card.
Rule: From a drawing (threshold, close gaps), then Fill, then dissolve the
lines into the fills.

### 4.3 Landscape composed from several images

`pines.png` (silhouette with transparency), `deer.png` (photo cut-out),
`mountains.jpg` (photo), `moon.png`. Goal: six sheets, landscape 10×8 in.

1. Ctrl+N, choose 8×10 in, Landscape. Select all four files in **Choose
   images…**. Artwork list: four rows.
2. Drag rows into order: pines, deer, mountains, moon (front to back).
3. V. Scale pines to full width, drag down until the trunks pass into the
   bottom border (snap guide lights at the opening edge). Mountains: Properties
   ▸ **Fill**. Deer: place on the ridge line.
4. Canvas hatches deer and moon red (forecast). Bar: "deer.png won't be
   held. **Extend to frame**". Not wanted: the deer stands on a ridge. Leave
   it; the ridge will support it if they share a sheet or the deer overlaps a
   nearer sheet.
5. Ctrl+2. Select pines ▸ Reduce ▸ Exact colours finds one colour. Set "Sheets
   from 1 to 1". Deer: Tones, 2 colours, sheets 2–3. Mountains: Tones, 3
   colours, sheets 3–5; W and **Delete** on the sky region so the moon and
   backing show. Moon: one colour, sheet 5.
6. The Palette list for each item shows the mapping; the canvas shows the
   composed flat scene.
7. Ctrl+3. Issues: deer's dark colour on sheet 2 floats (nothing else is on
   sheet 2). Moon floats on sheet 5.
8. Deer: three options are visible in the fixes. **Push back** would put it on
   sheet 3 with the near ridge, where its feet overlap the ridge: supported,
   and physically right. Take it.
9. Moon: the first fix offered is **Move onto…** the far peak (6 mm away).
   Take it; where no peak is near enough, Ctrl+1 and add Scenery ▸ **Hill** behind the mountains on sheet 5 reaching both
   sides for it to rest on. Supported by contact, no scaffold.
10. X (Overlap) shows hidden card: pines extend under nothing (front); each
    ridge extends 5–15 mm under the one in front. Y to tilt: no gaps open at
    45°.
11. Export. Cut list shows six sheets; sheet 2 is now frame-only: preflight
    warns "Empty sheet". Shadowbox ▸ Sheets 5; mappings shift; export.

Problems: per-image depth (sheet ranges); isolated mid-ground subject (push
back to the sheet of what it stands on); a sheet left empty by fixes (empty
sheet warning; reduce the count).

Insight: in a composition, depth is assigned per item and luminance only
orders tones within an item. Rules: per-item sheet range; list order is
occlusion; an empty sheet is an error that points to the sheet count.

### 4.4 A name in lettering

"EMILIA" across the middle of a 5×7 in landscape box, with a row of tulips
(one flat illustration) along the bottom. Three sheets.

1. Empty document from Ctrl+N. T, click in the opening, type EMILIA, Esc.
   Tool row: Font ▾ (stencil and heavy faces listed first), Size 28 mm, Bold,
   Spacing 0, Arc 0, style **Card**.
2. Canvas: six letters outlined orange. Bar: "EMILIA: no letter touches
   anything. **Tighten letters**". Click: spacing goes negative until each
   pair overlaps by Min. width. E-M-I-L-I-A is now one piece, still floating
   as a word.
3. Bar: "EMILIA touches nothing. **Add banner**". Alternatives in Properties:
   Sit on border, Add banner. The user wants the name mid-frame, so: Add
   banner. A ribbon item appears behind the word on the same sheet, ends
   running into the left and right borders. Drag its height handle to 14 mm;
   the letters rise out of it.
4. Counters: A's triangle is a hole in the card, which is fine in Card style.
   No issue.
5. Drop `tulips.png`. Touch bottom applied (transparent background). Ctrl+2:
   Exact colours finds 3; map to sheets 1–3. Text: Sheet 2.
6. Ctrl+3. Issue: "Too thin, 1.3 mm" on the I's serifs and the tulip stems
   where they cross behind the banner. **Thicken** on the serifs. Stems: they
   are on sheet 1 and reach the bottom border; fine after Thicken.
7. Variation: the user wants the name *cut out* of sheet 1 so the pink of
   sheet 2 shows through. Select text, style **Cut-out**, Sheet 1. Canvas:
   letters become holes; the A's counter and nothing else turns red. Bar:
   "A: the centre will fall out. **Use a stencil font**". Click: the font
   list opens at the stencil faces; with one chosen the issue never appears.
   If she insists on her font, **Stencil gaps** adds a 2.5 mm tie at the
   natural stencil position: the one tie Offcut allows, and only on lettering.
8. Arc 30 % bends the word over the tulips; banner follows (it is tied to the
   text item).
9. Export.

Problems: loose letters (tighten); floating word (banner or border); thin
serifs (thicken, or a heavier font); cut-out counters (a stencil font, else stencil gaps).

Insight: lettering has two physical forms with opposite failure modes. Card
letters must touch; cut-out letters must not enclose. Rules: Text has a
Card/Cut-out style; letter checks run in Arrange; fonts list stencil and
heavy faces first.

### 4.5 Flat cartoon logo with transparency

`tortilla-master.png`: a cartoon tortoise in four flat colours with
anti-aliased edges, transparent background (the image in the current
screenshot).

1. Drop it. Placed with Touch bottom automatically; feet sink into the
   border.
2. Ctrl+2, **Reduce**. Mode preselects **Exact colours**: "4 colours found";
   anti-aliased edge pixels snap to the nearest. No smoothing, no k-means, no
   sliders except Min. region.
3. Palette list orders by luminance: dark outline, mid shell, light belly,
   cream. The user wants the shell in front of the outline? No: the outline
   is the darkest and frontmost, as usual. But the cream eye highlight should
   be the backing, not a sheet. Set cream's sheet to Backing.
4. Ctrl+3. Issues: eye pupil (dark, sheet 1) floats inside the light eye;
   shell highlights (light) are islands on sheet 3 but supported, since sheet
   3 is continuous behind sheets 1–2. Only the pupil is real.
5. Pupil: **Push back** lands it on sheet 3, where it reads light, not dark:
   wrong, and the hover preview shows it. So B at 2.5 mm on sheet 1 enlarges
   the pupil upward until it merges with the eyelid line: an overlap, which
   reads as a heavy-lidded eye, not as a strut. Brush size will not go below
   Min. width.
6. "Weak contact 1.4 mm": the tail meets the shell by a sliver. **Thicken**.
7. Single view, 1 to 4. Sheet 1 (outline) is the delicate one; the Issues
   list is empty. Export.

Problems: anti-aliasing (exact snap); depth order is a design decision, not
luminance (drag palette rows, map a colour to Backing); a dark detail
enclosed by light (grow it into the nearest dark mass so the contact is part of the drawing).

Insight: flat art already has its colours; k-means damages it. Rules: Exact
colours when an image has at most eight flat colours; the user owns depth
order; Push back is previewed because it changes the colour a piece reads as.

### 4.6 AI first: "reimagine as shadowbox"

This script shows the route without a reasoning model: one drawing from a
template, then the user steering by review. With a director configured, Auto
runs steps 4 to 6 by itself (plan, draw, check, critique, refine) and the
review bar appears once, on the best round; the chips then go to the
director. See section 7.

Two photos: a child on a swing, and the garden. The user does not want to
edit, she wants a design.

1. Drop both. Arrange roughly. Bar: "These are photos. **Auto** or
   **Reimagine with AI**".
2. AI ▾ ▸ **Reimagine as shadowbox…**. The consent dialog: thumbnails of the
   composition and both references (three uploads, 610 KB), provider default.
   Instructions: "swing hanging from a big tree branch, flowers along the
   bottom". ▸ Full prompt shows the N-tone, touch-the-border rules Offcut
   adds. **Send**.
3. Bar: "Waiting for GPT Image 2.6 sunburst… Cancel". The document stays usable.
4. Result: a new item "AI illustration" at the top of Artwork, sources
   unticked, already snapped to five tones and validated. Versions: 0
   (composition) · 1 ●7 ⚑. The canvas shows it with red hatch on the child's
   shoes, two flowers and a bird. Review bar: "Result 1 · 7 issues · 38 s
   Does this work as a shadowbox?"
5. The tree is good; the flowers hang in mid-air on stalks that look like
   props. Support ▾ ▸ **Looks added on**, then Support ▾ ▸ **Pieces still
   float**. The bar now shows the pending instruction with **Send**. Send: no
   dialog, since nothing new is uploaded. The seven hatched areas go as markup.
6. Version 2 ●2: flowers now grow from a grassy bank along the bottom edge;
   the bird sits on the branch. But the child's face has changed. Likeness ▾
   ▸ **Doesn't look like the subject**. Version 3 ●4 ▲: likeness back, but
   the drift mark shows two new issues. Click version 2 again, **Write a
   review…**: add "keep everything, only restore her fringe and ponytail",
   **Mark** the head, Send. That is a branch: version 2a ●2. **Use this**.
7. Remaining two: a shoe (**Push back**, fine) and "Too thin" on the swing
   ropes at 0.9 mm. **Thicken** makes them 2 mm: acceptable, they are ropes
   and part of the scene.
8. Tilt, export. The project keeps every version with its prompt, the review
   given and the prompt-file versions.

Problems: provider not configured (dialog body offers Settings); invalid AI
output (always validated; badges make versions comparable); supports that
look added on (the commonest complaint, so it is the first chip); likeness
drift under refinement (original resent as reference; drift mark; branching
from the last good version); tone drift (Exact snap); privacy (thumbnails of
every first upload).

Insight: AI output is a draft with defects, results vary run to run, and
refinement can make things worse. Rules: a thread with issue badges and a
drift mark; a review bar whose answers are the next instruction; validation
findings ride along automatically; the same Colours and Issues machinery
applies to AI art as to anything else.

### 4.7 Sketch scenery for a floating subject

A heron (cut-out photo, three tones) stands in the lower left; a small bird
from the same photo is in the upper right, touching nothing.

1. Arrange. The bird is hatched red (forecast). Bar: "The bird won't be
   held. **Push back**". No neighbour is within reach, so Extend and Move
   onto are not offered. Hovering Push back shows the bird turning pale and
   nearly disappearing. Not wanted.
2. Piece properties ▸ **Sketch scenery**. A Sketch item "scenery" is created
   on the bird's sheet; K is active; the hatch stays lit. Tool row: Pen,
   Size 3 mm, Tone darkest.
3. Draw a rough bough coming in from the right border and passing under the
   bird's feet, with a few scribbled leaf clusters. With a pen, width follows
   pressure. The strokes show at 70 % over the scene. Once the bough touches
   both the border and the bird, the hatch turns green.
4. **Use as drawn** would do: strokes widen to Min. width and the bird is
   held. It looks like a scribble, though. Ctrl+Z.
5. Notes for AI: "an alder branch with catkins". **Refine with AI…**. Send to
   AI shows two thumbnails: the strokes, and the scene without them. Full
   prompt: the brief, then the sketch task (keep placement, extent and
   intent; flat shapes in the scene's tone; join the frame and the shapes it
   overlaps), then the note. Send.
6. Version 1 ●0 arrives in the review bar: a clean branch with catkins, but
   it has grown across half the sky. Detail ▾ ▸ **Too detailed for card**,
   then Write a review…: "keep to where I drew". Version 2 ●0. **Use this**.
   Version 0 is still the strokes.
7. Sheets: the bird is held by the branch. No scaffold marker, because a
   sketch item is artwork, not a drawn support.

Problems: Push back changes how the piece reads (hover preview); a rough
sketch looks rough (AI refine); the model over-elaborates (chips; the task
pins extent to the strokes).

Insight: the user knows what should hold the piece and where, but cannot
draw it cleanly. Rules: a sketch states intent in place; the model makes it
card in the scene's style; strokes are kept as version 0; a sketch is an
item, so it flows through Colours and Sheets like everything else.

### 4.8 Repair after a test cut

The Mochi project from DESIGN.md was cut. The tail support drooped and the
whiskers tore on the mat.

1. Open `mochi.offcut`. Lands in the stage it was saved in (Sheets), bar
   "Ready to export".
2. Shadowbox (click empty canvas) ▸ Min. width 2.0 → 3.0 mm. Validation
   re-runs. Bar: "5 issues". Issues list: whiskers ×4 "Too thin 1.6 mm", tail
   support "Weak contact 2.2 mm".
3. Whiskers: select all four rows (Shift-click), **Remove**. They merge into
   the cheek.
4. Tail: the grass tuft was drawn as a Branch support (its row shows the
   scaffold marker). V, click it; drag the width handle to 5 mm and the lower
   end 8 mm further into the border. Issue clears. The marker stays, a
   reminder that the tuft could become Sketch scenery next time.
5. Ctrl+E. Cut list: Sheet 1 ☑ changed, Sheet 2 ☑ changed, 3–5 unchanged.
   Tick "Only changed sheets". Export two SVGs.

Insight: iteration after a physical test is small and local. Rules: Min.
width is a live document setting that re-validates; drawn supports remain editable
objects and stay marked as scaffold; export tracks what changed since the last export.

### 4.9 Director plans: two filled examples

The taco tortoise (reference in four colours; run 05 measured nine tones,
bold contours, 26 floating pieces):

```json
{ "plan_version": 1, "route": "generate",
  "summary": "Tortoise on a grassy bank; outlines dissolved into fills; fillings merged; spots and eye are holes.",
  "style": { "notes": "flat shapes meeting edge to edge, no outlines" },
  "sheets": [
    { "id": "s1", "depth": 1, "card": "#2b1a12", "role": "front", "holds": "border, bank and tree" },
    { "id": "s2", "depth": 2, "card": "#5a9e3a", "role": "mid", "holds": "tortoise skin, lettuce, grassy bank" },
    { "id": "s3", "depth": 3, "card": "#f2b632", "role": "mid", "holds": "shell, fillings" },
    { "id": "s4", "depth": 4, "card": "#f6efd2", "role": "backing", "holds": "sky, eye whites, toes" } ],
  "features": [
    { "id": "contours", "label": "outlines", "source": "reference", "keep": "drop",
      "attention": "dissolve every outline into the fill on either side", "evidence": ["m1"] },
    { "id": "body", "label": "head, legs", "source": "reference", "keep": "keep",
      "arrangement": { "kind": "sheet", "sheet": "s2", "through": ["s1"] },
      "support": { "by": "overlap", "with": "bank", "contact_mm": 25 }, "evidence": [] },
    { "id": "bank", "label": "grassy bank with three tufts", "source": "invented", "keep": "keep",
      "arrangement": { "kind": "sheet", "sheet": "s2", "through": ["s1"] },
      "support": { "by": "frame", "with": "left, right, bottom", "contact_mm": 180 }, "evidence": ["m4"] },
    { "id": "shell", "label": "taco shell", "source": "reference", "keep": "keep",
      "arrangement": { "kind": "sheet", "sheet": "s3", "through": ["s1", "s2"] },
      "support": { "by": "not_needed", "with": "s3 is continuous behind", "contact_mm": 0 }, "evidence": [] },
    { "id": "fillings", "label": "tomato, cheese, beans", "source": "reference", "keep": "simplify",
      "simplify_to": "four large lumps in shell colour", "attention": "red and brown are dropped as colours",
      "arrangement": { "kind": "merge", "sheet": "s3", "through": ["s1", "s2"], "merge_with": "shell" },
      "support": { "by": "not_needed", "with": "", "contact_mm": 0 }, "evidence": ["m2"] },
    { "id": "spots", "label": "shell spots", "source": "reference", "keep": "simplify",
      "simplify_to": "six spots, each over 4 mm",
      "arrangement": { "kind": "hole", "sheet": "s4", "through": ["s3"] },
      "support": { "by": "not_needed", "with": "", "contact_mm": 0 }, "evidence": ["m3"] },
    { "id": "eye", "label": "eye white and highlight", "source": "reference", "keep": "simplify",
      "simplify_to": "one white shape; pupil a dark shape touching the head's edge",
      "arrangement": { "kind": "hole", "sheet": "s4", "through": ["s1", "s2", "s3"] },
      "support": { "by": "not_needed", "with": "", "contact_mm": 0 }, "evidence": ["m3"] } ],
  "scene_directions": [
    "The tortoise stands on a grassy bank that fills the bottom third and runs off both sides.",
    "Plain cream sky. No sun, clouds or stars.",
    "Flat coloured regions only: shapes meet edge to edge with no outlines or contour lines.",
    "The shell's spots are cream, not dark. Fillings are four big yellow lumps." ],
  "avoid": ["outlines", "red", "brown", "thin grass blades", "loose dots"],
  "local": { "sheet_count": 4, "palette": [], "ops": [] },
  "splits": [], "risks": ["pupil may still float; critic to check"] }
```

A pet photo (the spaniel of 4.1: cut-out, placed by Touch bottom; the
forecast measured 63 unsupported pieces, most under 10 mm², and fur speckle
in every tone). Here the director decides the source is not worth editing in
place, so `route` is `generate`, and it uses fewer sheets than the five the
document allows:

```json
{ "plan_version": 1, "route": "generate",
  "summary": "Head-and-chest portrait of the spaniel rising from the bottom border, ears overlapping the side borders; four sheets; eyes and nose joined to the dark ear mass; whiskers dropped.",
  "style": { "outline": "none", "notes": "large flat shapes; fur as three or four scalloped edges, not strands" },
  "sheets": [
    { "id": "s1", "depth": 1, "card": "#2a211c", "role": "front", "holds": "border, ears, eye surrounds, nose, mouth line" },
    { "id": "s2", "depth": 2, "card": "#8a5a36", "role": "mid", "holds": "head and chest mid-tone" },
    { "id": "s3", "depth": 3, "card": "#d9b98c", "role": "mid", "holds": "muzzle, blaze, chest bib" },
    { "id": "s4", "depth": 4, "card": "#f4ead8", "role": "backing", "holds": "background, eye highlights" } ],
  "features": [
    { "id": "ears", "label": "both ears", "source": "reference", "keep": "simplify",
      "simplify_to": "two solid shapes with a scalloped lower edge",
      "arrangement": { "kind": "sheet", "sheet": "s1", "through": [] },
      "support": { "by": "frame", "with": "left and right borders", "contact_mm": 20 }, "evidence": ["m1"] },
    { "id": "eyes", "label": "eyes with dark surrounds", "source": "reference", "keep": "keep",
      "attention": "likeness lives here: keep spacing and tilt",
      "arrangement": { "kind": "sheet", "sheet": "s1", "through": [] },
      "support": { "by": "overlap", "with": "ears, through a dark brow band", "contact_mm": 4 }, "evidence": ["m5", "m6"] },
    { "id": "highlights", "label": "eye highlights", "source": "reference", "keep": "simplify",
      "simplify_to": "one dot per eye, at least 2 mm",
      "arrangement": { "kind": "hole", "sheet": "s4", "through": ["s1", "s2", "s3"] },
      "support": { "by": "not_needed", "with": "", "contact_mm": 0 }, "evidence": ["m7"] },
    { "id": "nose", "label": "nose and mouth line", "source": "reference", "keep": "simplify",
      "simplify_to": "nose and a short mouth line as one shape",
      "arrangement": { "kind": "sheet", "sheet": "s1", "through": [] },
      "support": { "by": "overlap", "with": "a dark stripe down the muzzle to the brow band", "contact_mm": 3 }, "evidence": ["m8"] },
    { "id": "head", "label": "head and chest", "source": "reference", "keep": "simplify",
      "simplify_to": "one mid-brown mass",
      "arrangement": { "kind": "sheet", "sheet": "s2", "through": ["s1"] },
      "support": { "by": "ground", "with": "bottom border", "contact_mm": 90 }, "evidence": ["m2"] },
    { "id": "blaze", "label": "muzzle, blaze and bib", "source": "reference", "keep": "keep",
      "arrangement": { "kind": "sheet", "sheet": "s3", "through": ["s1", "s2"] },
      "support": { "by": "ground", "with": "bib runs into the bottom border", "contact_mm": 40 }, "evidence": [] },
    { "id": "whiskers", "label": "whiskers", "source": "reference", "keep": "drop",
      "arrangement": { "kind": "merge", "sheet": "s3", "through": [], "merge_with": "blaze" },
      "support": { "by": "not_needed", "with": "", "contact_mm": 0 }, "evidence": ["m9"] },
    { "id": "collar", "label": "collar and tag", "source": "reference", "keep": "simplify",
      "simplify_to": "a plain band; no tag",
      "arrangement": { "kind": "merge", "sheet": "s1", "through": [], "merge_with": "ears" },
      "support": { "by": "overlap", "with": "ear tips at both ends", "contact_mm": 6 }, "evidence": ["m10"] },
    { "id": "sofa", "label": "sofa and room", "source": "reference", "keep": "drop",
      "arrangement": { "kind": "merge", "sheet": "s4", "through": [], "merge_with": "" },
      "support": { "by": "not_needed", "with": "", "contact_mm": 0 }, "evidence": ["m3"] } ],
  "scene_directions": [
    "A head-and-chest portrait of the spaniel from the photo, facing the same way, filling the frame.",
    "The chest runs off the bottom edge; each ear hangs past the side edge.",
    "A dark band across the brow joins both ears and both eyes; a dark stripe runs from it down to the nose.",
    "Plain pale background, nothing in it.",
    "Fur is shown only by scalloped edges where two colours meet." ],
  "avoid": ["whiskers", "individual hairs", "collar tag", "furniture", "outlines"],
  "local": { "sheet_count": 4, "palette": [], "ops": [] },
  "splits": [], "risks": ["the brow band can read as a mask; critic to compare likeness with the photo"] }
```

Had the user chosen Local only, or Arrange for me, the same analysis would
return `route: "local"` with `splits` (dog, sofa, wall as wand seeds),
`local.palette` and ops such as `show_through` for the highlights, and no
drawing.

## 5. Rejected alternatives

| Alternative | Why not |
|---|---|
| A wizard (Next/Back) | The fix for a late problem is usually early. A wizard makes going back feel like failure and hides the stages' live coupling. |
| Draft's five-step bar | See section 9. Two of the five steps have no canvas interaction of their own. |
| No stages; one canvas with view toggles (today's app, grown) | Clicking would be ambiguous: is this a pixel, a region or a piece? Stages answer that. |
| Left tool palette | Costs a column for six buttons; the options row exists regardless. Revisit only if the tool count doubles. |
| Fixed palette of all eight tools with the inapplicable ones disabled | Considered for positional stability. Rejected in favour of a fixed order with only live tools shown: fewer dead buttons, and each tool still sits in the same relative place. |
| Derived layer per operation | Clutter, and edits to a derived layer die when its input changes. |
| Mask layers the user edits directly | Same; and it invites pixel-fiddling on sheets instead of fixing causes. Overrides give the same power without the fragility. |
| Bézier pen | Unlearnable for this audience; precision not needed. |
| Auto-bridges | Never acceptable to the product owner, and they hide the real problem, which is composition. |
| The Support tool as the main answer to floating pieces (this study's first version) | An ideal shadowbox has no scaffolds. Scene-first fixes come first; Branch and Shape stay as a labelled fallback, and what they hold is marked. |
| Sketching as a Sheets-stage override | A sketch is artwork with intent, and it needs versions, Notes and review. As an item it gets all of that and re-derives like everything else. |
| A chat pane for AI iteration | A second workspace. The thread lives on the item; the review bar is the conversation. |
| Review as a modal dialog | It would cover the result being judged. The bottom bar leaves the canvas, sheets and tilt view usable while deciding. |
| Editable full prompt | Lets users delete the constraints that make results cuttable. Read-only view plus three levels of user text. |
| Send dialog on every follow-up | Follow-ups in a thread upload nothing new; a dialog per chip would kill iteration. First uploads are always shown. Listed as an open question. |
| Floating view overlay on the canvas | Needs widgets over a canvas, or hand-painted buttons without accessibility. The bar already exists. |
| Modal dialogs for Reduce and Remove background | Block the canvas they preview on. Properties sections are live and need no popover. |
| "Don't ask again" on AI upload | Uploads may be family photos; one extra click per request is the right cost. Left as an open question for per-project suppression. |
| Tool key switches stage (draft) | The same key acts on different things per stage; a jump the user did not ask for is disorienting. The bar says where the tool lives instead. |
| A separate Export stage | Nothing to do on its canvas. The cut list fits a dialog, and the assembled and tilt previews belong where problems can still be fixed. |

## 6. Risks to watch

- **Override alignment**: tone overrides are in frame coordinates. Moving an
  item after drawing supports can orphan them. The design reports it; if it
  proves common, anchor overrides to the nearest item.
- **Region edits versus Reduce parameters**: changing N after wand edits
  invalidates them. Warn with a count and rely on undo; do not try to replay.
- **Weak-joint detection cost**: erosion-based checks on a 640 px canvas are
  cheap; they must stay background jobs with cancellation as resolution grows.
- **Tool row width**: no toolbar overflow in Rapport. Keep to six tools and
  three actions; everything else is in Properties and context menus.
- **Text**: needs glyph outlines at working resolution. Without gap 10 this
  is Windows-only DirectWrite code inside Offcut.

## 7. The AI director: evidence and reasoning

### What the first live runs showed

Three runs through Pollinations with microsoft/mai-image-2.7 (build folders
`live-ai-04-taco`, `live-ai-05-taco-instr`, `live-ai-06-dman`; this study
looked at the 05 and 06 responses and the 05 preview, and the result lines
of all three):

- **Directions are followed.** Run 05 added "the sky is plain" and "the
  turtle stands on a grassy hill that runs off both sides of the frame". The
  drawing has exactly that, with grass tufts nobody asked for that suit it.
- **Rules are not.** The same prompt said "exactly 4 flat colours with no
  outlines" twice. The drawing has heavy dark contours and about nine
  colours. Run 06 produced clock numerals, sparkles and stars far finer than
  card, and floating shapes in the sky.
- **Offcut then hid the damage.** The import forced four tones by clustering
  and reported "Removed 26 floating pieces" (84 for run 06). The preview
  looks plausible only because the lost detail is gone without trace.
- A drawing takes about 50 seconds.

Two conclusions shape the design. First, the prompt has to be written about
this picture: what stands on what, what is dropped, which colours exist.
Template text cannot do that; something has to look. Second, some habits have to be
undone locally, whatever the prompt says. Bold contours are the main one, and
they can't be kept as a "contour web" front sheet: a shadowbox's look comes
from depth, each shape's edge being the edge of its own card, so lacy
outlines on a separate sheet would sit at the wrong depth and break the 3D
effect (owner's decision). Offcut dissolves them into the neighbouring fills.
(Rejected: contour web as the front sheet.)

### Why a director, and why this split

| Question | Decision | Why |
|---|---|---|
| Who decides pass or fail? | Offcut's measurements | A vision model cannot count connected components reliably; flood fill can. The director explains and proposes, it never certifies |
| Who writes the image prompt? | Offcut compiles it from the plan | The plan is inspectable and editable; a prompt written freehand by a model is neither. Compilation also keeps the output spec constant |
| Why a structured plan? | JSON against a schema | It can be shown as a table, edited field by field, diffed between rounds, applied locally without drawing, and validated before any money is spent on an image |
| Why measurements as input? | Numbered findings plus an overlay | The director reasons about real problems ("m7: 38 mm² piece enclosed by sky, 41 mm from the nearest card") instead of guessing from pixels |
| Why a critic step? | Same model, second call | The first runs show the illustrator misses constraints every time. The critic turns findings into the next concrete direction, or into local ops that cost nothing |
| Why local ops first? | They are free, instant and exact | "Make the sun a hole to the backing" is one region reassignment. Asking the illustrator to redraw risks everything else changing |
| Why best-of, not last? | Present the best-scoring round | Refinement drifts; run 2 can be worse than run 1 |
| Why one consent per run? | The loop cannot stop to ask | The dialog lists every role, host and image up front, and the ceiling on calls |

### Arrangement: sheet, hole or merge

The owner's point is that separating layers is an arrangement problem. Three
moves cover it, and the plan must choose one per feature:

- **Own sheet**: costs a sheet and needs support. Right for large features
  and for anything that must read as nearer.
- **Hole**: the feature is an absence. A sun in a pale sky becomes a hole in
  the sky sheet showing yellow backing card; an eye highlight becomes a hole
  through to the backing. Holes cannot float and cost nothing, but they read
  in the colour of what is behind, so the director also chooses card
  colours, the backing's included. The one hazard is an island inside a
  hole, which the forecast catches.
- **Merge**: the feature stops being separate. Right for tonal variation and
  for detail finer than card.

This is why Show through becomes a first-class local operation and why depth
order must not be tied to lightness.

### Roles and the ChatGPT subscription route

Planning and drawing are separate roles because the providers that are good
or affordable at each differ. OpenAI's "Sign in with ChatGPT" preview makes
this concrete: it runs requests against a ChatGPT Plus or Pro plan through
`/v1/responses`, accepts image input, and cannot generate images. So it can
direct and cannot draw. Paired with a free image model it gives a complete
loop with no API credit, which is the owner's situation. The consequences
are in the design: every director call is stateless (that route has no
`previous_response_id`), usage-limit errors stop the run and keep the best
round, and Settings shows what each entry can do. The mechanics of that
route are as reported by the owner from OpenAI's announcement; they were not
tested here.

### Rejected

| Alternative | Why not |
|---|---|
| Longer, stricter rule lists in the image prompt | Tried in effect by run 05; ignored |
| Let the director call the image tool itself and return a finished picture | No local check between turns, no inspectable plan, and it ties both roles to one provider |
| Ask the director whether the result is valid | It would be guessing at connectivity. Measurements decide |
| Free-text plan | Cannot be edited as a table, applied locally or validated |
| A separate "AI mode" or wizard | The owner wants it to feel like Auto. It is Auto |
| Show every intermediate round for approval by default | Not seamless. It is the "Ask after each round" setting |
| Silent local fixes | Each round's ops are one named undo step and are listed in the plan dialog |

## 8. For the phase 1 implementation

Adjust now, in what phase 1 is building, so the director slots in:

- **Prompts**: assemble through one function that takes either a plan or a
  template. Put concrete scene directions first and the output spec last;
  cut the abstract rules sent to the illustrator to three lines.
- **Brief**: keep `brief.txt` whole, addressed to a designer (the director
  will read it); forbid outlines (shapes meet edge to edge); Offcut also dissolves any that appear.
- **Consent dialog**: lay it out as rows of role · entry · model · what it
  sees, with Until and Rounds, even while there is one role.
- **Review bar**: room for a one-sentence explanation and **Show plan**;
  record chips as ids, not only text; "Try again" means same plan.
- **Versions**: each record takes round, run id, score, measurements, plan,
  verdict and applied ops; the strip groups by run.
- **Provider entries**: the planned "chat model" field becomes "Reasoning
  model" on every entry type; an entry may lack either model; the single
  default becomes two (plans with, draws with). Sign in with ChatGPT is a
  reasoning-only entry, which lets the owner test the director without API
  credit.
- **Import**: snap to a supplied palette, and report tones found against
  tones wanted instead of forcing N by k-means alone.

## 9. Changes from the DESIGN.md draft

| Draft | This design | Why |
|---|---|---|
| Five steps | Three stages + Export dialog | Separate is one conversion and Export has no tools |
| Left tool palette, changing per step | Tools in the tool row | Six live tools do not earn a column |
| A tool key switches step | Never | The same key acts on different things per stage |
| A derived layer per operation | One row per item; live settings; AI results as a version thread | Crafters manage things, not history |
| Editable Mask layers from Separate | Sheets derived; edits are overrides | Upstream changes must not destroy work |
| Operations in the context bar; Reduce dialog | Live Properties sections | Parameters and preview belong together |
| Remove background in Simplify | In Arrange | Placement depends on the silhouette |
| Path tool for scaffolding | Scene first: move, push back, AI, sketch scenery; Branch/Shape as labelled fallback | An ideal shadowbox has no scaffolds |
| Floating pieces flagged in Support | Per-pixel hatch, forecast from Arrange onward | Cheapest fixes are earliest |
| AI: one prompt, upload reviewed, result imported | Auto as a director loop: measure, plan, draw, check, critique, fix; an inspectable plan; review at the end | Image models follow directions, not rules; arrangement needs reasoning about this picture |
| Strip above the canvas, status bar, view overlay | One bottom bar (guidance or review) | Fewer bars; no widget overlays in Rapport |
| New project dialog | Drop creates the project | Everything re-derives |
| – | Sketch items and tool; Accept; Fix all; hover preview | Intent no image has; glued pieces are legitimate |

## 10. Rapport API sketches

For the gaps in ux-design.md section 13.

```cpp
L::segmented{{"arrange", "Arrange"}, {"colours", "Colours"}, {"sheets", "Sheets"}}   // 1
    .current("arrange").bind(stage_).on_changed([this](const std::string& id) { set_stage(id); });
stage_->set_badge("colours", rap::badge::attention);            // none | done | attention

rap::path p;  p.move_to(a); p.cubic_to(c1, c2, b); p.close();   // 2–4
painter.fill(p, paint, rap::fill_rule::even_odd);
painter.stroke(p, paint, {.width = 1, .dash = {4, 4}, .dash_offset = t, .join = line_join::round});
painter.push_transform(rap::affine::rotation(angle, centre));  painter.pop_transform();

L::list{cols, &model}.on_cell_click(                            // 5
    [](size_t row, size_t col, point_f in_cell, const pointer_event& e) { return true; });
L::tool_button{wand_icon(), "Wand (W)"}.radio("tool");          // 6
L::button{"Looks added on"}.toggle().on_toggled(fn);
struct pointer_event { ...; float pressure = 1; pointer_device device; bool eraser; };   // 7
canvas_->capture_pointer(true);
L::canvas{}.on_files_dropped([](const std::vector<std::string>&, point_f at) {});   // 8
L::hbox{...}.surface(rap::surface::attention);  L::spin{.5, 10, 2, .5, 1}.suffix("mm");   // 9
auto s = ui.create_surface(w, h, pixel_format::rgba8);  s->painter().text(...);     // 10
```
