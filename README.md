# Offcut

**Make something of it.** A native Rapport workspace for turning images into
layered paper art, with room for engraving and other crafts later.

## Build

CMake 3.25+, a C++23 compiler, and Rapport's native platform backend.
Windows uses MSVC and the Windows SDK; no image-library installation is needed.
Linux needs GTK 3 or Qt 6, plus gdk-pixbuf for platform image decoding.

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

On Windows, run `build/bin/Debug/offcut.exe`. Image paths may be passed on the
command line. `OFFCUT_SNAPSHOT=path.png` captures the initial window and exits.
`OFFCUT_SETTINGS_PAGE=general|ai-providers` opens Settings at that page
(captured instead, with `OFFCUT_SNAPSHOT`); `OFFCUT_CONFIG_DIR` points Offcut at
another settings folder, for tests and snapshots.
The core also builds without Rapport: `-DOFFCUT_BUILD_APP=OFF`.

For a native integration check, run:

```sh
offcut --smoke-test path/to/reference.png build/smoke
```

This checks decoding, generation while a tab moves between windows, project
persistence, mask reimport, exports, a canvas drag with undo and redo, and
sheet selection that keeps the Assembled view. It writes `result.txt` and a
workspace snapshot, then exits.

`offcut --scene arrange|sheets out.png [images…]` puts a window into a state
with the pointer (two images moved, scaled and rotated; or the sheets with one
unticked and another highlighted), saves a screenshot and exits. With one image
it generates a second.

## The window

Following [docs/ux-design.md](docs/ux-design.md): a main toolbar with the stage
switch (**1 Arrange › 2 Colours › 3 Sheets**, each marked done ✓ or needing
attention •), a tool row, the canvas, a right panel with the list over
Properties, and a guidance bar along the bottom. There is no menu bar.

- **Main toolbar**: New, Open, Save · Undo, Redo · stage switch · Add images,
  Auto · AI, Export…, Settings.
- **Tool row**: in Arrange, Move (V) and Crop (C), the Move tool's Snap to
  opening option, and Touch bottom, Fit and Reset for the selected image; in
  Sheets, Auto. Its height never changes.
- **Right panel**: in Arrange and Colours, *Artwork*, the images front to back
  with a tick for visibility, a thumbnail, the name and the size in mm; drag to
  reorder, right-click for every image operation. In Sheets, *Sheets*, front to
  back with a preview tick, a thumbnail and floating-piece counts. Ticking and
  selecting never move the list. Properties shows the selected image (X, Y, W,
  H in mm, rotation, crop in source pixels, Touch bottom/left/right/top, Fit to
  frame, Reset), the selected sheet, or with nothing selected the Shadowbox
  settings: frame, borders, sheets, smoothing, spacer gap, Min. width, where
  new images start, Remove floating pieces and AI instructions.
- **Guidance bar**: what is next or wrong, with one action ("Add an image to
  start", "Arrange your images, then Auto makes the sheets", "Design changed:
  make the sheets again before exporting", "Sheet 2 has 3 floating pieces",
  "Ready to export"); transient messages for five seconds; Cancel while an AI
  request runs; frame size and sheets; in Sheets the Assembled and Single sheet
  views; and zoom.

## First workflow

1. **New project from images** (Ctrl+N) opens frame setup before creating a
   project; dropping pictures on an empty window creates one at once. Defaults
   are 8×10 inches, the first image's orientation and a 10 mm border.
2. **Arrange.** Each image is its own layer with a position, scale, rotation
   and crop. A new image starts where **New images** in the Shadowbox settings
   puts it: **Fit whole image**, **Fill opening**, or **Touch edges**, which
   places the detected subject against the chosen opening edges, overlapping
   them by Min. width. Transparent or plain-background subjects default to
   Bottom contact; other images to Fit. An image's menu re-applies any of them.
   On the canvas, click an image (or its row) to select it; drag to move;
   drag a corner to scale with the aspect kept; drag the handle above the top
   edge to rotate (Shift: 15° steps). C or a double-click crops: drag the edges,
   Enter applies, Esc cancels. Edges snap to the opening within 3 mm, with a
   guide line; hold Alt to place freely. Arrow keys nudge by 1 mm (Shift: 10).
   Pan with the middle button, Space-drag or a drag on empty canvas; the wheel
   zooms at the pointer.
