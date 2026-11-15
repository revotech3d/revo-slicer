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
  never share the bed, so the sequential clearance check is skipped. `flat_belt_copies` repeats
  each object that many times in a row: the instance ordering simply lists it again.
- **Plates** – the whole plate is printed by layer like a fixed bed, then the belt moves it forward
  by its depth along Y (the extent of its extrusions) plus `flat_belt_object_gap`, and the plate is
  printed again, `flat_belt_copies` times. GCode generates the plate's layers once into a buffer and
  `FlatBelt::repeat_plates()` replays them with its own travels and retractions, so every plate is
  the same text that starts and ends retracted above the plate. A known number of plates is written
  out one after another (file progress and time left stay right); 0 copies loops one plate in
  `[sdcard_loop]` until stopped.
- **Long parts** – a part longer than the bed is printed in segments. GCode generates the
  part's layers whole into a buffer; `FlatBelt::segment_long_parts()` replays them once per
  segment, clipping each extrusion to the segment's band along Y, shifting it back onto the
  bed and making its own travels and retractions. Between segments the belt advances.
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
- **Auto**: steps when the seam leaves at least half the bed, otherwise a slope.

The belt advance is the print zone length minus the seam.

## Endless

With `flat_belt_endless` the first segment is written once, then one middle segment inside a
Klipper `[sdcard_loop]` (`SDCARD_LOOP_BEGIN COUNT=flat_belt_endless_count`, 0 = until stopped).
Every middle segment sits at the same place on the bed, so the loop body is identical on each
pass; it starts and ends retracted. Revo OS ends the loop with `FLAT_BELT_FINISH`
(`SDCARD_LOOP_DESIST`).

## Constraints

Long parts needs one object, `By layer`, relative E, no arc fitting, no prime tower and a
single filament, because the segmenter replays straight `G1` moves with relative extrusion. Plates
replays the same way and has the same needs, except that it takes any number of objects.

Every mode that moves the belt writes `FLAT_BELT_INFO ENDLESS= ADVANCE= SEGMENTS= UNIT=` at the
start of the print, so Revo OS can count segments, plates or pieces on the screen.

## 3D view

`FlatBeltPreview`, drawn by both canvases (3D view and print preview) right after the plates,
shows how the selected mode uses the belt around the current plate: the belt strip under the bed
and in front of it, fading out; the copies still to come in Sequential and Plates as grey
see-through boxes the size of each object, spaced as the belt will carry them; the infinity sign
behind the belt for Endless; the eject arrow for Fixed bed with Eject. The mode is named along the
left side of the plate. It reads the edited presets and the model at render time, so it follows
every change without its own invalidation.
