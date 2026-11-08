// Rule of Rose PC port: the playtest package's launcher (RuleOfRose.exe, built and packaged by build.py dist).
//
// A settings window (Win32 common controls, no other libraries). The package holds no game files: on the first Play
// the launcher asks for the player's own disc image (ISO 9660, 2048-byte sectors), checks that it is the release this
// build was recompiled from (SHA-256 of its SLES_542.18, the PAL disc SLES-54218), and extracts it into game\ next to
// the launcher, with a progress bar. It then runs bin\ps2EntryRunner.exe on game\SLES_542.18 (the runner's disc root
// is the ELF's folder, src/ror_file.cpp) in user\, with the memory card user\memcard (src/ror_mc.cpp) and one log
// folder per session, user\logs\<date-time>\ (ror.log, stdout.txt, stderr.txt), hiding itself until the game exits.
//
// Settings are files next to the launcher, which it reads at start and writes before each Play:
//   port.ini   skip_intro, fullscreen, window_width, window_height, volume (src/ror_options.cpp, src/ror_window.cpp);
//              movies (0: the launcher sets ROR_MOVIES=0, src/ror_movie.cpp)
//   gs.ini     [gs] renderer, upscale, texture_filtering (pcsx2-ror/gs.cpp); [EmuCore/GS] AspectRatio (PCSX2's)
//   pad.ini    the controls (src/ror_pad.cpp), opened in Notepad by "Controls..."
//
//   RuleOfRose.exe [--iso PATH] [--reset] [--play]   --iso: the image for the next extraction; --reset: extract again;
//                                                     --play: start the game at once, no window (tests)
// ROR_* variables already set in the environment are passed through unchanged (tests set ROR_MAX_FRAMES etc.).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "      \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace
{
    // SHA-256 of SLES_542.18 on the PAL disc (SLES-54218) the port is recompiled from. A different release has
    // different code at different addresses, which the recompiled program cannot run.
    constexpr char kElfSha256[] = "71417965b8931cadcebc33f48439df3290822316e4c139d9c4c7a4b6d7fb7692";
    constexpr char kElfName[] = "SLES_542.18";
    constexpr uint32_t kSector = 2048;
    constexpr wchar_t kTitle[] = L"Rule of Rose - PC port (playtest)";

    fs::path g_root, g_game, g_user, g_runner;

    // ---------------------------------------------------------------- disc image ----------------------------------

    struct Entry
    {
        std::string path;   // '/'-separated, as on the disc (upper case), without the ";1" version
        uint32_t lba = 0, size = 0;
    };

    std::string sha256(const std::vector<uint8_t> &data)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        uint8_t digest[32] = {};
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return {};
        BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), digest,
                   sizeof digest);
        BCryptCloseAlgorithmProvider(alg, 0);
        std::string hex;
        char b[3];
        for (uint8_t v : digest)
        {
            std::snprintf(b, sizeof b, "%02x", v);
            hex += b;
        }
        return hex;
    }

    bool readFile(const fs::path &p, std::vector<uint8_t> &out)
    {
        FILE *f = _wfopen(p.c_str(), L"rb");
        if (!f)
            return false;
        _fseeki64(f, 0, SEEK_END);
        out.resize(static_cast<size_t>(_ftelli64(f)));
        _fseeki64(f, 0, SEEK_SET);
        const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    }

    bool readAt(FILE *f, uint64_t off, void *buf, size_t n)
    {
        return _fseeki64(f, static_cast<int64_t>(off), SEEK_SET) == 0 && std::fread(buf, 1, n, f) == n;
    }

    uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

    // ISO 9660 directory records: length at +0, extent LBA +2, data length +10, flags +25 (2 = directory), name
    // length +32, name +33; "." and ".." are the names 00 and 01. Records do not cross sector boundaries.
    bool walk(FILE *f, uint32_t lba, uint32_t size, const std::string &prefix, std::vector<Entry> &out, int depth)
    {
        if (depth > 16)
            return false;
        std::vector<uint8_t> dir(size);
        if (!readAt(f, static_cast<uint64_t>(lba) * kSector, dir.data(), size))
            return false;
        for (uint32_t i = 0; i < size;)
        {
            const uint8_t len = dir[i];
            if (len == 0)
            {
                i = (i / kSector + 1) * kSector;
                continue;
            }
            if (i + len > size || len < 34)
                return false;
            const uint8_t *r = &dir[i];
            const uint8_t nameLen = r[32];
            if (!(nameLen == 1 && (r[33] == 0 || r[33] == 1)))
            {
                std::string name(reinterpret_cast<const char *>(r + 33), nameLen);
                if (const size_t semi = name.find(';'); semi != std::string::npos)
                    name.resize(semi);
                if (!name.empty() && name.back() == '.')
                    name.pop_back();
                if (name.empty() || name.find("..") != std::string::npos || name.find_first_of("/\\:") != std::string::npos)
                    return false;
                const std::string path = prefix.empty() ? name : prefix + "/" + name;
                if (r[25] & 2)
                {
                    if (!walk(f, le32(r + 2), le32(r + 10), path, out, depth + 1))
                        return false;
                }
                else
                    out.push_back({path, le32(r + 2), le32(r + 10)});
            }
            i += len;
        }
        return true;
    }

    // The disc image -> dst (a fresh folder). Empty on success, else what went wrong. progress(0..1000).
    template <typename Progress>
    std::wstring extractIso(const fs::path &iso, const fs::path &dst, Progress progress)
    {
        FILE *f = _wfopen(iso.c_str(), L"rb");
        if (!f)
            return L"Cannot open " + iso.wstring();
        struct Closer
        {
            FILE *f;
            ~Closer() { std::fclose(f); }
        } closer{f};
        uint8_t pvd[kSector];
        if (!readAt(f, 16ull * kSector, pvd, kSector) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
            return L"This file is not an ISO 9660 disc image with 2048-byte sectors (a .iso made from the DVD).";
        std::vector<Entry> files;
        if (!walk(f, le32(pvd + 156 + 2), le32(pvd + 156 + 10), "", files, 0))
            return L"The disc image's directory could not be read.";
        const Entry *elf = nullptr;
        uint64_t total = 0;
        for (const Entry &e : files)
        {
            total += e.size;
            if (e.path == kElfName)
                elf = &e;
        }
        std::vector<uint8_t> elfData(elf ? elf->size : 0);
        if (!elf || !readAt(f, static_cast<uint64_t>(elf->lba) * kSector, elfData.data(), elfData.size()) ||
            sha256(elfData) != kElfSha256)
            return L"This disc image is not the release this build needs: Rule of Rose, PAL, SLES-54218 (its "
                   L"SLES_542.18 must match). Other releases are not supported.";
        ULARGE_INTEGER freeBytes{};
        if (GetDiskFreeSpaceExW(dst.parent_path().c_str(), &freeBytes, nullptr, nullptr) &&
            freeBytes.QuadPart < total + (256ull << 20))
            return L"Not enough free disk space next to RuleOfRose.exe: the game's files need " +
                   std::to_wstring(total >> 20) + L" MB.";
        std::vector<uint8_t> buf(8u << 20);
        uint64_t done = 0;
        for (const Entry &e : files)
        {
            const fs::path out = dst / fs::path(e.path);
            std::error_code ec;
            fs::create_directories(out.parent_path(), ec);
            FILE *o = _wfopen(out.c_str(), L"wb");
            if (!o)
                return L"Cannot write " + out.wstring();
            for (uint64_t pos = 0; pos < e.size;)
            {
                const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), e.size - pos));
                if (!readAt(f, static_cast<uint64_t>(e.lba) * kSector + pos, buf.data(), n) ||
                    std::fwrite(buf.data(), 1, n, o) != n)
                {
                    std::fclose(o);
                    return L"Reading the disc image or writing " + out.wstring() + L" failed (disk full?).";
                }
                pos += n;
                done += n;
                progress(total ? static_cast<int>(done * 1000 / total) : 1000);
            }
            std::fclose(o);
        }
        return {};
    }

    bool gameReady()
    {
        std::vector<uint8_t> elf;
        return readFile(g_game / kElfName, elf) && sha256(elf) == kElfSha256;
    }

    // Extracts into game.partial, then renames it to game. Empty on success.
    template <typename Progress>
    std::wstring installFrom(const fs::path &iso, Progress progress)
    {
        const fs::path tmp = g_root / L"game.partial";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        std::wstring err = extractIso(iso, tmp, progress);
        if (!err.empty())
        {
            fs::remove_all(tmp, ec);
            return err;
        }
        fs::remove_all(g_game, ec);
        fs::rename(tmp, g_game, ec);
        if (ec || !gameReady())
            return L"The extracted files could not be put in place: " + g_game.wstring();
        return {};
    }

    // ---------------------------------------------------------------- settings ------------------------------------

    struct Settings
    {
        bool fullscreen = false;
        int width = 1280, height = 896;
        std::string aspect = "4:3";             // PCSX2's AspectRatio name
        std::string renderer = "auto", filtering = "ps2";
        int upscale = 2;
        bool skipIntro = true, movies = true;
        int volume = 100;
    };

    std::map<std::string, std::string> readIni(const fs::path &p)
    {
        std::map<std::string, std::string> m;
        std::ifstream in(p);
        std::string section;
        for (std::string line; std::getline(in, line);)
        {
            if (const size_t c = line.find_first_of("#;"); c != std::string::npos)
                line.resize(c);
            auto trim = [](std::string s)
            {
                s.erase(0, s.find_first_not_of(" \t\r"));
                s.erase(s.find_last_not_of(" \t\r") + 1);
                return s;
            };
            line = trim(line);
            if (line.size() > 2 && line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
                m[(section.empty() ? "" : section + "/") + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
        return m;
    }

    Settings loadSettings()
    {
        Settings s;
        const auto port = readIni(g_root / L"port.ini"), gs = readIni(g_root / L"gs.ini");
        auto num = [](const std::map<std::string, std::string> &m, const char *k, int def)
        {
            const auto it = m.find(k);
            return it == m.end() ? def : std::atoi(it->second.c_str());
        };
        auto str = [](const std::map<std::string, std::string> &m, const char *k, const std::string &def)
        {
            const auto it = m.find(k);
            return it == m.end() || it->second.empty() ? def : it->second;
        };
        s.fullscreen = num(port, "fullscreen", 0) == 1;
        s.width = num(port, "window_width", s.width);
        s.height = num(port, "window_height", s.height);
        s.volume = std::min(100, std::max(0, num(port, "volume", 100)));
        s.skipIntro = num(port, "skip_intro", 1) == 1;
        s.movies = num(port, "movies", 1) == 1;
        s.renderer = str(gs, "gs/renderer", s.renderer);
        s.upscale = std::max(1, num(gs, "gs/upscale", s.upscale));
        s.filtering = str(gs, "gs/texture_filtering", s.filtering);
        s.aspect = str(gs, "EmuCore/GS/AspectRatio", s.aspect);
        return s;
    }

    void saveSettings(const Settings &s)
    {
        std::ofstream port(g_root / L"port.ini");
        port << "# Rule of Rose PC port: options (written by RuleOfRose.exe; 0 = off, 1 = on)\n"
             << "skip_intro = " << (s.skipIntro ? 1 : 0) << "\n"
             << "movies = " << (s.movies ? 1 : 0) << "\n"
             << "fullscreen = " << (s.fullscreen ? 1 : 0) << "\n"
             << "window_width = " << s.width << "\n"
             << "window_height = " << s.height << "\n"
             << "volume = " << s.volume << "\n";
        std::ofstream gs(g_root / L"gs.ini");
        gs << "# Graphics (written by RuleOfRose.exe). [gs]: renderer = auto | d3d11 | d3d12 | vulkan | opengl;\n"
           << "# upscale = internal resolution multiplier; texture_filtering = ps2 | forced | nearest.\n"
           << "# Other sections go to PCSX2's GS as they are (e.g. [EmuCore/GS] AspectRatio).\n"
           << "[gs]\n"
           << "renderer = " << s.renderer << "\n"
           << "upscale = " << s.upscale << "\n"
           << "texture_filtering = " << s.filtering << "\n"
           << "\n[EmuCore/GS]\n"
           << "AspectRatio = " << s.aspect << "\n";
    }

    // ---------------------------------------------------------------- running the game ----------------------------

    void setDefault(const wchar_t *name, const std::wstring &value)
    {
        wchar_t cur[8];
        if (GetEnvironmentVariableW(name, cur, 8) == 0)
            SetEnvironmentVariableW(name, value.c_str());
    }

    // Starts the game; returns its process handle (or null) and the session's log folder.
    HANDLE startGame(const Settings &s, fs::path &logs, std::wstring &err)
    {
        wchar_t stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::wcsftime(stamp, std::size(stamp), L"%Y%m%d-%H%M%S", &tm);
        logs = g_user / L"logs" / stamp;
        std::error_code ec;
        fs::create_directories(logs, ec);
        fs::create_directories(g_user / L"memcard", ec);
        // Windows paths: PCSX2's file layer does not open gs.ini by a '/'-separated path.
        setDefault(L"ROR_PAD_CONFIG", (g_root / L"pad.ini").wstring());
        setDefault(L"ROR_GS_INI", (g_root / L"gs.ini").wstring());
        setDefault(L"ROR_PORT_INI", (g_root / L"port.ini").wstring());
        setDefault(L"ROR_MC_DIR", (g_user / L"memcard").wstring());
        SetEnvironmentVariableW(L"ROR_LOG", (logs / L"ror.log").wstring().c_str());
        if (!s.movies)
            setDefault(L"ROR_MOVIES", L"0");
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE out = CreateFileW((logs / L"stdout.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE errh = CreateFileW((logs / L"stderr.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = out;
        si.hStdError = errh;
        std::wstring cmd = L"\"" + g_runner.wstring() + L"\" \"" + (g_game / kElfName).wstring() + L"\"";
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(g_runner.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       g_user.c_str(), &si, &pi);
        CloseHandle(out);
        CloseHandle(errh);
        if (!ok)
        {
            err = L"Could not start bin\\ps2EntryRunner.exe (error " + std::to_wstring(GetLastError()) + L").";
            return nullptr;
        }
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    // ---------------------------------------------------------------- the window ----------------------------------

    enum : int
    {
        IdMode = 100, IdSize, IdAspect, IdRenderer, IdUpscale, IdFilter, IdSkipIntro, IdMovies, IdVolume, IdVolumeText,
        IdControls, IdSaves, IdLogs, IdDisc, IdPlay, IdQuit, IdStatus, IdProgress
    };
    constexpr UINT WmProgress = WM_APP + 1, WmExtracted = WM_APP + 2, WmGameEnded = WM_APP + 3;

    struct Choice
    {
        const wchar_t *label;
        const char *value;
    };
    const Choice kModes[] = {{L"Window", "0"}, {L"Fullscreen", "1"}};
    const Choice kSizes[] = {{L"640 x 448 (1x)", "640x448"}, {L"960 x 672 (1.5x)", "960x672"},
                             {L"1280 x 896 (2x)", "1280x896"}, {L"1600 x 1120 (2.5x)", "1600x1120"},
                             {L"1920 x 1344 (3x)", "1920x1344"}};
    const Choice kAspects[] = {{L"4:3 (as on the PS2)", "4:3"}, {L"Stretch to the window", "Stretch"}};
    const Choice kRenderers[] = {{L"Automatic", "auto"}, {L"Direct3D 11", "d3d11"}, {L"Direct3D 12", "d3d12"},
                                 {L"Vulkan", "vulkan"}, {L"OpenGL", "opengl"}};
    const Choice kUpscales[] = {{L"Native (1x)", "1"}, {L"2x", "2"}, {L"3x", "3"}, {L"4x", "4"}};
    const Choice kFilters[] = {{L"As on the PS2", "ps2"}, {L"Smooth (bilinear everywhere)", "forced"},
                               {L"Sharp (no filtering)", "nearest"}};

    HWND g_wnd = nullptr;
    HFONT g_font = nullptr, g_bold = nullptr;
    int g_dpi = 96;
    Settings g_settings;
    std::wstring g_isoArg;
    bool g_busy = false;
    fs::path g_sessionLogs;

    int S(int v) { return MulDiv(v, g_dpi, 96); }
    HWND item(int id) { return GetDlgItem(g_wnd, id); }

    HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr)
    {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
        return c;
    }

    template <size_t N>
    void combo(int x, int y, const wchar_t *label, int id, const Choice (&choices)[N], const std::string &current)
    {
        add(L"STATIC", label, SS_LEFT, x, y + 4, 130, 20, 0);
        HWND c = add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x + 135, y, 230, 200, id);
        int sel = 0;
        for (size_t i = 0; i < N; ++i)
        {
            SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choices[i].label));
            if (current == choices[i].value)
                sel = static_cast<int>(i);
        }
        SendMessageW(c, CB_SETCURSEL, sel, 0);
    }

    template <size_t N>
    std::string picked(int id, const Choice (&choices)[N])
    {
        const LRESULT i = SendMessageW(item(id), CB_GETCURSEL, 0, 0);
        return choices[(i >= 0 && static_cast<size_t>(i) < N) ? i : 0].value;
    }

    bool checked(int id) { return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

    void readControls()
    {
        Settings &s = g_settings;
        s.fullscreen = picked(IdMode, kModes) == "1";
        const std::string size = picked(IdSize, kSizes);
        std::sscanf(size.c_str(), "%dx%d", &s.width, &s.height);
        s.aspect = picked(IdAspect, kAspects);
        s.renderer = picked(IdRenderer, kRenderers);
        s.upscale = std::atoi(picked(IdUpscale, kUpscales).c_str());
        s.filtering = picked(IdFilter, kFilters);
        s.skipIntro = checked(IdSkipIntro);
        s.movies = checked(IdMovies);
        s.volume = static_cast<int>(SendMessageW(item(IdVolume), TBM_GETPOS, 0, 0));
    }

    void showVolume()
    {
        const int v = static_cast<int>(SendMessageW(item(IdVolume), TBM_GETPOS, 0, 0));
        SetWindowTextW(item(IdVolumeText), (std::to_wstring(v) + L" %").c_str());
    }

    void status(const std::wstring &text) { SetWindowTextW(item(IdStatus), text.c_str()); }

    void showGameState()
    {
        status(gameReady() ? L"Game files: ready (PAL, SLES-54218)."
                           : L"Game files: not set up yet. Play asks for your disc image (.iso of the PAL "
                             L"release, SLES-54218) and copies its files once (about 2.6 GB).");
    }

    void setBusy(bool busy)
    {
        g_busy = busy;
        for (int id = IdMode; id <= IdQuit; ++id)
            if (HWND c = item(id))
                EnableWindow(c, !busy);
    }

    void openFolder(const fs::path &p)
    {
        std::error_code ec;
        fs::create_directories(p, ec);
        ShellExecuteW(g_wnd, L"explore", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    std::wstring pickIso()
    {
        wchar_t file[MAX_PATH * 4] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_wnd;
        ofn.lpstrFilter = L"PS2 disc image (*.iso)\0*.iso\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(std::size(file));
        ofn.lpstrTitle = L"Rule of Rose: choose your disc image (PAL, SLES-54218)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
    }

    // Extraction on a worker thread; WmExtracted carries a heap-allocated error text (null = fine) and whether to play.
    void beginInstall(const std::wstring &iso, bool thenPlay)
    {
        setBusy(true);
        ShowWindow(item(IdProgress), SW_SHOW);
        SendMessageW(item(IdProgress), PBM_SETPOS, 0, 0);
        status(L"Copying the game's files from the disc image...");
        std::thread([iso, thenPlay]
                    {
                        int last = -1;
                        std::wstring err = installFrom(iso, [&](int permille)
                                                       {
                                                           if (permille != last)
                                                           {
                                                               last = permille;
                                                               PostMessageW(g_wnd, WmProgress, permille, 0);
                                                           }
                                                       });
                        PostMessageW(g_wnd, WmExtracted, thenPlay,
                                     reinterpret_cast<LPARAM>(err.empty() ? nullptr : new std::wstring(err)));
                    })
            .detach();
    }

    void play()
    {
        readControls();
        saveSettings(g_settings);
        if (!gameReady())
        {
            std::wstring iso = g_isoArg.empty() ? pickIso() : g_isoArg;
            g_isoArg.clear();
            if (!iso.empty())
                beginInstall(iso, true);
            return;
        }
        std::wstring err;
        HANDLE proc = startGame(g_settings, g_sessionLogs, err);
        if (!proc)
        {
            MessageBoxW(g_wnd, err.c_str(), kTitle, MB_OK | MB_ICONERROR);
            return;
        }
        setBusy(true);
        status(L"The game is running.");
        ShowWindow(g_wnd, SW_HIDE);
        std::thread([proc]
                    {
                        WaitForSingleObject(proc, INFINITE);
                        DWORD code = 0;
                        GetExitCodeProcess(proc, &code);
                        CloseHandle(proc);
                        PostMessageW(g_wnd, WmGameEnded, code, 0);
                    })
            .detach();
    }

    void build()
    {
        const Settings &s = g_settings;
        add(L"STATIC", L"Rule of Rose", SS_LEFT, 16, 12, 400, 26, 0, g_bold);
        add(L"STATIC", L"Unofficial PC port - playtest build", SS_LEFT, 16, 38, 400, 20, 0);

        add(L"BUTTON", L"Display", BS_GROUPBOX, 12, 64, 396, 112, 0);
        combo(24, 84, L"Mode:", IdMode, kModes, s.fullscreen ? "1" : "0");
        combo(24, 114, L"Window size:", IdSize, kSizes, std::to_string(s.width) + "x" + std::to_string(s.height));
        combo(24, 144, L"Picture:", IdAspect, kAspects, s.aspect);

        add(L"BUTTON", L"Graphics", BS_GROUPBOX, 12, 182, 396, 112, 0);
        combo(24, 202, L"Renderer:", IdRenderer, kRenderers, s.renderer);
        combo(24, 232, L"Resolution:", IdUpscale, kUpscales, std::to_string(s.upscale));
        combo(24, 262, L"Texture filtering:", IdFilter, kFilters, s.filtering);

        add(L"BUTTON", L"Game", BS_GROUPBOX, 12, 300, 396, 104, 0);
        add(L"BUTTON", L"Skip the intro (logos and intro movie at launch)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 320,
            370, 20, IdSkipIntro);
        add(L"BUTTON", L"Play movies", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 344, 370, 20, IdMovies);
        SendMessageW(item(IdSkipIntro), BM_SETCHECK, s.skipIntro ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(item(IdMovies), BM_SETCHECK, s.movies ? BST_CHECKED : BST_UNCHECKED, 0);
        add(L"STATIC", L"Volume:", SS_LEFT, 24, 374, 60, 20, 0);
        HWND vol = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 84, 370, 260, 26, IdVolume);
        SendMessageW(vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(vol, TBM_SETPAGESIZE, 0, 10);
        SendMessageW(vol, TBM_SETPOS, TRUE, s.volume);
        add(L"STATIC", L"", SS_LEFT, 350, 374, 50, 20, IdVolumeText);
        showVolume();

        add(L"STATIC", L"In the game: F11 fullscreen, hold F to fast-forward, Start skips movies.", SS_LEFT, 16, 412,
            400, 20, 0);
        add(L"BUTTON", L"Controls...", BS_PUSHBUTTON | WS_TABSTOP, 12, 438, 94, 28, IdControls);
        add(L"BUTTON", L"Saves", BS_PUSHBUTTON | WS_TABSTOP, 112, 438, 94, 28, IdSaves);
        add(L"BUTTON", L"Logs", BS_PUSHBUTTON | WS_TABSTOP, 212, 438, 94, 28, IdLogs);
        add(L"BUTTON", L"Disc image...", BS_PUSHBUTTON | WS_TABSTOP, 312, 438, 96, 28, IdDisc);

        add(L"STATIC", L"", SS_LEFT, 16, 476, 392, 34, IdStatus);
        HWND bar = add(PROGRESS_CLASSW, L"", PBS_SMOOTH, 12, 512, 396, 16, IdProgress);
        SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
        ShowWindow(bar, SW_HIDE);
        add(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 208, 536, 112, 34, IdPlay, g_bold);
        add(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, 328, 536, 80, 34, IdQuit);
        showGameState();
    }

    LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case IdPlay:
                if (!g_busy)
                    play();
                return 0;
            case IdQuit:
                readControls();
                saveSettings(g_settings);
                DestroyWindow(hwnd);
                return 0;
            case IdControls:
                ShellExecuteW(hwnd, L"open", L"notepad.exe", (L"\"" + (g_root / L"pad.ini").wstring() + L"\"").c_str(),
                              nullptr, SW_SHOWNORMAL);
                return 0;
            case IdSaves:
                openFolder(g_user / L"memcard");
                return 0;
            case IdLogs:
                openFolder(g_user / L"logs");
                return 0;
            case IdDisc:
            {
                if (gameReady() && MessageBoxW(hwnd, L"The game's files are already set up. Copy them again from "
                                                     L"another disc image?", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                const std::wstring iso = pickIso();
                if (!iso.empty())
                    beginInstall(iso, false);
                return 0;
            }
            }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lp) == item(IdVolume))
                showVolume();
            return 0;
        case WmProgress:
            SendMessageW(item(IdProgress), PBM_SETPOS, wp, 0);
            status(L"Copying the game's files from the disc image... " + std::to_wstring(wp / 10) + L" %");
            return 0;
        case WmExtracted:
        {
            setBusy(false);
            ShowWindow(item(IdProgress), SW_HIDE);
            std::wstring *err = reinterpret_cast<std::wstring *>(lp);
            showGameState();
            if (err)
            {
                MessageBoxW(hwnd, err->c_str(), kTitle, MB_OK | MB_ICONERROR);
                delete err;
            }
            else if (wp)
                play();
            return 0;
        }
        case WmGameEnded:
            setBusy(false);
            showGameState();
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            if (wp != 0)
            {
                const std::wstring text = L"The game ended unexpectedly (code " + std::to_wstring(wp) +
                                          L"). Please send the folder\n\n" + g_sessionLogs.wstring() +
                                          L"\n\nwith a note of what you were doing. Open it now?";
                if (MessageBoxW(hwnd, text.c_str(), kTitle, MB_YESNO | MB_ICONWARNING) == IDYES)
                    openFolder(g_sessionLogs);
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CLOSE:
            if (g_busy)
                return 0;
            readControls();
            saveSettings(g_settings);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    SetProcessDPIAware();
    wchar_t self[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    g_root = fs::path(self).parent_path();
    g_game = g_root / L"game";
    g_user = g_root / L"user";
    g_runner = g_root / L"bin" / L"ps2EntryRunner.exe";
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool reset = false, playNow = false;
    for (int i = 1; argv && i < argc; ++i)
    {
        const std::wstring a = argv[i];
        if (a == L"--iso" && i + 1 < argc)
            g_isoArg = argv[++i];
        else if (a == L"--reset")
            reset = true;
        else if (a == L"--play")
            playNow = true;
        else if (a.size() > 4 && _wcsicmp(a.c_str() + a.size() - 4, L".iso") == 0)
            g_isoArg = a;   // an image dropped on the launcher
    }
    if (!fs::exists(g_runner))
    {
        MessageBoxW(nullptr, L"bin\\ps2EntryRunner.exe is missing: unpack the whole package, keeping its folders.",
                    kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    g_settings = loadSettings();
    if (reset)
    {
        std::error_code ec;
        fs::remove(g_game / kElfName, ec);   // gameReady() fails, so the next Play extracts again
    }

    // Tests: --play extracts if needed (with --iso) and runs the game without the window, returning its exit code.
    if (playNow)
    {
        if (!gameReady())
        {
            const std::wstring err = g_isoArg.empty() ? L"no game files and no --iso" : installFrom(g_isoArg, [](int) {});
            if (!err.empty())
            {
                MessageBoxW(nullptr, err.c_str(), kTitle, MB_OK | MB_ICONERROR);
                return 1;
            }
        }
        saveSettings(g_settings);
        fs::path logs;
        std::wstring err;
        HANDLE proc = startGame(g_settings, logs, err);
        if (!proc)
            return 1;
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        return static_cast<int>(code);
    }

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_bold = CreateFontW(-MulDiv(12, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"RorLauncher";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, S(420), S(582)};
    AdjustWindowRect(&r, style, FALSE);
    const int w = r.right - r.left, h = r.bottom - r.top;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                            (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    build();
    ShowWindow(g_wnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(g_wnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
