# Offcut foundation

The first product is Tamara's custom shadow-box workflow: images in, a small
number of physically connected card layers out, at a chosen real-world size.
Photo engraving and other craft outputs can share source composition and layout
later without inheriting paper-specific constraints.

## Boundaries

- `project`: portable document state and bounded binary persistence: typed
  layers (Image now; Colour regions, Mask and Text reserved) with ids, the
  layer each was derived from, and per-image transforms; OFFCUT01–03.
- `transform`: image transform maths (frame ↔ source, bounds, hit testing,
  crop, scale, fit, touch, snapping), placement as initial transforms, CPU
  composition, tonal separation, connectivity and raster previews, including
  the stacked sheets with a highlighted sheet and floating pieces in red.
- `history`: snapshot undo of the document, sharing layer pixels between
  snapshots by id; `plan_rows` keeps lists in place across changes.
- `guidance`: the guidance bar's sentence, most important first.
- `export`: closed vector contours, physical SVG sizing, PNGs and AI request assets.
- `image_io`: Rapport's platform decoder into straight-alpha RGBA working images.
- `application`: windows, tab transfer and one background work queue.
- `editor_window`: the shell of docs/ux-design.md: title-bar document tabs, a
  main toolbar with the stage switch, a tool row, the canvas (`window_canvas`:
  drawing, handles and pointer/key editing), the right panel's lists and
  Properties (`window_parts`: list models, thumbnails, icons) and the
  guidance bar; no menu bar.
- `settings`: Tortilla-style Settings dialog with a page sidebar (General,
  AI providers), lazily built pages, OK/Cancel for the General page, and `show_settings(page)` for other code to open a page.
- `ai_providers_page`: the provider list (status checked on a worker with GET
  models only) and Add provider area. Adding and removing apply at once, as on
  Tortilla's Accounts page; keys go to Credential Manager, not to settings.
- `ai_provider`: the provider registry (OpenAI, Pollinations, local, custom),
  configured entries in `ai-accounts.txt` (nonsecret fields only), migration
  from the single-provider settings, model defaults, failure wording and the
  frame-matched image-edit endpoint for an entry.
- `ai_client`: WinHTTP multipart image edits, cancellable asynchronous handles,
  JSON/base64 helpers, typed HTTP/network errors and Windows Credential Manager
  (`Offcut/AI/<id>`; migrated entries keep `Offcut/OpenAI` and the like). Model
  parsing lists GPT Image models newest first and Pollinations image-input
  models. GPT Image edits use PNG, image[] for references, a supported size,
  quality auto and no response_format; redirects are never followed.
- `ai_workflow`: builds the composition and visible references, records request
  and response assets, then uses the manual import's shared illustration path.

Each tab owns a shared project independently of its window. Transform jobs take
a snapshot and publish results on the UI thread. Moving a busy tab does not
cancel or lose the result. Windows retire outside their own callbacks.
Source changes invalidate generated cuts and block stale SVG exports.

## Transform contract

Both local generation and flat-illustration import produce a shared tone index
map: darkest/front is 0. The import composites alpha over white, uses deterministic
luminance-quantile-seeded OKLab k-means, and cleans the labels with a small mode
filter. Aspect mismatches cover-crop at the centre; only indices are resampled,
with nearest-neighbour sampling, so alignment and the number of depth slots stay
under local control. Separate binary masks remain an alternative import.

`build_sheets` turns the common working-canvas map into binary cutting masks.
Each sheet retains its visible tone and frame. Hidden material is limited to
pixels whose nearest visible-or-deeper region belongs to this sheet, within
(depth difference × layer gap) at a 45° view. An exact Euclidean distance transform
keeps the mountain beneath the mountain-facing edge of a tree, not its sky-facing
edge. If this disconnects a component that the full cumulative sheet supports,
its cumulative component’s hidden pixels are restored. Added support never
changes visible regions. The backing remains solid; final settling counts removed
floating components. OFFCUT02 appends an optional gap field; older files default
to 5 mm. Models provide one illustration, never SVG; tracing is procedural.

