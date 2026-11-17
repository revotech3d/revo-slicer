#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "test_helpers.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/FlatBelt.hpp"
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
        { "gcode_flavor", "klipper" }, // Revo OS
        { "layer_change_gcode", "G92 E0" },
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

// Cubes of 20 x 20 x HEIGHT mm in a row across the bed (X), at the bed centre in Y.
std::string slice_cubes(const DynamicPrintConfig &config, int count)
{
    Model model;
    Print print;
    for (int i = 0; i < count; ++i) {
        ModelObject *object = model.add_object();
        object->name = "cube.stl";
        object->add_volume(make_cube(20., 20., HEIGHT));
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(BED / 2. + 40. * (i - (count - 1) / 2.), BED / 2., HEIGHT / 2.));
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
    }
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

// Breaks in the extruded path from the first line starting with `from` on: extruding moves that do not
// start where the previous one ended (a travel in between).
int path_breaks(const std::string &gcode, const std::string &from)
{
    int    n = 0;
    bool   on = from.empty();
    double x = 0., y = 0., ex = std::numeric_limits<double>::quiet_NaN(), ey = ex;
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        on = on || line.rfind(from, 0) == 0;
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        const std::string cmd = line.substr(0, line.find(';'));
        const double px = x, py = y, e = param(cmd, 'E'), nx = param(cmd, 'X'), ny = param(cmd, 'Y');
        if (nx == nx) x = nx;
        if (ny == ny) y = ny;
        const bool xy = std::hypot(x - px, y - py) > 1e-6;
        if (!on || !xy || !(e > 0.))
            continue;
        if (!(std::hypot(px - ex, py - ey) < 1e-3))
            ++n;
        ex = x;
        ey = y;
    }
    return n;
}

} // namespace

TEST_CASE("Long parts prints a part longer than the bed in segments on the bed", "[FlatBelt]")
{
    const std::string gcode = slice_long_bar(belt_config("long_parts", false));

    // Print zone 350 - 2 * 5 = 340 mm; Steps seam 10 layers * 20 mm = 200 mm; belt advance
    // 340 - 200 = 140 mm. Segments n with n * 140 + 200 >= 600: three.
    const double window  = BED - 2. * MARGIN;
    const double advance = window - LAYERS * STEP;
    REQUIRE(count_lines_starting(gcode, "FLAT_BELT_INFO MODE=long TARGET=3 LOOP=0") == 1);
    CHECK(count_lines_starting(gcode, "; FLAT_BELT_SEGMENT ") == 3);
    CHECK(count_lines_starting(gcode, "FLAT_BELT_UNIT_DONE") == 3);

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

TEST_CASE("Long parts extrudes the whole part plus walls along the seams", "[FlatBelt]")
{
    const Extrusions whole = extrusions(slice_long_bar(belt_config("fixed_bed", false)), "");
    const Extrusions seg   = extrusions(slice_long_bar(belt_config("long_parts", false)), "FLAT_BELT_INFO");
    REQUIRE(whole.e > 0.);
    // nothing is lost; the cut at each seam adds the walls that close every segment's piece of a layer
    CHECK(seg.e >= 0.99 * whole.e);
    CHECK(seg.e <= 1.15 * whole.e);
}

TEST_CASE("V-steps prints each segment's piece of a layer with closed walls along the teeth", "[FlatBelt]")
{
    DynamicPrintConfig config = belt_config("long_parts", false);
    config.set_deserialize_strict({ { "flat_belt_seam", "vsteps" }, { "flat_belt_tooth_width", 8 }, { "flat_belt_tooth_depth", 8 } });
    const std::string gcode = slice_long_bar(config);
    // The layers are cut along the teeth before the walls are made, so no path is cut at the seam: each piece
    // of a layer adds a few breaks in the path (its walls and infill), not one per line crossing the teeth.
    // 10 layers, 3 segments: 2 seams, a piece on each side of each.
    const int whole = path_breaks(slice_long_bar(belt_config("fixed_bed", false)), "");
    const int seg   = path_breaks(gcode, "FLAT_BELT_INFO");
    WARN("path breaks: whole part " << whole << ", in segments " << seg);
    CHECK(seg <= whole + 10 * LAYERS * 2 * 2);
}

TEST_CASE("V-steps joins the segments with teeth", "[FlatBelt]")
{
    DynamicPrintConfig config = belt_config("long_parts", false);
    config.set_deserialize_strict({
        { "flat_belt_seam", "vsteps" },
        { "flat_belt_tooth_width", 10 },
        { "flat_belt_tooth_depth", 8 },
        { "flat_belt_tooth_shift", 0.5 },
    });
    const std::string gcode = slice_long_bar(config);

    // Seam: 10 layers * 20 mm steps plus the 8 mm teeth = 208 mm; belt advance 340 - 208 = 132 mm.
    // Segments n with n * 132 + 208 >= 600: three.
    const double advance = BED - 2. * MARGIN - (LAYERS * STEP + 8.);
    REQUIRE(count_lines_starting(gcode, "FLAT_BELT_INFO MODE=long TARGET=3 LOOP=0") == 1);
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);)
        if (line.rfind("BELT_ADVANCE DIST=", 0) == 0)
            CHECK_THAT(std::atof(line.c_str() + 18), WithinAbs(advance, 0.01));

    const Extrusions seg = extrusions(gcode, "FLAT_BELT_INFO");
    CHECK(seg.y_min >= MARGIN - 0.01);
    CHECK(seg.y_max <= BED - MARGIN + 0.01);
    // every bit of the part is printed, the teeth only move where one segment ends (plus their walls)
    const Extrusions whole = extrusions(slice_long_bar(belt_config("fixed_bed", false)), "");
    CHECK(seg.e >= 0.99 * whole.e);
    CHECK(seg.e <= 1.15 * whole.e);
}

