#pragma once

#include <string>

namespace Slic3r {

class PrintConfig;

// Revo flat belt, Long parts mode: a part longer than the bed is printed in segments.
//
// The part is sliced and its G-code generated as usual, the whole length at once. The
// layers are then replayed once per segment, each move clipped to the segment's band along
// Y and shifted back onto the bed by the distance the belt has advanced so far. Between
// segments the gantry lifts and the belt advance G-code runs.
//
// The band boundary between two segments recedes towards the front with every layer, so
// the previous segment ends in a seam the next one lays its layers onto:
//   - Slope: by z / tan(alpha), where tan(alpha) follows from the toolhead clearance
//     (how far the nozzle reaches below the toolhead over how far the toolhead reaches
//     forward), so the toolhead never touches the finished segment;
//   - Steps: by one step length per layer, a lap joint that is strongest for low parts and
//     always clear of the toolhead once a step is longer than the toolhead front.
namespace FlatBelt {

struct LongPartsPlan
{
    bool   steps     = false; // Steps seam, otherwise Slope
    double tan_alpha = 0.;    // Slope seam
    double step      = 0.;    // Steps seam, per layer
    double seam      = 0.;    // seam length along Y, mm
    double advance   = 0.;    // belt advance between segments, mm
    double bed_y_min = 0.;    // front edge of the print zone, mm
};

// Plans the seam and the belt advance for a part of the given height and layer count.
// Throws a SlicingError when the seam does not fit the bed.
LongPartsPlan plan_long_parts(const PrintConfig &config, double part_height, size_t layer_count);

// Turns the layers' G-code (relative E, no arcs) into the segmented print.
// advance_gcode is the processed belt advance G-code.
std::string segment_long_parts(const std::string   &layers_gcode,
                               const LongPartsPlan &plan,
                               const PrintConfig   &config,
                               double               part_height,
                               const std::string   &advance_gcode);

} // namespace FlatBelt
} // namespace Slic3r