Local generation remains the offline minimum. `frame_and_settle` adds the frame
and, with **Remove floating pieces** enabled, removes card that cannot reach it.
It never adds bridges: long thin supports spoil the artwork and cannot carry
card detail. Specks are always removed; switching the option off retains larger
floating pieces. Deeper layers can show removed detail where it connects. Isolated subjects can leave frame-only layers; the
status bar suggests contact with the edge or a ground, or an AI rework.

Before smoothing, artwork edge pixels fill the frame area so blur does not fade
edge contact and detach shapes. **Min. width (mm)** controls subject overlap into
the frame and the AI prompt; local minimum-feature-width validation is still to
come. The internal names
`options.connect` and `options.bridge_mm` remain for project-file compatibility.

## Frame and placement

The fixed outside size comes from a picture-frame preset or Custom dimensions,
with portrait/landscape orientation. Borders can be linked or independently set
on all four sides; one shared geometry defines composition, masks, previews and
flattening. The source preview tints the card border to show the opening.

Fit contains the whole image; Fill covers the opening. Touch edges detects each
source subject using alpha or a near-white/uniform border-colour heuristic. It
uses one scale, opposing-edge constraints and per-axis translations to overlap
selected opening edges by Min. width. Plain-background and transparent subjects
default to Bottom contact; other images use Fit.

Placement no longer applies to the composite: it sets the initial transform of
a newly added image (`place_image`), and an image's menu re-applies it. Each
Image layer keeps its own transform: the centre of its crop rectangle in frame
millimetres, millimetres per source pixel (uniform scale), a clockwise rotation
about that centre, and the crop in source pixels. `flatten` samples each
visible layer through its transform, nearest neighbour, over destination
pixels only. Recropping keeps the remaining pixels where they were. Touch
bottom/left/right/top moves an image at its current scale so its subject
overlaps that opening edge by Min. width. OFFCUT03 stores the transforms as
exact doubles; OFFCUT02 and OFFCUT01 files fix their project placement into
each image's transform as they load, so they look as before (OFFCUT01 as
uniform borders and Fit).

## Layer editor

Offcut is a small layer-based image editor with one purpose: turning source
images into a stack of self-supporting monochrome sheets. The original pipeline
(compose → tone map → sheets) is exposed as explicit, inspectable steps instead
of one hidden Generate.

### Document and layer types

A document is the frame plus a front-to-back stack of typed layers. Each type
has its own icon and operations.

| Type | Holds | Operations |
|---|---|---|
| Image | RGBA raster: photo or illustration | Position, scale, rotate, crop; remove background; reduce to N colours; AI rework |
| Colour regions | The image reduced to N flat colours plus a palette | Wand-select a region, then delete (transparent), recolour or merge; split into masks |
| Text | Editable lettering: font, size, weight, spacing, arc | Edit text and style; assign to a sheet; becomes a mask on Separate |
| Mask | One monochrome shape: card | Add or subtract with wand, brush or vector paths; assign to a sheet |
| Sheet (output) | The union of masks at one depth, plus the frame | Support check, overlap with the sheets behind, export |

Operations are non-destructive: each produces a derived layer and hides its
input, so the original is always one step away. Undo/redo covers every edit.

### Order of operations

1. Arrange: add images; position, scale, rotate and crop each in the frame.
   Placement modes (Touch edges etc.) set the initial transform of a new image;
   they no longer apply to the composite.
