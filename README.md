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
The core also builds without Rapport: `-DOFFCUT_BUILD_APP=OFF`.

For a native integration check, run:

```sh
offcut --smoke-test path/to/reference.png build/smoke
```

This checks decoding, generation while a tab moves between windows, project
persistence, mask reimport and exports. It writes `result.txt` and a workspace
snapshot, then exits.

## First workflow

1. **New from image** creates a project; select multiple images to start a stack.
2. Add, reorder, hide or remove source images. The list is bottom to top.
   Images retain aspect ratio and are contained inside the shared frame.
3. Set the overall size, frame, layer count, smoothing and bridge width.
4. **Generate layers** creates cumulative tonal masks, a continuous frame,
   optional straight bridges to the frame, and a solid backing.
5. Inspect the assembled preview or select each cut layer. Boxes change preview
   visibility; every cut layer is included in export.
6. **Export SVGs** writes a new folder with numbered SVGs, black-on-white PNG
   masks, a preview and assembly notes. Layer 01 is the front.

Tabs reorder, detach into new windows and transfer between windows through
Rapport, following Expedition's interaction model. Document settings, sources,
cut layers, preview selection and zoom/pan travel with their tab.

`.offcut` projects embed working images and masks so reopening does not depend
on the original image paths. Source imports are reduced to at most 1024 pixels
on the longest edge; this is a bounded working copy, not archival image storage.
Flattening replaces the source stack; save first if you want to keep it.
Unsaved projects prompt before closing.

## AI-assisted route

Settings records the preferred AI app: ChatGPT, Claude or Grok. This version uses
a manual handoff, not account authentication or network generation:

1. Enter optional creative instructions.
2. Export an AI request folder containing the composition, references and a
   detailed prompt, or copy just the prompt.
3. Attach those images in your AI app. Request separate aligned masks, not a
   contact sheet. Black means retained card; white means removed material.
4. Import the returned images. Use zero-padded filenames in front-to-back order;
   imports sort by filename and require identical pixel dimensions.
5. Offcut thresholds the masks, adds the frame and bridges, and traces SVGs.

No credentials are collected and no images are uploaded by Offcut. Direct
provider adapters and supported sign-in flows are future work. They should
return the same layer-mask contract as local generation; tracing and physical
validation remain local. Rapport's OS secret store is available when a supported
provider connection requires credentials.

## Current limits

This is a functional foundation, not a finished automatic papercut designer.
Local generation separates luminance; it does not infer subjects or artistic
depth. Bridges use a simple shortest-to-frame rule and can cross artwork.
Minimum bridge width is enforced on generated bridges, not every original thin
feature. Connectivity is topological, not a guarantee of cardstock strength.

Masks use a maximum 640-pixel working canvas. SVGs are real closed vector paths
with holes, exact grid contours and redundant straight vertices removed.
EXIF orientation is not yet applied on import. Curve fitting,
topology-preserving contour smoothing, manual bridge editing,
crop/position controls, undo/redo, ZIP export, calibrated light simulation and
physical Cricut validation are still to come. The preview is flat artwork, not a
prediction of light transmission. Smoothing currently filters the raster before
thresholding; it does not fit Bézier curves.

## Foundations

Built on [Rapport](https://github.com/TurkeyMan/rapport), pinned as a submodule.
IfritView guides the separation between image data, transforms and the native
canvas view; Expedition guides the document-tab/window lifecycle. Offcut does
not import IfritView's full decoder, RAW-development or GPU pipeline.
