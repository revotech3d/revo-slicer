#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "test_helpers.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Revon Belt: a 350 x 350 bed, the belt carries the print towards -Y.
constexpr double BED      = 350.;
constexpr double MARGIN   = 5.;   // FlatBelt keeps 5 mm clear at the front and back edge
constexpr double LENGTH   = 600.; // longer than the bed
constexpr double HEIGHT   = 2.;   // 10 layers of 0.2 mm
constexpr double STEP     = 20.;  // Steps seam, per layer
constexpr int    LAYERS   = 10;

DynamicPrintConfig belt_config(const char *mode, bool endless)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "printable_area", "0x0,350x0,350x350,0x350" },
        { "printable_height", 300 },
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "use_relative_e_distances", 1 },
        { "enable_arc_fitting", "disabled" },
        { "enable_prime_tower", 0 },
        { "print_sequence", "by layer" },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
        { "flat_belt", 1 },
        { "flat_belt_mode", mode },
        { "flat_belt_seam", "steps" },
        { "flat_belt_step_length", STEP },
        { "flat_belt_head_clearance_y", 11 },
        { "flat_belt_endless", endless ? 1 : 0 },
        { "flat_belt_advance_gcode", "BELT_ADVANCE DIST={belt_advance}" },
    });
    return config;
}

// The part lies along Y with its back end on the bed and the rest in front of it, as the
// plater places a Long parts part: Y from 345 - LENGTH to 345.
std::string slice_long_bar(const DynamicPrintConfig &config)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "bar.stl";
    object->add_volume(make_cube(20., LENGTH, HEIGHT));
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(BED / 2., BED - MARGIN - LENGTH / 2., HEIGHT / 2.));
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    const StringObjectException err = print.validate();
    INFO("validate: " << err.string);
    REQUIRE(err.string.empty());
    return Test::gcode(print);
}

int count_lines_starting(const std::string &gcode, const std::string &prefix)
{
    int n = 0;
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);)
        if (line.rfind(prefix, 0) == 0)
            ++n;
    return n;
}

double param(const std::string &line, char key)
{
    const size_t i = line.find(std::string(" ") + key);
    return i == std::string::npos ? std::numeric_limits<double>::quiet_NaN() : std::atof(line.c_str() + i + 2);
}

struct Extrusions
{
    double y_min = std::numeric_limits<double>::max();
    double y_max = std::numeric_limits<double>::lowest();
    double e     = 0.;
};

// Extruding XY moves from the first line starting with `from` on (the whole file when empty).
Extrusions extrusions(const std::string &gcode, const std::string &from)
{
    Extrusions r;
    bool on = from.empty();
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        if (!on) {
            on = line.rfind(from, 0) == 0;
            continue;
        }
        if (line.rfind("G1 ", 0) != 0)
            continue;
        const std::string cmd = line.substr(0, line.find(';'));
        const double e = param(cmd, 'E'), y = param(cmd, 'Y');
        if (!(e > 0.) || (cmd.find(" X") == std::string::npos && cmd.find(" Y") == std::string::npos))
            continue;
        r.e += e;
        if (y == y) {
            r.y_min = std::min(r.y_min, y);
            r.y_max = std::max(r.y_max, y);
        }
    }
    return r;
}

} // namespace

TEST_CASE("Long parts prints a part longer than the bed in segments on the bed", "[FlatBelt]")
{
    const std::string gcode = slice_long_bar(belt_config("long_parts", false));

    // Print zone 350 - 2 * 5 = 340 mm; Steps seam 10 layers * 20 mm = 200 mm; belt advance
    // 340 - 200 = 140 mm. Segments n with n * 140 + 200 >= 600: three.
    const double window  = BED - 2. * MARGIN;
    const double advance = window - LAYERS * STEP;
    REQUIRE(count_lines_starting(gcode, "FLAT_BELT_INFO ENDLESS=0") == 1);
    CHECK(count_lines_starting(gcode, "; FLAT_BELT_SEGMENT ") == 3);

    const int advances = count_lines_starting(gcode, "BELT_ADVANCE DIST=");
    CHECK(advances == 2);
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);)
        if (line.rfind("BELT_ADVANCE DIST=", 0) == 0)
            CHECK_THAT(std::atof(line.c_str() + 18), WithinAbs(advance, 0.01));

    // Every extrusion lands on the bed's print zone.
    const Extrusions seg = extrusions(gcode, "FLAT_BELT_INFO");
    CHECK(seg.y_min >= MARGIN - 0.01);
    CHECK(seg.y_max <= BED - MARGIN + 0.01);
}

TEST_CASE("Long parts extrudes the same filament as the unsegmented part", "[FlatBelt]")
{
    const Extrusions whole = extrusions(slice_long_bar(belt_config("fixed_bed", false)), "");
    const Extrusions seg   = extrusions(slice_long_bar(belt_config("long_parts", false)), "FLAT_BELT_INFO");
    REQUIRE(whole.e > 0.);
    CHECK_THAT(seg.e, WithinRel(whole.e, 0.01));
}

TEST_CASE("Endless repeats one middle segment in an sdcard loop", "[FlatBelt]")
{
    const std::string gcode = slice_long_bar(belt_config("long_parts", true));
    CHECK(count_lines_starting(gcode, "FLAT_BELT_INFO ENDLESS=1") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_BEGIN") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_END") == 1);
    CHECK(count_lines_starting(gcode, "BELT_ADVANCE DIST=") == 1);
}
