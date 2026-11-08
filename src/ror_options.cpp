// Port options: port.ini in the working folder (ROR_PORT_INI overrides the path). Lines "name = value", '#' comments.
// Without the file every option is off, which is how the regression tools run.
//
//   skip_intro = 1   boot straight to the title's "press Start": the logo and notice pages, CP_LOGO.PSS and the attract
//                    movie MV00_AD.PSS are skipped. It sets the game's own flag 0x2ac1cc: the title state machine
//                    0x1aaae4 goes from state 0 straight to 0x16 ("press Start") when it is 1, and sets it in state 1,
//                    so the game itself shows the intro only on the first visit to the title.
//
//   fullscreen = 1                 borderless fullscreen at start (F11 toggles it in the game; ror_window.cpp)
//   window_width / window_height   the window's client size at start (default the runtime's 640x448; it stays
//                                  resizable, and PCSX2's GS follows the size: ror_pcsx2host_win32.cpp)
//   volume = 0..100                raylib's master volume: the game's sound and the movies
//
// The fast-forward key (hold to run the game at 4x) is a pad.ini entry, fast_forward (ror_pad.cpp). The launcher
// (launcher/) writes this file from its settings window.
#include "ror.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace
{
    constexpr uint32_t kIntroSeen = 0x2ac1cc;

    const std::map<std::string, std::string> &options()
    {
        static const std::map<std::string, std::string> opts = []
        {
            std::map<std::string, std::string> m;
            const char *env = std::getenv("ROR_PORT_INI");
            const std::string path = env && *env ? env : "port.ini";
            std::ifstream in(path);
            for (std::string line; std::getline(in, line);)
            {
                if (const size_t hash = line.find('#'); hash != std::string::npos)
                    line.resize(hash);
                const size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                auto trim = [](std::string s)
                {
                    s.erase(0, s.find_first_not_of(" \t\r"));
                    s.erase(s.find_last_not_of(" \t\r") + 1);
                    return s;
                };
                m[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
            if (in.is_open() || !m.empty())
                for (const auto &[k, v] : m)
                    ror::log("option %s = %s (%s)", k.c_str(), v.c_str(), path.c_str());
            return m;
        }();
        return opts;
    }

    bool flag(const char *name)
    {
        const auto it = options().find(name);
        return it != options().end() && !it->second.empty() && it->second[0] == '1';
    }
}

namespace ror
{
    std::string option(const char *name, const char *fallback)
    {
        const auto it = options().find(name);
        return it != options().end() && !it->second.empty() ? it->second : std::string(fallback);
    }

    int optionInt(const char *name, int fallback)
    {
        const std::string v = option(name, "");
        char *end = nullptr;
        const long n = v.empty() ? fallback : std::strtol(v.c_str(), &end, 10);
        return v.empty() || (end && *end) ? fallback : static_cast<int>(n);
    }

    void optionsVblank(uint8_t *rdram)
    {
        static const bool skipIntro = flag("skip_intro");
        if (skipIntro && frame() < 400 && rd32(rdram, kIntroSeen) == 0)
            wr32(rdram, kIntroSeen, 1);
    }
}
