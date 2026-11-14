#include "FlatBelt.hpp"

#include "BoundingBox.hpp"
#include "Exception.hpp"
#include "I18N.hpp"
#include "PrintConfig.hpp"
#include "format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <vector>

namespace Slic3r {
namespace FlatBelt {

// Kept clear at the front and back edge of the bed.
static constexpr double BED_MARGIN = 5.;
// The toolhead clearance is used at 80 %, so a slightly bigger toolhead than measured still clears.
static constexpr double CLEARANCE_SAFETY = 0.8;
static constexpr double TRAVEL_HOP = 0.4;

LongPartsPlan plan_long_parts(const PrintConfig &config, double part_height, size_t layer_count)
{
    LongPartsPlan plan;
    const BoundingBoxf bed(config.printable_area.values);
    plan.bed_y_min = bed.min.y() + BED_MARGIN;
    const double window = bed.size().y() - 2. * BED_MARGIN;

    plan.tan_alpha = CLEARANCE_SAFETY * config.flat_belt_head_clearance_z.value / config.flat_belt_head_clearance_y.value;
    plan.step      = config.flat_belt_step_length.value;
    const double slope_seam = part_height / plan.tan_alpha;
    const double steps_seam = double(layer_count) * plan.step;

    switch (config.flat_belt_seam.value) {
    case FlatBeltSeam::Steps: plan.steps = true; break;
    case FlatBeltSeam::Slope: plan.steps = false; break;
    // Steps are the stronger joint; take them when they leave at least half the bed to print on.
    default: plan.steps = steps_seam <= 0.5 * window; break;
    }
    if (plan.steps && plan.step < config.flat_belt_head_clearance_y.value)
        throw SlicingError(format(_u8L("The seam step (%1% mm) must be longer than the toolhead front (%2% mm)."),
                                  plan.step, config.flat_belt_head_clearance_y.value));
    plan.seam    = plan.steps ? steps_seam : slope_seam;
    plan.advance = window - plan.seam;
    if (plan.advance < 0.25 * window)
        throw SlicingError(format(_u8L("The seam is %1% mm long and leaves too little of the bed for each segment. "
                                       "Use shorter steps, a slope seam or a lower part."),
                                  std::lround(plan.seam)));
    return plan;
}

namespace {

struct Move
{
    double x0, y0, x1, y1; // XY before and after
    double e;              // relative E
    double f;              // feed rate in effect, mm/min
    bool   xy;             // moves in XY
};

struct Item
{
    bool        is_move = false;
    Move        move{};
    std::string raw;
};

struct Layer
{
    double            z = 0.;
    std::vector<Item> items;
    double            y_min = std::numeric_limits<double>::max(); // of the extrusions
    double            y_max = std::numeric_limits<double>::lowest();
};

bool read_param(const std::string &line, char key, double &out)
{
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == ';') break;
        if (line[i] == key && (i == 0 || line[i - 1] == ' ')) {
            out = std::atof(line.c_str() + i + 1);
            return true;
        }
    }
    return false;
}

// Part of the segment a-b with y in [lo, hi], as parameters t0 < t1 along it.
bool clip_y(double ya, double yb, double lo, double hi, double &t0, double &t1)
{
    t0 = 0.; t1 = 1.;
    const double dy = yb - ya;
    if (std::abs(dy) < 1e-9)
        return ya >= lo && ya <= hi;
    double ta = (lo - ya) / dy, tb = (hi - ya) / dy;
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(0., ta);
    t1 = std::min(1., tb);
    return t1 - t0 > 1e-9;
}

class Emitter
{
public:
    Emitter(std::ostringstream &out, double retract, double retract_f, double travel_f)
        : m_out(out), m_retract(retract), m_retract_f(retract_f), m_travel_f(travel_f) {}

    void set_shift(double dy) { m_dy = dy; }
    void set_layer_z(double z) { m_z = z; }
    void approach_from_above(double z) { m_high = true; m_cur_z = z; m_has_pos = false; }

    void extrude(double xa, double ya, double xb, double yb, double e, double f)
    {
        if (!m_has_pos || std::hypot(xa - m_x, ya - m_y) > 1e-3 || std::abs(m_cur_z - m_z) > 1e-6)
            travel(xa, ya);
        if (m_retracted) {
            m_out << "G1 E" << fmt(m_retract) << " F" << fmt(m_retract_f) << "\n";
            m_retracted = false;
        }
        m_out << "G1 X" << fmt(xb) << " Y" << fmt(yb + m_dy) << " E" << fmt5(e) << " F" << fmt(f) << "\n";
        m_x = xb; m_y = yb;
    }

