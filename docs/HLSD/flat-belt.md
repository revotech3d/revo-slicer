# Flat belt (Revo)

A flat belt printer has a horizontal conveyor belt as its bed and a gantry that moves in Z.
The belt only moves between prints or segments, never while a layer is printed. The printer
sets `flat_belt`, which is independent of `belt_printer` (the tilted-belt pipeline: mesh
rotation, shear, belt brim, no skirt): a flat belt prints like a fixed bed, with skirt, brim
and prime tower as usual. Only in Long parts does the plate open along Y
(`PartPlate::belt_open_y()`), so a part can be longer than the bed.

`flat_belt_mode` (process) chooses how the belt is used:

- **Fixed bed** – the belt stands still; a normal print. With `flat_belt_eject` on, the end of the
  file runs `flat_belt_eject_gcode` after the machine end G-code: the printer waits
  `flat_belt_eject_time` minutes or until the bed is at `flat_belt_eject_bed_temp`, then turns the
  belt once to drop the part off the front (Revo OS `FLAT_BELT_EJECT`, belt length in `_REVO_CFG`).
- **Sequential** – objects print one by one (`By object`) at the bed centre: `GCode::object_origin()`
  replaces the instance's plate position wherever an origin is set, brim and object skirt included. Between them the
  gantry lifts by `flat_belt_lift` and `flat_belt_advance_gcode` advances the belt by the
  previous object's depth (skirt and brim included) plus `flat_belt_object_gap`. Objects
  never share the bed, so the sequential clearance check is skipped. A single object is written once
  as the body of an `[sdcard_loop]` (see Printer target below), `flat_belt_copies` being the starting
  target; the body starts and ends retracted and lifted above the piece, and ends with its own belt
  advance. With several objects `flat_belt_copies` repeats each one that many times in a row (the
  instance ordering simply lists it again) and the pieces are written out.
- **Plates** – the whole plate is printed by layer like a fixed bed, then the belt moves it forward
  by its depth along Y (the extent of its extrusions) plus `flat_belt_object_gap`, and the plate is
  printed again, `flat_belt_copies` times. GCode generates the plate's layers once into a buffer and
  `FlatBelt::repeat_plates()` replays them with its own travels and retractions, so every plate is
  the same text that starts and ends retracted above the plate. The plate is written once as the body
  of an `[sdcard_loop]`, followed by its belt advance; `flat_belt_copies` is the starting target,
  0 = until stopped.
- **Long parts** – a part longer than the bed is printed in segments. GCode generates the
  part's layers whole into a buffer; `FlatBelt::segment_long_parts()` replays them once per
  segment, clipping each extrusion to the segment's band along Y, shifting it back onto the
  bed and making its own travels and retractions. Between segments the belt advances.
  One object can be as long as 32-bit coordinates around its centre allow (Arachne and the
  Voronoi code take int32): about 43 m at the 10 um resolution the plater picks for a flat
  belt printer, about 4.3 m at the default 1 nm; a longer part is refused with that limit and
  pointed to Endless.
  A part that fits on the bed is refused with `STRING_EXCEPT_FLAT_BELT_FITS_BED`; the error
  notification offers a one-click switch to Fixed bed instead of a jump to the setting.

## Seam

The band boundary between two segments recedes towards the front with each layer, so the
next segment lays its layers onto the previous one:

- **Slope**: by `z / tan α`, with `tan α = 0.8 · clearance_z / clearance_y` – how far the nozzle
  reaches below the toolhead over how far the toolhead reaches forward. The toolhead then
  never touches the finished segment.
- **Steps**: by `flat_belt_step_length` per layer, a lap joint. One step longer than the
  toolhead front means at most one layer of the old segment lies under the toolhead.
- **V-steps**: steps whose boundary is not a straight line across the part but a row of V
  teeth, `flat_belt_tooth_width` wide and `flat_belt_tooth_depth` deep, shifted sideways by
  `flat_belt_tooth_shift` of a tooth on every layer (0.5 alternates them, 0.33 / 0.25 turn
  them gradually). The segments interlock in X/Y as well as in Z, and the weak points of
  neighbouring layers never lie on top of each other. The boundary reaches back by up to one
  tooth depth, so the seam is longer by the depth and a step minus the depth must still be
  longer than the toolhead front. Each move is cut at the tooth tips and valleys, where the
  boundary bends, and every piece clipped against two straight lines; the pieces of one move
  across all segments always add up to the whole move, so no filament is lost or doubled.
- **Auto**: steps when the seam leaves at least half the bed, otherwise a slope. Auto never
  picks V-steps; it is chosen explicitly.

The belt advance is the print zone length minus the seam. `flat_belt_segment_length` can make it
shorter: a warm part leaves the bed in small steps and stays stuck to the belt where it is still
hot. The print zone (advance plus seam) always ends at the back of the bed, so a part starts at
Y max and the belt carries it to the front. In every mode the belt moves at `flat_belt_advance_speed`, passed to
`flat_belt_advance_gcode` as `belt_advance_speed`.

## Endless

With `flat_belt_endless` the first segment is written once, then one middle segment inside a
Klipper `[sdcard_loop]`. Every middle segment sits at the same place on the bed, so the loop body
is identical on each pass; it starts and ends retracted. The target is the first segment plus
`flat_belt_endless_count` repeats, 0 = until stopped.

## Constraints

Long parts needs one object, `By layer`, relative E, no arc fitting, no prime tower and a
single filament, because the segmenter replays straight `G1` moves with relative extrusion. Plates
replays the same way and has the same needs, except that it takes any number of objects.

## Printer target

Every mode writes `FLAT_BELT_INFO MODE= TARGET= LOOP= FILE_UNITS= ADVANCE= START= UNIT=` at the start
of the print, and `FLAT_BELT_UNIT_DONE` after each piece, plate or segment, so the Revo OS screen
shows the mode, the units done of the target, the time per unit and the time left (Endless: the
belt length printed, `START + (done - 1) · ADVANCE`). `FILE_UNITS` is how many units the file's time
estimate covers, until the printer has measured one.

Where the unit is repeated in a loop (`LOOP=1`: Plates, Endless, Sequential with one object), the
loop runs until stopped (`COUNT=0`) and the target lives on the printer: `FLAT_BELT_SET_TARGET`
changes it while printing, and `FLAT_BELT_UNIT_DONE` ends the loop once it is reached. Klipper only
accepts `SDCARD_LOOP_DESIST` from outside the file, so the macro schedules it on a timer, which
runs between two lines of the file before the loop's end. The preview, time and filament the slicer
shows are therefore for one unit. Long parts and Sequential with several objects write every unit
out and keep a fixed target. The older `ENDLESS=` and `SEGMENTS=` parameters are still written for
Revo OS versions that predate the target.

The line sits near the start of the file, so the printer's file list (Revo Panel, the machine
screen) reads the mode and quantity from it before a print, and lets the quantity or length be
chosen at the start (`FLAT_BELT_NEXT`, taken by `FLAT_BELT_INFO` in place of the file's target).
Fixed bed adds `EJECT=` so a series of the print can drop each part once it has cooled.

## 3D view

`FlatBeltPreview`, drawn by both canvases (3D view and print preview) right after the plates,
shows how the selected mode uses the belt around the current plate: the belt strip under the bed
and in front of it, fading out; the copies still to come in Sequential and Plates as grey
see-through boxes the size of each object, spaced as the belt will carry them; the infinity sign
behind the belt for Endless; the eject arrow for Fixed bed with Eject. The mode is named along the
left side of the plate. It reads the edited presets and the model at render time, so it follows
every change without its own invalidation.
