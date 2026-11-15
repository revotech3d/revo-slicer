#include "FlatBeltPreview.hpp"

#include "3DScene.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/Color.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <glad/gl.h>
#include <wx/font.h>
#include <wx/string.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace Slic3r {
namespace GUI {

namespace {

const ColorRGBA BELT_COLOR   { 0.13f, 0.15f, 0.17f, 1.0f };
const ColorRGBA GHOST_COLOR  { 0.62f, 0.66f, 0.71f, 1.0f };
const ColorRGBA EDGE_COLOR   { 0.85f, 0.88f, 0.92f, 1.0f };
const ColorRGBA ACCENT_COLOR { 0.353f, 0.627f, 1.0f, 1.0f }; // #5AA0FF

constexpr int    MAX_GHOSTS     = 8;   // copies drawn along the belt, the rest is in the fog
constexpr int    MAX_PLATES     = 6;
constexpr double BELT_MARGIN    = 12.; // belt wider than the bed on each side, mm
constexpr double LABEL_HEIGHT   = 12.; // mode name along the left side, mm
constexpr double FOG_ALPHA      = 0.5; // ghost opacity next to the bed
constexpr int    FOG_SLICES     = 10;

ColorRGBA with_alpha(ColorRGBA c, float a)
{
    c.a(a);
    return c;
}

void init_unit_quad(GLModel &model)
{
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    g.add_vertex(Vec3f(0.f, 0.f, 0.f));
    g.add_vertex(Vec3f(1.f, 0.f, 0.f));
    g.add_vertex(Vec3f(1.f, 1.f, 0.f));
    g.add_vertex(Vec3f(0.f, 1.f, 0.f));
    g.add_triangle(0, 1, 2);
    g.add_triangle(0, 2, 3);
    model.init_from(std::move(g));
}

void init_unit_box(GLModel &faces, GLModel &edges)
{
    GLModel::Geometry f, e;
    f.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    e.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    for (int i = 0; i < 8; ++i) {
        const Vec3f v(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
        f.add_vertex(v);
        e.add_vertex(v);
    }
    const unsigned int tris[12][3] = { { 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 }, { 0, 1, 4 }, { 1, 5, 4 },
                                       { 2, 6, 3 }, { 3, 6, 7 }, { 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
    for (const auto &t : tris)
        f.add_triangle(t[0], t[1], t[2]);
    const unsigned int lines[12][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 },
                                        { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    for (const auto &l : lines)
        e.add_line(l[0], l[1]);
    faces.init_from(std::move(f));
    edges.init_from(std::move(e));
}

// Lemniscate of Bernoulli as a flat ribbon, 2 units wide.
void init_infinity(GLModel &model)
{
    constexpr int   N = 160;
    constexpr float W = 0.035f;
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    auto point = [](float t) {
        const float s = std::sin(t), c = std::cos(t), d = 1.f + s * s;
        return Vec2f(c / d, s * c / d);
    };
    for (int i = 0; i <= N; ++i) {
        const float t = 2.f * float(PI) * float(i) / float(N);
        const Vec2f p = point(t), n = (point(t + 0.01f) - point(t - 0.01f)).normalized();
        const Vec2f side(-n.y() * W, n.x() * W);
        g.add_vertex(Vec3f(p.x() + side.x(), p.y() + side.y(), 0.f));
        g.add_vertex(Vec3f(p.x() - side.x(), p.y() - side.y(), 0.f));
        if (i > 0) {
            const unsigned int a = 2 * (i - 1);
            g.add_triangle(a, a + 1, a + 2);
            g.add_triangle(a + 1, a + 3, a + 2);
        }
    }
    model.init_from(std::move(g));
}

// Arrow pointing to -Y (the front of the machine), 1 unit long.
void init_arrow(GLModel &model)
{
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
    for (const Vec3f &v : { Vec3f(-0.04f, 0.f, 0.f), Vec3f(0.04f, 0.f, 0.f), Vec3f(0.04f, -0.7f, 0.f), Vec3f(-0.04f, -0.7f, 0.f),
                            Vec3f(-0.14f, -0.7f, 0.f), Vec3f(0.14f, -0.7f, 0.f), Vec3f(0.f, -1.f, 0.f) })
        g.add_vertex(v);
    g.add_triangle(0, 2, 1);
    g.add_triangle(0, 3, 2);
    g.add_triangle(4, 6, 5);
    model.init_from(std::move(g));
}

Transform3d box_transform(const BoundingBoxf3 &box)
{
    return Geometry::translation_transform(box.min) * Geometry::scale_transform(box.size().cwiseMax(Vec3d(0.01, 0.01, 0.01)));
}

// Letter-spaced capitals, as in the [r]evo launch style.
wxString spaced_caps(const wxString &text)
{
    const wxString upper = text.Upper();
    wxString       out;
    for (size_t i = 0; i < upper.length(); ++i) {
        if (i > 0)
            out += wxString::FromUTF8("\xE2\x80\x89"); // thin space
        out += upper[i];
    }
    return out;
}

} // namespace

void FlatBeltPreview::render(const Transform3d &view_matrix, const Transform3d &projection_matrix, bool bottom)
{
    GUI_App &app = wxGetApp();
    if (bottom || app.preset_bundle == nullptr || app.plater() == nullptr)
        return;
    const DynamicPrintConfig &printer = app.preset_bundle->printers.get_edited_preset().config;
    const auto               *flat    = printer.option<ConfigOptionBool>("flat_belt");
    if (flat == nullptr || !flat->value)
        return;
    const DynamicPrintConfig &print = app.preset_bundle->prints.get_edited_preset().config;
    const auto               *mode_opt = print.option<ConfigOptionEnum<FlatBeltMode>>("flat_belt_mode");
    if (mode_opt == nullptr)
        return;
    const FlatBeltMode  mode   = mode_opt->value;
    const double        gap    = print.opt_float("flat_belt_object_gap");
    const int           copies = print.opt_int("flat_belt_copies");
    const FlatBeltEject eject  = print.opt_enum<FlatBeltEject>("flat_belt_eject");

    PartPlate *plate = app.plater()->get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        return;
    const BoundingBoxf3 &plate_box = plate->get_bounding_box();
    const BoundingBoxf   bed(Vec2d(plate_box.min.x(), plate_box.min.y()), Vec2d(plate_box.max.x(), plate_box.max.y()));
    const Vec2d          center = bed.center();

    // The objects on this plate, in the order they print.
    std::vector<BoundingBoxf3> objects;
    const Model &model = app.plater()->model();
    for (int o = 0; o < int(model.objects.size()); ++o)
        for (int i = 0; i < int(model.objects[o]->instances.size()); ++i)
            if (model.objects[o]->instances[i]->printable && plate->contain_instance(o, i))
                objects.push_back(model.objects[o]->instance_bounding_box(size_t(i)));

    // Ghosts: the copies still to come, laid out along the belt in front of the bed the way the
    // belt carries them.
    std::vector<Ghost> ghosts;
    if (mode == FlatBeltMode::Sequential && !objects.empty()) {
        const size_t per_object = size_t(std::max(1, copies));
        const size_t total      = objects.size() * per_object;
        double       y          = center.y();
        double       prev_depth = objects.front().size().y();
        for (size_t k = 1; k < total && k <= MAX_GHOSTS; ++k) {
            const Vec3d s = objects[k / per_object].size();
            y -= prev_depth / 2. + gap + s.y() / 2.;
            prev_depth = s.y();
            ghosts.push_back({ BoundingBoxf3(Vec3d(center.x() - s.x() / 2., y - s.y() / 2., 0.),
                                             Vec3d(center.x() + s.x() / 2., y + s.y() / 2., s.z())), 0.f });
        }
    } else if (mode == FlatBeltMode::Plates && !objects.empty()) {
        BoundingBoxf3 all;
        for (const BoundingBoxf3 &o : objects)
            all.merge(o);
        const double advance = all.size().y() + gap;
        const int    plates  = copies == 0 ? MAX_PLATES : std::min(copies - 1, MAX_PLATES);
        for (int k = 1; k <= plates; ++k)
            for (BoundingBoxf3 o : objects) {
                o.translate(0., -k * advance, 0.);
                ghosts.push_back({ o, 0.f });
            }
    }

    // How far in front of the bed the belt is drawn before it fades out.
    double reach = 0.;
    if (mode == FlatBeltMode::FixedBed)
        reach = eject == FlatBeltEject::Off ? 0. : 130.;
    else {
        reach = 300.;
        for (const Ghost &g : ghosts)
            reach = std::max(reach, bed.min.y() - g.box.min.y() + 60.);
        for (const BoundingBoxf3 &o : objects)
            reach = std::max(reach, bed.min.y() - o.min.y() + 150.);
    }
    const double far_y = bed.min.y() - reach;
    for (Ghost &g : ghosts) {
        const double f = std::clamp((center.y() - g.box.center().y()) / (center.y() - far_y), 0., 1.);
        g.alpha = float(FOG_ALPHA * (1. - f) + 0.04);
    }

    GLShaderProgram *shader = app.get_shader("flat");
    if (shader == nullptr)
        return;
    if (!m_quad.is_initialized()) {
        init_unit_quad(m_quad);
        init_unit_box(m_box_faces, m_box_edges);
        init_infinity(m_infinity);
        init_arrow(m_arrow);
    }

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDepthMask(GL_FALSE));
    shader->start_using();
    shader->set_uniform("projection_matrix", projection_matrix);
    auto draw = [&](GLModel &m, const Transform3d &t, const ColorRGBA &color) {
        shader->set_uniform("view_model_matrix", view_matrix * t);
        m.set_color(color);
        m.render();
    };
    auto quad = [&](double x0, double y0, double x1, double y1, double z, const ColorRGBA &color) {
        draw(m_quad, Geometry::translation_transform(Vec3d(x0, y0, z)) * Geometry::scale_transform(Vec3d(x1 - x0, y1 - y0, 1.)), color);
    };

    // The belt under the bed, and in front of it fading into the fog.
    const double x0 = bed.min.x() - BELT_MARGIN, x1 = bed.max.x() + BELT_MARGIN, z = -0.6;
    quad(x0, bed.min.y(), x1, bed.max.y() + BELT_MARGIN, z, BELT_COLOR);
    for (int i = 0; i < FOG_SLICES && reach > 0.; ++i) {
        const double y1 = bed.min.y() - reach * i / FOG_SLICES, y0 = bed.min.y() - reach * (i + 1) / FOG_SLICES;
        quad(x0, y0, x1, y1, z, with_alpha(BELT_COLOR, 1.f - float(i) / FOG_SLICES));
    }

    for (const Ghost &g : ghosts) {
        draw(m_box_faces, box_transform(g.box), with_alpha(GHOST_COLOR, g.alpha));
        draw(m_box_edges, box_transform(g.box), with_alpha(EDGE_COLOR, std::min(1.f, g.alpha * 1.6f)));
    }

    if (mode == FlatBeltMode::LongParts && print.opt_bool("flat_belt_endless"))
        draw(m_infinity, Geometry::translation_transform(Vec3d(center.x(), bed.max.y() + 60., 0.2)) * Geometry::scale_transform(70.),
             with_alpha(ACCENT_COLOR, 0.9f));
    if (mode == FlatBeltMode::FixedBed && eject != FlatBeltEject::Off)
        draw(m_arrow, Geometry::translation_transform(Vec3d(center.x(), bed.min.y() - 10., 0.2)) * Geometry::scale_transform(100.),
             with_alpha(ACCENT_COLOR, 0.85f));

    shader->stop_using();
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_BLEND));

    // The mode name along the left side of the bed.
    wxString text;
    switch (mode) {
    case FlatBeltMode::FixedBed:
        if (eject == FlatBeltEject::AfterTime) {
            const int mins = print.opt_int("flat_belt_eject_time");
            // TRN 3D view, flat belt: mode name along the bed; the belt ejects the part %d minutes after the print
            text = wxString::Format(_L_PLURAL("Fixed bed · eject after %d minute", "Fixed bed · eject after %d minutes", mins), mins);
        } else if (eject == FlatBeltEject::AfterBedTemp)
            // TRN 3D view, flat belt: mode name along the bed; the belt ejects the part once the bed is at %d °C
            text = wxString::Format(_L("Fixed bed · eject at bed %d °C"), print.opt_int("flat_belt_eject_bed_temp"));
        else
            text = _L("Fixed bed");
        break;
    case FlatBeltMode::Sequential: {
        const int n = int(objects.size()) * std::max(1, copies);
        // TRN 3D view, flat belt: mode name along the bed and the number of pieces printed one after another
        text = wxString::Format(_L_PLURAL("Sequential · %d piece", "Sequential · %d pieces", n), n);
        break;
    }
    case FlatBeltMode::Plates:
        // TRN 3D view, flat belt: mode name along the bed and how many times the whole plate is printed
        text = copies == 0 ? _L("Plates · until stopped") :
                             wxString::Format(_L_PLURAL("Plates · %d plate", "Plates · %d plates", copies), copies);
        break;
    case FlatBeltMode::LongParts:
        // TRN 3D view, flat belt: mode name along the bed; Endless repeats the part until the print is stopped
        text = print.opt_bool("flat_belt_endless") ? _L("Long parts · endless") : _L("Long parts");
        break;
    default: break;
    }
    if (!text.empty())
        render_label(view_matrix, projection_matrix, spaced_caps(text).ToUTF8().data(), bed);
}

void FlatBeltPreview::render_label(const Transform3d &view_matrix, const Transform3d &projection_matrix,
                                   const std::string &text, const BoundingBoxf &bed)
{
    if (text != m_label_text || bed.min != m_label_bed.min || bed.max != m_label_bed.max) {
        m_label_text = text;
        m_label_bed  = bed;
        m_label_texture.reset();
        m_label_quad.reset();
        wxFont font = Label::sysFont(wxGetApp().em_unit() * 4, true);
        if (!m_label_texture.generate_from_text_string(text, font, *wxBLACK, wxColour(0x5a, 0xa0, 0xff)))
            return;
        // Reads from the front to the back, the tops of the letters facing out.
        const double depth  = bed.size().y() - 20.;
        double       height = LABEL_HEIGHT;
        double       width  = height * m_label_texture.get_width() / std::max(1, m_label_texture.get_height());
        if (width > depth) {
            height *= depth / width;
            width = depth;
        }
        const float base = float(bed.min.x() - 6.), top = float(bed.min.x() - 6. - height);
        const float y0 = float(bed.min.y() + 10.), y1 = float(bed.min.y() + 10. + width), z = 0.1f;
        GLModel::Geometry g;
        g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3T2 };
        g.add_vertex(Vec3f(base, y0, z), Vec2f(0.f, 1.f));
        g.add_vertex(Vec3f(base, y1, z), Vec2f(1.f, 1.f));
        g.add_vertex(Vec3f(top, y1, z), Vec2f(1.f, 0.f));
        g.add_vertex(Vec3f(top, y0, z), Vec2f(0.f, 0.f));
        g.add_triangle(0, 1, 2);
        g.add_triangle(0, 2, 3);
        m_label_quad.init_from(std::move(g));
    }
    if (m_label_texture.get_id() == 0 || !m_label_quad.is_initialized())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("printbed");
    if (shader == nullptr)
        return;
    shader->start_using();
    shader->set_uniform("view_model_matrix", view_matrix);
    shader->set_uniform("projection_matrix", projection_matrix);
    shader->set_uniform("transparent_background", false);
    shader->set_uniform("svg_source", 0);
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glBindTexture(GL_TEXTURE_2D, GLuint(m_label_texture.get_id())));
    m_label_quad.render();
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDepthMask(GL_TRUE));
    shader->stop_using();
}

} // namespace GUI
} // namespace Slic3r
