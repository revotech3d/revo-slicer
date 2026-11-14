# Flat belt (Revo)

A flat belt printer has a horizontal conveyor belt as its bed and a gantry that moves in Z.
The belt only moves between prints or segments, never while a layer is printed. The printer
sets `flat_belt`, which is independent of `belt_printer` (the tilted-belt pipeline: mesh
rotation, shear, belt brim, no skirt): a flat belt prints like a fixed bed, with skirt, brim
and prime tower as usual. Only in Long parts does the plate open along Y
(`PartPlate::belt_open_y()`), so a part can be longer than the bed.

`flat_belt_mode` (process) chooses how the belt is used:

- **Fixed bed** – the belt stands still; a normal print.
- **Sequential** – objects print one by one (`By object`) at the bed centre. Between them the
  gantry lifts by `flat_belt_lift` and `flat_belt_advance_gcode` advances the belt by the
  previous object's depth (skirt and brim included) plus `flat_belt_object_gap`. Objects
  never share the bed, so the sequential clearance check is skipped.
- **Long parts** – a part longer than the bed is printed in segments. GCode generates the
  part's layers whole into a buffer; `FlatBelt::segment_long_parts()` replays them once per
  segment, clipping each extrusion to the segment's band along Y, shifting it back onto the
  bed and making its own travels and retractions. Between segments the belt advances.

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
single filament, because the segmenter replays straight `G1` moves with relative extrusion.
