#pragma once

#include <string>

#include <wx/string.h>

#include "libslic3r/Preset.hpp"
#include "I18N.hpp"

namespace Slic3r { namespace GUI {

// [r]evo Slicer: Revon printers use four plate types. Two of them reuse Orca plate types that Revon
// printers have no use for, so projects and profiles keep the same keys and stay readable by Orca:
// Engineering Plate (eng_plate_temp) is the stainless steel plate, Textured Cool Plate
// (textured_cool_plate_temp) is the belt of the Revon Belt.
inline bool revo_printer_model(const VendorProfile::PrinterModel *pm) { return pm && pm->family == "Revon"; }

// Label of a curr_bed_type enum label for the current printer.
inline wxString revo_bed_label(const std::string &label, bool revo)
{
    if (revo && label == "Engineering Plate")
        return _L("Stainless Steel Plate");
    if (revo && label == "Textured Cool Plate")
        return _L("Belt");
    if (revo && label == "Smooth High Temp Plate")
        return _L("Smooth PEI Plate");
    return _L(label);
}

}} // namespace Slic3r::GUI