TEST_CASE("Long parts: short segments advance slowly", "[FlatBelt]")
{
    DynamicPrintConfig config = belt_config("long_parts", false);
    config.set_deserialize_strict({
        { "flat_belt_seam", "steps" },
        { "flat_belt_segment_length", 100 },
        { "flat_belt_advance_speed", 8 },
        { "flat_belt_advance_gcode", "BELT_ADVANCE DIST={belt_advance} SPEED={belt_advance_speed}" },
    });
    const std::string gcode = slice_long_bar(config);

    // 100 mm per segment instead of the 140 mm the seam leaves, every advance at 8 mm/s
    int advances = 0;
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);)
        if (line.rfind("BELT_ADVANCE DIST=", 0) == 0) {
            ++advances;
            CHECK_THAT(std::atof(line.c_str() + 18), WithinAbs(100., 0.01));
            CHECK(line.find(" SPEED=8") != std::string::npos);
        }
    CHECK(advances >= 3); // 600 mm bar, 200 mm seam: four segments
    // printed at the back of the bed: 100 + 200 mm zone ending at the back margin
    const Extrusions seg = extrusions(gcode, "FLAT_BELT_INFO");
    CHECK(seg.y_max <= BED - MARGIN + 0.01);
    CHECK(seg.y_max >= BED - MARGIN - 2.);
    CHECK(seg.y_min >= BED - MARGIN - 300. - 0.01);
    const Extrusions whole = extrusions(slice_long_bar(belt_config("fixed_bed", false)), "");
    CHECK(seg.e >= 0.99 * whole.e);
    CHECK(seg.e <= 1.15 * whole.e);
}

