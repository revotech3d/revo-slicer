#pragma once

#include "GLModel.hpp"
#include "GLTexture.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// [r]evo flat belt: draws the belt around the current plate the way the selected Belt mode uses
// it, in the 3D view and in the print preview. Sequential and Plates show the copies still to
// come as grey ghosts along the belt, fading towards the front; Long parts shows a longer belt,
// Endless the infinity sign behind it; Fixed bed shows the eject arrow when Eject is on. The mode
// is named along the left side of the plate. Nothing is drawn for other printers.
class FlatBeltPreview
{
public:
    void render(const Transform3d &view_matrix, const Transform3d &projection_matrix, bool bottom);

private:
    struct Ghost
    {
        BoundingBoxf3 box;
        float         alpha;
    };

    void render_label(const Transform3d &view_matrix, const Transform3d &projection_matrix,
                      const std::string &text, const BoundingBoxf &bed);

    GLModel     m_box_faces; // unit cube, scaled and moved per ghost
    GLModel     m_box_edges;
    GLModel     m_quad;      // unit square, the belt strip pieces
    GLModel     m_infinity;
    GLModel     m_arrow;
    GLModel     m_label_quad;
    GLTexture   m_label_texture;
    std::string m_label_text;
    BoundingBoxf m_label_bed;
};

} // namespace GUI
} // namespace Slic3r