3. Set the frame size, linked or separate borders, sheet count, smoothing and
   spacer gap. **Min. width (mm)** controls how far Touch sinks a subject into
   the border and the minimum feature width requested in the AI prompt.
4. **Auto** (Ctrl+G) composes the images through their transforms, quantizes
   depth tones and builds sheets with limited hidden overlap, a continuous
   frame and a solid backing, then opens Sheets. **Remove floating pieces**
   discards card that cannot reach its sheet's frame; off, it stays and shows
   red in Sheets. Specks are always removed.
5. **Sheets** shows the ticked sheets stacked; the selected sheet is outlined and
   the others dimmed. Ticks change only the preview; every sheet is exported.
   **Single sheet** (or 1–9, Page Up/Down) shows one sheet alone; 0 goes back.
6. **Export…** (Ctrl+E) shows a preflight and the cut list, then writes a new
   folder with numbered SVGs, black-on-white PNG masks, a preview and assembly
   notes. Sheet 01 is the front.

Document tabs sit in the title bar and reorder, detach and transfer between
windows, following Expedition's interaction model. Each tab keeps its own
document, selection, stage, zoom and undo history.

**Undo** (Ctrl+Z; redo Ctrl+Y or Ctrl+Shift+Z) covers moving, scaling,
rotating and cropping images, adding, removing, reordering, hiding and
flattening them, every Shadowbox setting, Auto, imports and AI results. A drag
or typing into a field is one step. The history holds 100 steps per document;
selection, zoom and stage switches are not steps. The document is unsaved
exactly when it differs from its last saved step.

Shortcuts: Ctrl+N/O/S/W create/open/save/close projects; Ctrl+T opens a blank
tab. Ctrl+I/G/E add images/Auto/Export. Ctrl+1/2/3 switch stages, Ctrl+]/[
bring an image forward or send it back, Ctrl+=/−/0 zoom in/out/fit, and Ctrl+,
opens Settings. V and C pick Move and Crop; Delete removes the selected image.

`.offcut` projects embed working images and masks so reopening does not depend
on the original image paths. Imports are reduced to at most 1024 pixels on the
longest edge; this is a bounded working copy, not archival image storage.
Version 3 projects (`OFFCUT03`) store each layer's type, id, the layer it was
made from, and its transform (position and scale in mm, rotation, crop) exactly.
Version 2 and 1 projects open looking as they did: their composite placement
becomes each image's transform. Flattening replaces the image stack with one
image covering the opening; Ctrl+Z brings the stack back.

## AI-assisted route

On Windows, **Settings → AI providers** lists the providers Offcut can use, as
Tortilla lists its forge accounts: Name, Provider, Model and Status. Opening the
page checks each one on a worker by listing its models with the stored key
(“Connected”, “Checking…”, or why not, such as “Key rejected (401)” or “No key in
Windows Credential Manager”); no images are uploaded. Select a row to change its
model, **Make default**, or **Remove** it (after confirming; this deletes its key
from Credential Manager). Several entries may use the same provider.

**Add provider** offers OpenAI, Pollinations, a local OpenAI-compatible server or
a custom OpenAI-compatible server. OpenAI and Pollinations take a key, with a
“Make a key on …” link to their key pages; local and custom servers take a URL,
and a custom server an optional key. **Add** first lists the models with that key
and shows the provider's own error if that fails (for OpenAI, a link to its
organization settings when verification is the problem). Only then is the key
stored, as `Offcut/AI/<id>`, and the row added with its image-capable models
cached: OpenAI's GPT Image models (newest chosen), Pollinations models that take
image input (`google/gemini-3.1-flash-image` preferred), or whatever a compatible
server lists (else type the model). Changes on this page apply at once.

OpenAI API usage is billed separately from ChatGPT, and some GPT Image models
need organization verification. Pollinations needs an `sk_` secret key, not a
`pk_` publishable key. Local servers send no key.

**AI → Reimagine as shadowbox…** asks which configured provider to use
(the default first), and shows its endpoint, model and size, the composition
dimensions/KB, the visible reference count/KB and the prompt. Only **Send**
uploads them. With no provider configured it offers to open Settings at AI
providers instead. The request runs in the background and can be cancelled.
Offcut saves PROMPT.txt, composition/references, response.png and response.json
(model, endpoint, elapsed seconds and usage) in a fresh offcut-ai-request-NN folder
beside the saved project, or in your chosen output folder, then imports the result.