TEST_CASE("V-steps refuses teeth too deep for the toolhead", "[FlatBelt]")
{
    DynamicPrintConfig config = belt_config("long_parts", false);
    // step 20 mm minus a 12 mm tooth leaves 8 mm, less than the 11 mm toolhead front
    config.set_deserialize_strict({ { "flat_belt_seam", "vsteps" }, { "flat_belt_tooth_depth", 12 } });
    PrintConfig pc;
    pc.apply(config, true);
    CHECK_THROWS_AS(FlatBelt::plan_long_parts(pc, HEIGHT, LAYERS), SlicingError);
}

TEST_CASE("Endless repeats one middle segment in an sdcard loop", "[FlatBelt]")
{
    const std::string gcode = slice_long_bar(belt_config("long_parts", true));
    // Until stopped (flat_belt_endless_count 0): the printer ends the loop at the target length.
    CHECK(count_lines_starting(gcode, "FLAT_BELT_INFO MODE=endless TARGET=0 LOOP=1 FILE_UNITS=2") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_BEGIN COUNT=0") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_END") == 1);
    CHECK(count_lines_starting(gcode, "BELT_ADVANCE DIST=") == 1);
    // the first segment and the repeated one each report when they are done
    CHECK(count_lines_starting(gcode, "FLAT_BELT_UNIT_DONE") == 2);
}

TEST_CASE("Endless passes the number of repeats to the printer as the target", "[FlatBelt]")
{
    // 4 repeats after the first segment: 5 segments, still one loop the printer can extend or cut short.
    DynamicPrintConfig config = belt_config("long_parts", true);
    config.set_deserialize_strict({ { "flat_belt_endless_count", 4 } });
    const std::string gcode = slice_long_bar(config);
    CHECK(count_lines_starting(gcode, "FLAT_BELT_INFO MODE=endless TARGET=5 LOOP=1") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_BEGIN COUNT=0") == 1);
}

TEST_CASE("Plates repeats the plate in a loop with the copies as the printer's target", "[FlatBelt]")
{
    // Three cubes in a row, 3 copies: one plate in the loop, the printer stops after three. Each pass
    // ends with a belt advance of the plate depth (20 mm of cubes) plus the gap (50 mm).
    DynamicPrintConfig config = belt_config("plates", false);
    config.set_deserialize_strict({ { "flat_belt_copies", 3 }, { "flat_belt_object_gap", 50 } });
    const std::string gcode = slice_cubes(config, 3);

    CHECK(count_lines_starting(gcode, "FLAT_BELT_INFO MODE=plates TARGET=3 LOOP=1 FILE_UNITS=1") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_BEGIN COUNT=0") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_END") == 1);
    CHECK(count_lines_starting(gcode, "FLAT_BELT_UNIT_DONE") == 1);
    CHECK(count_lines_starting(gcode, "BELT_ADVANCE DIST=") == 1);
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);)
        if (line.rfind("BELT_ADVANCE DIST=", 0) == 0)
            CHECK_THAT(std::atof(line.c_str() + 18), WithinAbs(20. + 50., 1.));

    // The loop body is one plate.
    const Extrusions one    = extrusions(slice_cubes(belt_config("fixed_bed", false), 3), "");
    const Extrusions plates = extrusions(gcode, "FLAT_BELT_INFO");
    REQUIRE(one.e > 0.);
    CHECK_THAT(plates.e, WithinRel(one.e, 0.01));
}

TEST_CASE("Plates until stopped repeats one plate in an sdcard loop", "[FlatBelt]")
{
    DynamicPrintConfig config = belt_config("plates", false);
    config.set_deserialize_strict({ { "flat_belt_copies", 0 } });
    const std::string gcode = slice_cubes(config, 3);
    CHECK(count_lines_starting(gcode, "FLAT_BELT_INFO MODE=plates TARGET=0 LOOP=1") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_BEGIN COUNT=0") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_END") == 1);
    CHECK(count_lines_starting(gcode, "BELT_ADVANCE DIST=") == 1);
}