    void retract()
    {
        if (!m_retracted) {
            m_out << "G1 E-" << fmt(m_retract) << " F" << fmt(m_retract_f) << "\n";
            m_retracted = true;
        }
    }

    void lift_to(double z)
    {
        retract();
        m_out << "G1 Z" << fmt(z) << " F600\n";
        m_cur_z = z;
    }

private:
    void travel(double x, double y)
    {
        retract();
        if (m_high) {
            // After a belt advance: over the new start point first, only then down.
            m_out << "G0 X" << fmt(x) << " Y" << fmt(y + m_dy) << " F" << fmt(m_travel_f) << "\n";
            m_out << "G1 Z" << fmt(m_z) << " F600\n";
            m_high = false;
        } else {
            m_out << "G1 Z" << fmt(m_z + TRAVEL_HOP) << " F600\n";
            m_out << "G0 X" << fmt(x) << " Y" << fmt(y + m_dy) << " F" << fmt(m_travel_f) << "\n";
            m_out << "G1 Z" << fmt(m_z) << " F600\n";
        }
        m_cur_z = m_z;
        m_x = x; m_y = y; m_has_pos = true;
    }

    static std::string fmt(double v) { char b[32]; std::snprintf(b, sizeof(b), "%.3f", v); return b; }
    static std::string fmt5(double v) { char b[32]; std::snprintf(b, sizeof(b), "%.5f", v); return b; }

    std::ostringstream &m_out;
    double m_retract, m_retract_f, m_travel_f;
    double m_dy = 0., m_z = 0., m_cur_z = 0., m_x = 0., m_y = 0.;
    bool   m_has_pos = false, m_retracted = false, m_high = false;
};

} // namespace

