# Offcut foundation

The first product is Tamara's custom shadow-box workflow: images in, a small
number of physically connected card layers out, at a chosen real-world size.
Photo engraving and other craft outputs can share source composition and layout
later without inheriting paper-specific constraints.

## Boundaries

- `project`: portable document state and bounded binary persistence.
- `transform`: CPU composition, tonal separation, connectivity and raster previews.
- `export`: closed vector contours, physical SVG sizing, PNGs and AI request assets.
- `image_io`: Rapport's platform decoder into straight-alpha RGBA working images.
- `application`: windows, tab transfer and one background work queue.
- `editor_window`: native controls and an IfritView-style image canvas.
- `settings`: preferred AI app and manual handoff; no speculative authentication.

Each tab owns a shared project independently of its window. Transform jobs take
a snapshot and publish results on the UI thread. Moving a busy tab does not
cancel or lose the result. Windows retire outside their own callbacks.
Source changes invalidate generated cuts and block stale SVG exports.

## Transform contract

Every cut layer is a binary mask on a common canvas. One means retained material.
All masks share physical dimensions including the frame. Front-to-back ordering
is explicit. Local and AI-assisted generation converge on that representation.
This lets tracing, bridge repair and future manufacturability checks be shared.

## Next milestones

1. Test Jay and the supplied example designs with Tamara; import SVGs into Design
   Space and cut a small physical prototype.
2. Add contour fitting with topology and minimum-feature checks, then manual
   bridge placement and layer repair. Add undo before destructive editing grows.
3. Add crop/placement, source transforms, richer colour simplification, and better
   artistic layer allocation.
4. Investigate provider-supported authentication and image-generation APIs or
   documented local agent integrations. Keep requests reviewable and uploads
   explicit; never represent consumer web login as an implemented API connection.
5. Add engraving output as a separate export/processing profile.