TEST_CASE("Sequential repeats one object in a loop with the copies as the printer's target", "[FlatBelt]")
{
    // One 20 mm cube, 5 copies, 50 mm gap: one piece in the loop, then a belt advance of its depth plus
    // the gap; the printer stops after five.
    DynamicPrintConfig config = belt_config("sequential", false);
    config.set_deserialize_strict({ { "print_sequence", "by object" }, { "flat_belt_copies", 5 }, { "flat_belt_object_gap", 50 } });
    const std::string gcode = slice_cubes(config, 1);
    CHECK(count_lines_starting(gcode, "FLAT_BELT_INFO MODE=sequential TARGET=5 LOOP=1 FILE_UNITS=1") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_BEGIN COUNT=0") == 1);
    CHECK(count_lines_starting(gcode, "FLAT_BELT_UNIT_DONE") == 1);
    REQUIRE(count_lines_starting(gcode, "BELT_ADVANCE DIST=") == 1);
    const size_t at = gcode.find("\nBELT_ADVANCE DIST=");
    CHECK_THAT(std::atof(gcode.c_str() + at + 19), WithinAbs(20. + 50., 1.));
    // The loop body starts and ends lifted above the piece: the advance and the next piece's start
    // happen clear of it.
    CHECK(gcode.find("SDCARD_LOOP_END") > at);
}

TEST_CASE("Sequential with several objects prints each piece once and reports it", "[FlatBelt]")
{
    // Two cubes, 2 copies each: four pieces written out, no loop, three advances between them.
    DynamicPrintConfig config = belt_config("sequential", false);
    config.set_deserialize_strict({ { "print_sequence", "by object" }, { "flat_belt_copies", 2 } });
    const std::string gcode = slice_cubes(config, 2);
    CHECK(count_lines_starting(gcode, "FLAT_BELT_INFO MODE=sequential TARGET=4 LOOP=0") == 1);
    CHECK(count_lines_starting(gcode, "SDCARD_LOOP_BEGIN") == 0);
    CHECK(count_lines_starting(gcode, "FLAT_BELT_UNIT_DONE") == 4);
    CHECK(count_lines_starting(gcode, "BELT_ADVANCE DIST=") == 3);
}

TEST_CASE("Sequential clears the path conflict found by the previous slice", "[FlatBelt]")
{
    // Two overlapping cubes conflict when printed layer by layer. Sequential skips the
    // check, and must not report the stale conflict, whose instances are gone.
    Model model;
    for (int i = 0; i < 2; ++i) {
        ModelObject *object = model.add_object();
        object->name = "cube.stl";
        object->add_volume(make_cube(20., 20., HEIGHT));
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(BED / 2. + 10. * i, BED / 2., HEIGHT / 2.));
        object->ensure_on_bed();
    }
    Print print;
    print.apply(model, belt_config("plates", false));
    Test::gcode(print);
    REQUIRE(print.get_conflict_result().has_value());

    DynamicPrintConfig config = belt_config("sequential", false);
    config.set_deserialize_strict({ { "print_sequence", "by object" } });
    print.apply(model, config);
    Test::gcode(print);
    CHECK_FALSE(print.get_conflict_result().has_value());
}

