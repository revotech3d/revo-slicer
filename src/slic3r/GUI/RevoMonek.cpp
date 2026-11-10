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

namespace {
std::string base_key(const std::string& opt_id) { return opt_id.substr(0, opt_id.find('#')); }

std::set<std::string> revealed()
{
    std::set<std::string> out;
    const std::string     v = wxGetApp().app_config->get("revo_monek_odkryte");
    size_t                a = 0;
    while (a < v.size()) {
        size_t b = v.find(';', a);
        if (b == std::string::npos) b = v.size();
        if (b > a) out.insert(v.substr(a, b - a));
        a = b + 1;
    }
    return out;
}
} // namespace

bool allows(const std::string& opt_id)
{
    if (!active())
        return true;
    const std::string k = base_key(opt_id);
    return keys().count(k) > 0 || revealed().count(k) > 0;
}

bool reveal(const std::string& opt_id)
{
    if (allows(opt_id))
        return false;
    std::string v = wxGetApp().app_config->get("revo_monek_odkryte");
    v += (v.empty() ? "" : ";") + base_key(opt_id);
    wxGetApp().app_config->set("revo_monek_odkryte", v);
    return true;
}

} // namespace Slic3r::GUI::Revo::Monek
