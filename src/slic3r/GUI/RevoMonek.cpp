#include "RevoMonek.hpp"

#include <fstream>
#include <set>
#include <string>

#include <boost/algorithm/string/trim.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r::GUI::Revo::Monek {

namespace {
const std::set<std::string>& keys()
{
    static const std::set<std::string> list = [] {
        std::set<std::string> out;
        std::ifstream         f(resources_dir() + "/revo/monek.txt");
        std::string           line;
        while (std::getline(f, line)) {
            boost::algorithm::trim(line);
            if (!line.empty() && line[0] != '#')
                out.insert(line);
        }
        return out;
    }();
    return list;
}
} // namespace

bool active() { return wxGetApp().app_config && wxGetApp().app_config->get("user_mode") == "monek"; }

bool allows(const std::string& opt_id)
{
    if (!active())
        return true;
    return keys().count(opt_id.substr(0, opt_id.find('#'))) > 0;
}

} // namespace Slic3r::GUI::Revo::Monek