2. Simplify: remove backgrounds and reduce each image to N colours, locally
   (OKLab k-means) or by AI request ("remove the background and flatten to N
   tones").
3. Separate: split colour regions into masks. The wand deletes or merges stray
   regions; masks are assigned to sheets. Unrelated features sharing a colour
   share a sheet and must be arranged to touch.
4. Support: the sheets view shows problems in place rather than deleting them
   silently: floating pieces in red, necks under Min. width flagged. Fix with
   the drawing tools or an AI "fix support" request carrying the masks and the
   flagged issues.
5. Export.

A next-step strip above the canvas always states what is missing or wrong
("No sheets yet: split a colour layer into masks", "Sheet 3 has 2 floating
pieces"). It guides; it does not force a wizard. Auto runs steps 2–4 with
defaults and replaces today's one-click Generate.

### Tools

- Wand: on colour regions it selects the exact connected region (no
  tolerance); on images it uses a tolerance.
- Path: click to place points; the curve passes through them (Catmull-Rom),
  Alt-click makes a corner. Closing the path adds to the mask (subtract with
  Alt). Intended for scaffolding: branches, ledges, broad bridges.
- Brush and eraser for touch-ups.

### Workspace layout

Precedents, and what Offcut takes from each:

- Photoshop, Affinity Photo, Krita, GIMP: a vertical tool palette on the left
  with single-key shortcuts; a context bar across the top holding the active
  tool's options; docked Layers and Properties panels on the right. This is
  the arrangement image-editing users already know.
- Affinity's Personas and Lightroom's modules: a few named workspaces, each
  showing only the tools for that stage of the job, switchable at any time.
  This is what makes the order of operations visible without a wizard.
- Cricut Design Space and Silhouette Studio, the cutting tools Offcut's
  users know: a right-hand layers panel that groups and attaches by output,
  and a final "Make it" stage that lists what will be cut.
- Figma: properties that change with the selection, and a canvas-first
  window with little chrome.

The window, top to bottom:

1. Title bar with document tabs (unchanged).
2. Main toolbar: New, Open, Save | Undo, Redo | the step bar | AI and
   Settings at the right. The step bar is a segmented control: Arrange ›
   Simplify › Separate › Support › Export. Each step shows a state mark
   (done ✓, needs attention •, not started). Clicking a step switches
   workspace. Nothing is locked: any step can be opened at any time, and the
   next-step strip says what is missing.
3. Context bar: the active tool's options on the left (for example, the
   wand's Add/Subtract/Contiguous), and the selected layer's quick operations
   for this step on the right (for example, Remove background and Reduce
   colours in Simplify). It is empty when there is nothing to show; it never
   jumps in height.
4. Body:
   - Left, a narrow tool palette: icon buttons in a column with a tooltip and
     shortcut each. Its tools depend on the step. Hand (H, or hold Space) and
     Zoom (Z) are always at the bottom.
   - Centre, the canvas. The next-step strip runs across its top edge. View
     controls sit in a small overlay at the bottom-right (Original, Assembled,
     Single sheet, Tilt; Fit and zoom %).
   - Right, a dock with Layers above Properties, separated by a splitter.
     Support adds an Issues panel; Export replaces Properties with the cut
     list.
5. Status bar: transient messages, frame size, sheets, zoom.

Layers panel: front-to-back rows, each with type icon, thumbnail, name,
visibility tick and the sheet it goes to. Derived layers are nested under
their result with their inputs hidden (disclosure arrow). From Separate
onward, the panel groups masks under their sheet headers (Sheet 1 (front) …
Sheet N (back)); dragging between groups reassigns. A right-click menu holds
every operation valid for the layer's type, so nothing is only reachable
through one step. Ticking and selecting never move the list.

Properties panel: what the selected layer's type and the current step
need. Fields are aligned forms as elsewhere in the app; buttons for
operations live in the context bar, not here.

### Each step's workspace

| Step | Tool palette | Context bar (tool · operations) | Right dock | Canvas | Strip examples |
|---|---|---|---|---|---|
| Arrange | Move/transform (V), Crop (C), Text (T) | Move: snap to opening, aspect lock · Touch bottom/left/right/top, Fit, Reset. Text: font, size, weight, spacing, arc | Layers (images, text); Properties: X, Y, W, H mm, rotation, crop; text style | Original view; frame and opening outlined; snap guides; selection handles | "Add an image or text to start" · "MOCHI: I and C don't touch anything" |
| Simplify | Wand with tolerance (W), Eraser (E) | Wand: tolerance, contiguous, add/subtract · Remove background…, Reduce colours…, Flatten with AI…, Reimagine as shadowbox (AI)… | Layers; Properties: palette swatches dark→light, each with target sheet; region count | Selected layer over a checkerboard; others dimmed | "cat.jpg is still a photo: reduce it to colours" |
| Separate | Wand exact (W), Brush (B), Eraser (E) | Wand: add/subtract · Delete region, Merge, Recolour, Remove specks under N mm², Separate, Separate all | Layers grouped by sheet; Properties: selected region (area, colour) or mask (sheet, colour) | Colour regions, or the selected sheet's masks with the others dimmed | "2 layers not yet separated into masks" |
| Support | Path (P), Brush (B), Eraser (E) | Path: add/subtract, smooth/corner · Push back, Delete piece, Widen, Fix support with AI… | Layers by sheet; Issues list (sheet, kind, size); clicking an issue zooms to it | Single sheet or Assembled; red floating pieces, orange thin necks, dashed hidden overlap | "Sheet 1: nose and tail tip float" · "All sheets supported" |
| Export | Hand, Zoom only | Card size and colours, spacer thickness · Export SVG…, Export PNG references | Cut list: each sheet with thumbnail, size, piece count, suggested card colour; assembly order | Assembled with depth shading; drag to tilt | "Ready to export" · "2 issues remain in Support" |

Shortcuts work in every step. Pressing a tool's key in a step that doesn't
show the tool switches to the step that does; the step bar shows the
switch, so the user is never stuck. Ctrl+1–5 switch steps directly. Auto
(AI menu or Ctrl+G) runs Simplify → Support with defaults and lands in
Support.

Today's controls move as follows. The frame, border and placement fields go
to a Frame properties view, shown when nothing is selected in Arrange. The
layers, smoothing, layer gap and min. width fields go to Simplify and Support
properties. Generate becomes Auto. The old AI handoff menu becomes the AI
operations in each step's context bar, plus the AI menu for export/import of
manual requests.

### Walkthrough: a cat photo with lettering

The reference interaction. Someone has a photo of their cat, Mochi, and wants
an 8 × 10" shadowbox with "MOCHI" arched along the bottom. Tool shortcuts:
V move, C crop, W wand, T text, P path, B brush, E eraser.

**1. New project.** Ctrl+N opens New project: name, frame preset (8 × 10",
portrait), border (10 mm, or per side), number of sheets (5) and layer gap
(card + spacer). Create shows the empty frame. Strip: "Add an image or text
to start."

**2. Add the photo.** Drag cat.jpg onto the canvas (or toolbar +). An Image
layer appears in the layers panel, selected, fitted inside the opening, with
transform handles. Strip: "Arrange your images, then simplify them."

**3. Arrange.** Drag to move; corner handles scale with the aspect locked
(Shift frees it); the handle above the top edge rotates. C (or double-click)
enters crop: edge handles trim the image, Enter commits, Esc cancels. Edges
snap to the opening with a highlighted guide within 3 mm; the inspector has
"Touch bottom/left/right/top" buttons and exact X, Y, W, H (mm), rotation and
crop. The user sinks the cat's body into the bottom border so it will be
supported.

**4. Remove the background.** On the layer's menu (right-click, or the
inspector's Operations): Remove background… offers Local (colour similarity
flooded from the image edges, then wand and eraser touch-ups) or AI (provider
picker, upload review). The result is a new Image layer "cat — no
background" with the photo nested beneath it, hidden. The canvas shows the
cut-out over a checkerboard.

**5. Reduce colours.** Reduce colours… opens a dialog with a live preview:
Colours (default: sheets − 1, since the border is the darkest sheet), Smooth
(removes ragged edges), Minimum region (mm²; specks below it merge into a
neighbour). AI alternative: Flatten with AI. OK creates a Colour regions layer
"cat — 4 colours". The inspector shows its palette as swatches ordered dark to
light, each labelled with the sheet it will go to (darker = nearer the front);
dragging swatches changes that order.

**6. Clean up regions.** W, then click a region: it is selected exactly
(outline, area in mm², colour). Shift-click adds to the selection. Delete
makes it transparent; clicking a swatch recolours it (merging it into that
tone); M merges it into the neighbour it shares the longest edge with.
"Remove regions under N mm²" clears remaining speckle in one go. Typical
here: delete the stray background tones, merge the noisy fur patches.

**7. Add lettering.** T, click below the cat: a Text layer is created; type
MOCHI. The inspector edits font (system fonts, with stencil fonts listed
first), size (mm), weight, letter spacing, arc (bend %), and the sheet the
letters go to. Text stays editable until it is separated. Letters are solid
card, so counters (the holes in O, A) are holes in the card and need no
bridges, but every letter must touch something: the strip flags letters that
float ("I and the C don't touch anything"). Quick fixes in the inspector:
Tighten until touching, Sit on the border, or Add a ribbon (a banner shape
behind the word, on the same sheet). Here MOCHI overlaps the cat's paws and
the bottom border, so it is already supported.

**8. Separate into masks.** Separate (on the colour layer and the text layer,
or Separate all) converts them into Mask layers, one per tone, each assigned
to a sheet. The layers panel then groups masks under their sheets:
Sheet 1 (front) ▸ cat tone 1, MOCHI; Sheet 2 ▸ cat tone 2; … Sheet 5 (back) ▸
background card. Dragging a mask to another sheet reassigns it. Sheets are
cumulative as today (each sheet also holds the darker tones in front of it,
extended under them by the layer gap), so the user never draws that overlap
by hand.

**9. Check support.** The Sheets view (Ctrl+3) shows each sheet's final shape
with the frame. Problems are drawn in place: floating pieces in red, necks
and gaps under Min. width in orange, hidden overlap dashed. The strip lists
them: "Sheet 1: nose and tail tip float; Sheet 2: whiskers narrower than
2 mm". Clicking one zooms to it and offers fixes:
- Push back: drop the piece from this sheet so it appears on the first sheet
  behind where it is supported (the nose then reads one card deeper). This is
  the default auto-fix; today's silent removal becomes this, visibly.
- Delete the piece.
- Widen to Min. width (whiskers), or remove them.
- Draw scaffold: switch to the path tool on this sheet.
- Fix support with AI: send the sheets and the flagged problems; the reply
  becomes new mask layers for review.

**10. Scaffold.** P on Sheet 1: click points from the tail tip down to the
border, shaping a grass tuft; the outline passes smoothly through the points,
Alt-click makes a sharp tip. Clicking the first point closes the path and adds
it to a "scaffold" mask on that sheet (Alt-close subtracts instead). The red
marker on the tail clears.

**11. Preview and export.** The Assembled view shows the stack with depth
shading; dragging tilts it to show parallax. With the strip reading "Ready to
export", Ctrl+E exports SVG sheets, PNG references and assembly notes
(spacers, suggested card order). Exporting with problems still listed asks
first.

**Shortcut:** Reimagine as shadowbox (AI) on the photo at step 3 replaces steps
4–6 with one request that returns a Colour regions layer; the user continues
at step 7. Auto runs steps 4–9 locally with defaults.

### AI operations

AI requests are layer operations that return rasters, which become new
layers: on images (remove background, flatten to N tones, reimagine as a
shadowbox) and on sheets (fix support). The configured-provider picker is used
for each; uploads are always reviewed first.

### Phases

1. Typed layer model and layers panel; per-image transforms edited on the
   canvas and in the inspector; layer list that keeps scroll and selection;
   preview stacks the ticked layers and highlights the selected one;
   next-step strip; undo.
2. Simplify and separate: reduce to N colours as its own step, wand, split
   into masks, sheet assignment.
3. Sheets view with in-place support diagnostics instead of silent removal.
4. Text layers (with floating-letter checks and quick fixes), path tool,
   brush and eraser.
5. AI operations wired to each layer type.

## Next milestones

1. Extend the native OpenAI-compatible image-edit connection to other provider
   protocols as needed. Its explicit Send/Cancel review picks a configured
   provider and lists endpoint, model,
   prompt, composition size and reference count before any upload. Requests run
   on the background worker; cancellation closes the asynchronous WinHTTP request.
   Response records retain model/endpoint/time/usage, never credentials.
   Providers rework references into one flat N-tone illustration. Features sharing a tone share a sheet: request broad
   contacts with each other or the border through the same or darker tones.
   Models should produce the illustration, never SVG. Keep requests
   reviewable and uploads explicit, using supported authentication flows.
2. Extend local validation to minimum retained width as well as connectivity,
   then trace SVGs procedurally. Reject or request rework for invalid designs;
   retain local generation as the offline minimum.
3. Test Jay and the supplied examples with Tamara in Design Space and make small
   physical cuts. Add contour fitting that preserves topology, manual layer
   repair and undo as editing grows.
4. Add crop/placement, source transforms and richer colour simplification.
5. Add engraving output as a separate export/processing profile.