std::string segment_long_parts(const std::string   &layers_gcode,
                               const LongPartsPlan &plan,
                               const PrintConfig   &config,
                               double               part_height,
                               const std::string   &advance_gcode)
{
    // 1) Parse into layers, with every move's start and end point.
    std::vector<Layer> layers(1); // layers[0]: whatever comes before the first layer change
    double x = 0., y = 0., f = 1800.;
    {
        std::istringstream in(layers_gcode);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind(";LAYER_CHANGE", 0) == 0)
                layers.emplace_back();
            Layer &layer = layers.back();
            const bool g0 = line.rfind("G0 ", 0) == 0, g1 = line.rfind("G1 ", 0) == 0;
            if (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0)
                throw SlicingError(_u8L("Long parts flat belt mode needs Arc fitting disabled."));
            if (line.rfind(";Z:", 0) == 0 && layers.size() > 1)
                layer.z = std::atof(line.c_str() + 3);
            if (!g0 && !g1) {
                Item it; it.raw = line;
                layer.items.push_back(std::move(it));
                continue;
            }
            Move m{x, y, x, y, 0., f, false};
            double v;
            if (read_param(line, 'F', v)) f = m.f = v;
            if (read_param(line, 'X', v)) m.x1 = v;
            if (read_param(line, 'Y', v)) m.y1 = v;
            if (read_param(line, 'E', v)) m.e = v;
            if (read_param(line, 'Z', v) && layer.z == 0. && layers.size() > 1) layer.z = v;
            m.xy = std::hypot(m.x1 - m.x0, m.y1 - m.y0) > 1e-9;
            x = m.x1; y = m.y1;
            if (m.xy && m.e > 0.) {
                layer.y_min = std::min({layer.y_min, m.y0, m.y1});
                layer.y_max = std::max({layer.y_max, m.y0, m.y1});
                Item it; it.is_move = true; it.move = m;
                layer.items.push_back(std::move(it));
            }
            // Travels, retracts, wipes and Z hops are left out: the emitter makes its own.
        }
    }
    // A layer's height: the Z the layer change moved to (the first Z found in it).
    double y_front = std::numeric_limits<double>::max(), y_back = std::numeric_limits<double>::lowest();
    for (size_t i = 1; i < layers.size(); ++i) {
        y_front = std::min(y_front, layers[i].y_min);
        y_back  = std::max(y_back, layers[i].y_max);
        if (layers[i].z == 0.)
            layers[i].z = layers[i - 1].z;
    }
    if (layers.size() < 2 || y_front > y_back)
        return layers_gcode;

    // 2) Segment boundaries along the part: Ys_j = j * advance + seam, receding with each layer.
    const double length = y_back - y_front;
    size_t n_seg = 1;
    while (double(n_seg) * plan.advance + plan.seam < length)
        ++n_seg;
    auto boundary = [&](size_t j, size_t layer_idx /* 0 based */) {
        const double recede = plan.steps ? double(layer_idx) * plan.step : layers[layer_idx + 1].z / plan.tan_alpha;
        return y_front + double(j) * plan.advance + plan.seam - recede;
    };

    const double retract   = config.retraction_length.get_at(0);
    const double retract_f = 60. * config.retraction_speed.get_at(0);
    const double travel_v  = config.travel_speed.get_at(0);
    const double travel_f  = 60. * (std::isfinite(travel_v) && travel_v > 0. ? travel_v : 150.);

    const bool endless = config.flat_belt_endless.value;
    if (endless && n_seg < 3)
        throw SlicingError(format(_u8L("Endless printing repeats a middle segment of the part, so the model must be at least %1% mm long."),
                                  std::lround(2. * plan.advance + plan.seam + 1.)));

    std::ostringstream out;
    Emitter em(out, retract, retract_f, travel_f);
    for (const Item &it : layers.front().items)
        if (!it.is_move)
            out << it.raw << "\n";

    auto emit_segment = [&](size_t k) {
        // Segment k sits on the bed after k advances: front of the part at the front of the print zone.
        em.set_shift(plan.bed_y_min - y_front - double(k) * plan.advance);
        for (size_t li = 1; li < layers.size(); ++li) {
            const Layer &layer = layers[li];
            const double lo = k == 0 ? std::numeric_limits<double>::lowest() : boundary(k, li - 1);
            const double hi = k + 1 == n_seg ? std::numeric_limits<double>::max() : boundary(k + 1, li - 1);
            if (layer.y_max < lo || layer.y_min > hi)
                continue;
            em.set_layer_z(layer.z);
            for (const Item &it : layer.items) {
                if (!it.is_move) {
                    out << it.raw << "\n";
                    continue;
                }
                const Move &m = it.move;
                double t0, t1;
                if (!clip_y(m.y0, m.y1, lo, hi, t0, t1))
                    continue;
                em.extrude(m.x0 + t0 * (m.x1 - m.x0), m.y0 + t0 * (m.y1 - m.y0),
                           m.x0 + t1 * (m.x1 - m.x0), m.y0 + t1 * (m.y1 - m.y0), m.e * (t1 - t0), m.f);
            }
        }
    };
    auto emit_advance = [&]() {
        const double safe_z = part_height + config.flat_belt_lift.value;
        out << "; FLAT_BELT_ADVANCE\n";
        em.lift_to(safe_z);
        out << advance_gcode << (advance_gcode.empty() || advance_gcode.back() == '\n' ? "" : "\n");
        em.approach_from_above(safe_z);
    };

    if (endless) {
        // The first segment has the part's front end. Every later one is the same middle
        // segment at the same place on the bed (the belt has carried the rest forward), so it is
        // written once and repeated by Klipper's [sdcard_loop] until the print is stopped, or
        // flat_belt_endless_count times. SDCARD_LOOP_DESIST ends it after the current segment.
        out << "; FLAT_BELT_SEGMENT 1\n";
        emit_segment(0);
        em.retract(); // the loop body starts and ends retracted, so every pass sees the same state
        out << "SDCARD_LOOP_BEGIN COUNT=" << std::max(0, config.flat_belt_endless_count.value) << "\n";
        emit_advance();
        out << "; FLAT_BELT_SEGMENT REPEAT\n";
        emit_segment(1);
        em.retract();
        out << "SDCARD_LOOP_END\n";
    } else {
        for (size_t k = 0; k < n_seg; ++k) {
            out << "; FLAT_BELT_SEGMENT " << (k + 1) << "/" << n_seg << "\n";
            emit_segment(k);
            if (k + 1 < n_seg)
                emit_advance();
        }
    }
    em.retract();
    return out.str();
}

} // namespace FlatBelt
} // namespace Slic3r