Keys never enter projects, settings or request records, and are never shown or
logged. The list itself (names, URLs, models, credential target names) is kept in
`ai-accounts.txt` in Offcut's settings folder. On first use Offcut migrates older
settings into the list without reading any key: `ai-providers.txt` profiles that
were connected or whose key is still saved, or `ai-endpoint.txt`, or a key saved
as `Offcut/OpenAI`; migrated entries keep their original credential targets.
`OPENAI_API_KEY` is no longer read: add the key on the AI providers page.
GPT Image edits are multipart with `image[]` for the composition and references,
a size of 1024x1024, 1536x1024 or 1024x1536 matching the frame, quality `auto`,
PNG output and no `response_format`; redirects are refused.

The manual route remains available, with ChatGPT, Claude or Grok as preferred app:

1. Enter optional AI instructions in the Shadowbox properties.
2. Use **AI → Use my own AI app** in the toolbar to export the composition,
   references and detailed prompt, or copy just the prompt.
3. Ask your AI app for **one flat illustration with exactly N opaque colours**,
   one tone per sheet, darkest at the front and lightest at the back. The border
   uses the darkest tone. Avoid gradients, textures, extra outline colours and
   anti-aliasing; return the image itself, not a mockup or contact sheet.
4. Features sharing a tone share a sheet. Arrange them to touch each other or
   the border through the same or darker tones: a bird on a branch, lettering
   on the ground, a cloud meeting the top edge. Move, resize, overlap or add
   connecting elements; prefer contacts at least Min. width wide to point
   contacts or thin artificial bridges.
5. Choose **AI → Use my own AI app → Import flat illustration…**. Offcut clusters colours in
   OKLab, cleans the tone labels and builds all aligned sheets locally. An aspect
   mismatch is centre cover-cropped; the status reports significant cropping and
   floating pieces removed from each layer. Inspect the result, then export SVGs.

**Import sheet images…** remains an alternative for separate, already aligned
black-on-white masks. Black means retained card; white is removed. Use zero-padded front-to-back
filenames; these images must have identical dimensions and the project’s aspect.

Set **Layer gap (mm)** to your spacer thickness (default 5 mm). Local generation
and flat-illustration import use the same sheet builder: a sheet extends beneath
front card near its own visible regions, with reach proportional to depth at a
45° viewing angle. Hidden material is restored where needed to connect a sheet
to its frame; the final backing stays solid. Assembly notes include the gap.

Both routes use the same single-illustration import. Separation, physical checks
and SVG tracing remain local. Minimum retained-feature-width validation is still
to come. A consumer AI app login is separate from an image API key.

## Current limits

This is a functional foundation, not a finished automatic papercut designer.
Local generation separates luminance; it does not infer subjects or artistic
depth. Floating card is removed rather than supported by long thin connections
that look wrong and cannot carry detail. Deeper layers can reveal removed
detail where it connects. An isolated subject on a plain
background may leave frame-only layers: the status bar suggests touching the
edge or a ground, or using an AI rework. Touch edges can establish that contact;
subject detection uses alpha and a near-white or uniform border-colour heuristic.
Minimum width controls placement overlap and guides the AI prompt; it is not
validated as a minimum feature width locally. Connectivity does not guarantee strength.

Masks use a maximum 640-pixel working canvas. SVGs are real closed vector paths
with holes, exact grid contours and redundant straight vertices removed.
EXIF orientation is not yet applied on import. Curve fitting,
topology-preserving contour smoothing, manual layer repair,
per-item colour reduction, ZIP export, calibrated light simulation and
physical Cricut validation are still to come. The preview is flat artwork, not a
prediction of light transmission. Smoothing currently filters the raster before
thresholding; it extends artwork edge pixels through the frame first so blur
does not sever contact at the edge. It does not fit Bézier curves.

## Foundations

Built on [Rapport](https://github.com/TurkeyMan/rapport), pinned as a submodule.
IfritView guides the separation between image data, transforms and the native
canvas view; Expedition guides the document-tab/window lifecycle. Offcut does
not import IfritView's full decoder, RAW-development or GPU pipeline.