TEST_CASE("Sequential prints every copy with its brim at the bed centre", "[FlatBelt]")
{
    // Two cubes 40 mm apart on the plate, 2 copies each, with a brim: all four pieces print
    // at the bed centre, each with its own brim.
    DynamicPrintConfig config = belt_config("sequential", false);
    config.set_deserialize_strict({ { "print_sequence", "by object" }, { "flat_belt_copies", 2 },
                                    { "brim_type", "outer_only" }, { "brim_width", 3 } });
    const std::string gcode = slice_cubes(config, 2);

    struct Box { double x_min, x_max, y_min, y_max; };
    std::vector<Box> pieces(1, Box{ 1e9, -1e9, 1e9, -1e9 });
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("BELT_ADVANCE", 0) == 0)
            pieces.push_back(Box{ 1e9, -1e9, 1e9, -1e9 });
        if (line.rfind("G1 ", 0) != 0)
            continue;
        const std::string cmd = line.substr(0, line.find(';'));
        const double e = param(cmd, 'E'), x = param(cmd, 'X'), y = param(cmd, 'Y');
        if (!(e > 0.) || x != x || y != y)
            continue;
        Box &b = pieces.back();
        b.x_min = std::min(b.x_min, x); b.x_max = std::max(b.x_max, x);
        b.y_min = std::min(b.y_min, y); b.y_max = std::max(b.y_max, y);
    }
    REQUIRE(pieces.size() == 4);
    for (const Box &b : pieces) {
        CHECK_THAT((b.x_min + b.x_max) / 2., Catch::Matchers::WithinAbs(BED / 2., 1.));
        CHECK_THAT((b.y_min + b.y_max) / 2., Catch::Matchers::WithinAbs(BED / 2., 1.));
        CHECK(b.x_max - b.x_min > 24.); // 20 mm cube and its 3 mm brim
    }
}

TEST_CASE("Fixed bed ejects the part after the print when Eject is on", "[FlatBelt]")
{
    DynamicPrintConfig config = belt_config("fixed_bed", false);
    SECTION("off by default") {
        CHECK(slice_cubes(config, 1).find("\n; FLAT_BELT_EJECT\n") == std::string::npos); // the config dump names it too
    }
    SECTION("after a set time, at the end of the file") {
        config.set_deserialize_strict({ { "flat_belt_eject", "time" }, { "flat_belt_eject_time", 10 } });
        const std::string gcode = slice_cubes(config, 1);
        CHECK(gcode.find("\nFLAT_BELT_INFO MODE=fixed TARGET=0 LOOP=0 FILE_UNITS=0 UNIT=piece EJECT=time\n") != std::string::npos);
        const size_t      at    = gcode.find("FLAT_BELT_EJECT WAIT=600 BED_TEMP=0");
        REQUIRE(at != std::string::npos);
        CHECK(gcode.rfind(";LAYER_CHANGE") < at); // after the last layer
    }
    SECTION("once the bed has cooled") {
        config.set_deserialize_strict({ { "flat_belt_eject", "bed_temp" }, { "flat_belt_eject_bed_temp", 35 } });
        CHECK(slice_cubes(config, 1).find("FLAT_BELT_EJECT WAIT=0 BED_TEMP=35") != std::string::npos);
    }
    SECTION("only in Fixed bed mode") {
        config = belt_config("plates", false);
        config.set_deserialize_strict({ { "flat_belt_eject", "time" } });
        CHECK(slice_cubes(config, 1).find("\n; FLAT_BELT_EJECT\n") == std::string::npos); // the config dump names it too
    }
}

TEST_CASE("Long parts offers Fixed bed for a part that fits on the bed", "[FlatBelt]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(20., 20., HEIGHT));
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(BED / 2., BED / 2., HEIGHT / 2.));
    object->ensure_on_bed();
    for (bool endless : { false, true }) {
        Print print;
        print.apply(model, belt_config("long_parts", endless));
        const StringObjectException err = print.validate();
        CHECK(err.type == STRING_EXCEPT_FLAT_BELT_FITS_BED);
        CHECK(err.string.find("Fixed bed is enough") != std::string::npos);
    }
}

TEST_CASE("Long parts names the length limit of one object", "[FlatBelt]")
{
    // Coordinates are 32-bit around the object's centre: at the default 1 nm resolution
    // an object stops at ~4.29 m. A 5 m part gets a message, not an empty first layer.
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(20., 5000., 2.));
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(BED / 2., BED / 2., 1.));
    object->ensure_on_bed();
    Print print;
    print.apply(model, belt_config("long_parts", false));
    const StringObjectException err = print.validate();
    CHECK(err.string.find("at most 4.2 m") != std::string::npos);
}
