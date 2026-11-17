#pragma once

#include <functional>
#include <string>
#include <vector>

#include "BoundingBox.hpp"

namespace Slic3r {

class Layer;
class PrintConfig;

// Revo flat belt, Long parts mode: a part longer than the bed is printed in segments.
//
// The part's layers are cut along the segment boundaries while slicing (cut_long_parts_layers), so
// every segment's piece of a layer has closed walls along the seam. Its G-code is generated as
// usual, the whole length at once, and replayed once per segment: the paths inside the segment's
// band along Y, shifted back onto the bed by the distance the belt has advanced so far. Between
// segments the gantry lifts and the belt advance G-code runs.
//
// The band boundary between two segments recedes towards the front with every layer, so
// the previous segment ends in a seam the next one lays its layers onto:
//   - Slope: by z / tan(alpha), where tan(alpha) follows from the toolhead clearance
//     (how far the nozzle reaches below the toolhead over how far the toolhead reaches
//     forward), so the toolhead never touches the finished segment;
//   - Steps: by one step length per layer, a lap joint that is strongest for low parts and
//     always clear of the toolhead once a step is longer than the toolhead front.
//   - V-steps: steps whose boundary is not a straight line across the part but a row of V
//     teeth (flat_belt_tooth_width wide, flat_belt_tooth_depth deep), shifted by
//     flat_belt_tooth_shift of a tooth on every layer, so the segments interlock in X/Y too and
//     the weak points of neighbouring layers never lie on top of each other. The toolhead clears
//     the finished segment while a step minus the tooth depth is longer than its front.
namespace FlatBelt {

struct LongPartsPlan
{
    bool   steps     = false; // Steps or V-steps seam, otherwise Slope
    bool   vsteps    = false; // V-steps: the step boundary is a row of V teeth across the part
    double tooth_width = 0.;  // V-steps, along X, mm
    double tooth_depth = 0.;  // V-steps, along Y, mm
    double tooth_shift = 0.;  // V-steps, phase shift per layer, fraction of a tooth
    double tan_alpha = 0.;    // Slope seam
    double step      = 0.;    // Steps seam, per layer
    double seam      = 0.;    // seam length along Y, mm
    double advance   = 0.;    // belt advance between segments, mm
    double bed_y_min = 0.;    // front edge of the print zone, mm; the zone ends at the back margin of the bed
    double x_origin  = 0.;    // V-steps: X the teeth are measured from (the part's left edge), mm
};

// Number of segments for a part of the given length along Y.
size_t long_parts_segments(const LongPartsPlan &plan, double length);

// Y of the boundary between segments j - 1 and j on a layer (0 based) at height z, for a part whose front
// is at y_front. V-steps: this is the tooth tips; the boundary reaches back by tooth_offset() from it.
double long_parts_boundary(const LongPartsPlan &plan, double y_front, size_t j, size_t layer_idx, double z);

// Cuts every layer of the part along the segment boundaries (steps, V teeth or slope), so each segment's piece
// of a layer is its own island: the perimeters close along the seam, one outline around the steps or teeth,
// and the G-code replay finds every path inside one segment instead of cutting paths at the seam into
// short pieces with a retraction each. part: the part's extent in the layers' frame.
void cut_long_parts_layers(const LongPartsPlan &plan, const BoundingBoxf &part, const std::vector<Slic3r::Layer*> &layers);

// Plans the seam and the belt advance for a part of the given height and layer count.
// Throws a SlicingError when the seam does not fit the bed.
LongPartsPlan plan_long_parts(const PrintConfig &config, double part_height, size_t layer_count);

// Turns the layers' G-code (relative E, no arcs) into the segmented print.
// advance_gcode is the processed belt advance G-code. part: the part's extent in G-code coordinates, the same
// outline cut_long_parts_layers() used, so the replay's boundaries fall in the cuts.
std::string segment_long_parts(const std::string   &layers_gcode,
                               const LongPartsPlan &plan,
                               const PrintConfig   &config,
                               double               part_height,
                               const BoundingBoxf  &part,
                               const std::string   &advance_gcode);

// Plates mode: the whole plate (relative E, no arcs) written once and repeated by Klipper's [sdcard_loop],
// the belt moving each printed plate forward by its depth plus flat_belt_object_gap. FLAT_BELT_INFO passes
// flat_belt_copies (0 = until stopped) to the printer as the target, which can be changed while printing;
// FLAT_BELT_UNIT_DONE after each plate ends the loop when the target is reached.
// advance_gcode returns the processed belt advance G-code for a distance in mm.
std::string repeat_plates(const std::string                         &layers_gcode,
                          const PrintConfig                         &config,
                          double                                     plate_height,
                          const std::function<std::string(double)>  &advance_gcode);

} // namespace FlatBelt
} // namespace Slic3r
