// The GS (renderer plan step 2, notes/renderer.md 6.4/6.5a): PCSX2's settings without a VM, MTGS on the runner's child
// window (or none, for the replay tool), VSync with the EE's privileged registers, and a GS dump's other streams (state,
// GIF transfers, GS->host reads) fed as GSDumpReplayer feeds them. The bring-up is pcsx2-gsrunner's InitializeConfig +
// SettingsOverride and the -direct mode's (tools/pcsx2-patches/0001): no VMManager::Initialize, no BIOS, no EE/IOP/SPU2.
#include "pcsx2/PrecompiledHeader.h"

#include "ror_internal.h"
#include "ror_pcsx2.h"

#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/WindowInfo.h"

#include "pcsx2/Config.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#include "pcsx2/Gif_Unit.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/MTVU.h"
#include "pcsx2/SaveState.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/VMManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using ror_pcsx2::log;

namespace
{
    MemorySettingsInterface s_settings;
    bool s_config_done = false;
    bool s_gs_open = false;
    // config/gs.ini (ror_pcsx2_gs_config): {section, key, value}, applied over the defaults and the port's overrides.
    std::vector<std::array<std::string, 3>> s_user_settings;

    std::mutex s_window_mutex; // s_window: written on the caller's thread, read on the GS thread (AcquireRenderWindow)
    WindowInfo s_window;       // Surfaceless both before ror_pcsx2_gs_open and for a null window
    bool s_window_set = false;

    std::atomic<unsigned long long> s_vsyncs{0};
    std::atomic<unsigned long long> s_presents{0};

    // pcsx2-gsrunner's InitializeConfig, then its SettingsOverride minus what only a dump runner wants.
    bool initializeConfig()
    {
        if (s_config_done)
            return true;
        EmuFolders::SetAppRoot();
        if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
        {
            log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: PCSX2's resources/ or data folder not found next to the exe (%s)",
                EmuFolders::AppRoot.c_str());
            return false;
        }
        const char *error = nullptr;
        if (!VMManager::PerformEarlyHardwareChecks(&error))
        {
            log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: %s", error ? error : "hardware check failed");
            return false;
        }
        const std::string roboto = EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
        const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto.c_str());
        if (roboto_data.empty())
        {
            log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: cannot read %s", roboto.c_str());
            return false;
        }
        std::vector<ImGuiManager::FontInfo> fonts;
        ImGuiManager::FontInfo fi{};
        fi.data = roboto_data;
        fonts.push_back(fi);
        ImGuiManager::SetFonts(std::move(fonts));

        // Everything in memory: no ini is read or written.
        Host::Internal::SetBaseSettingsLayer(&s_settings);
        VMManager::SetDefaultSettings(s_settings, true, true, true, true, true);

        // Our 50 Hz VBlank clock is the master: no frame limiter, no host vsync (a 60 Hz display would pace it).
        s_settings.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
        s_settings.SetIntValue("EmuCore/GS", "VsyncEnable", false);
        s_settings.SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
        // Screenshots at the internal resolution (aspect corrected): the same size live and in the surfaceless replay.
        s_settings.SetIntValue("EmuCore/GS", "ScreenshotSize", static_cast<int>(GSScreenshotSize::InternalResolution));
        // Input is the runner's (config/pad.ini), sound our own seam.
        s_settings.SetBoolValue("InputSources", "SDL", false);
        s_settings.SetBoolValue("InputSources", "XInput", false);
        s_settings.SetStringValue("SPU2/Output", "OutputModule", "nullout");
        Pad::ClearPortBindings(s_settings, 0);
        s_settings.ClearSection("Hotkeys");
        for (u32 i = 0; i < 2; i++)
        {
            s_settings.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
            s_settings.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
        }
        // No on-screen messages over the game.
        s_settings.SetBoolValue("EmuCore/GS", "OsdShowMessages", false);
        s_settings.SetBoolValue("Logging", "EnableSystemConsole", false);

        for (const auto &[section, key, value] : s_user_settings)
            s_settings.SetStringValue(section.c_str(), key.c_str(), value.c_str());

        VMManager::Internal::LoadStartupSettings();
        s_config_done = true;
        return true;
    }

    // CSR.FINISH/SIGNAL and LBLID as we last wrote them: what VU1's output (MTVU, VU_Thread::Get_MTVUChanges) sets in
    // PS2MEM_GS between two VSyncs is counted (renderer plan: a log line before any forwarding to the EE).
    u64 s_csr_written = 0, s_siglbl_written = 0;
    std::atomic<unsigned long long> s_finish{0}, s_signal{0}, s_label{0};

    void countGsEvents()
    {
        u64 csr, siglbl;
        std::memcpy(&csr, PS2MEM_GS + 0x1000, 8);
        std::memcpy(&siglbl, PS2MEM_GS + 0x1080, 8);
        const u64 fresh = csr & ~s_csr_written;
        const bool label = (siglbl >> 32) != (s_siglbl_written >> 32);
        if (!(fresh & 3) && !label)
            return;
        if (s_finish + s_signal + s_label == 0)
            log(ROR_PCSX2_LOG_WARNING, "pcsx2-ror: VU1's output set %s%s%s (handled by PCSX2, not forwarded to the EE)",
                fresh & 2 ? "FINISH " : "", fresh & 1 ? "SIGNAL " : "", label ? "LABEL" : "");
        s_finish += (fresh >> 1) & 1;
        s_signal += fresh & 1;
        s_label += label;
    }

    void rememberRegs()
    {
        std::memcpy(&s_csr_written, PS2MEM_GS + 0x1000, 8);
        std::memcpy(&s_siglbl_written, PS2MEM_GS + 0x1080, 8);
    }

    std::string trim(std::string s)
    {
        const auto first = s.find_first_not_of(" \t\r");
        const auto last = s.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
    }

    std::string lower(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // gs.ini's [gs] keys -> PCSX2's (Pcsx2Config::GSOptions: "Renderer" = GSRendererType, "upscale_multiplier",
    // "filter" = BiFiltering). False for an unknown key or value.
    bool mapGsKey(const std::string &key, const std::string &value, std::array<std::string, 3> &out)
    {
        const std::string k = lower(key), v = lower(value);
        if (k == "renderer")
        {
            static const std::pair<const char *, int> kRenderers[] = {{"auto", -1}, {"d3d11", 3}, {"d3d12", 15},
                {"vulkan", 14}, {"opengl", 12}, {"software", 13}};
            for (const auto &[name, id] : kRenderers)
                if (v == name)
                {
                    out = {"EmuCore/GS", "Renderer", std::to_string(id)};
                    return true;
                }
            return false;
        }
        if (k == "upscale")
        {
            char *end = nullptr;
            const double m = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end || m < 1.0 || m > 12.0)
                return false;
            out = {"EmuCore/GS", "upscale_multiplier", v};
            return true;
        }
        if (k == "texture_filtering")
        {
            static const std::pair<const char *, int> kFilters[] = {{"nearest", 0}, {"forced", 1}, {"ps2", 2},
                {"forced_no_sprites", 3}};
            for (const auto &[name, id] : kFilters)
                if (v == name)
                {
                    out = {"EmuCore/GS", "filter", std::to_string(id)};
                    return true;
                }
            return false;
        }
        return false;
    }
} // namespace

extern "C" int ror_pcsx2_gs_config(const char *path)
{
    if (!ror_pcsx2::isOpen() || s_gs_open || s_config_done)
        return ROR_PCSX2_E_STATE;
    if (!path || !*path)
        return ROR_PCSX2_E_ARG;
    const std::optional<std::string> text = FileSystem::ReadFileToString(path);
    if (!text)
    {
        log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: cannot read %s", path);
        return ROR_PCSX2_E_FAILED;
    }
    int bad = 0;
    std::string section, applied;
    size_t pos = 0;
    while (pos <= text->size())
    {
        const size_t eol = std::min(text->find('\n', pos), text->size());
        std::string line = text->substr(pos, eol - pos);
        pos = eol + 1;
        if (const size_t c = line.find_first_of(";#"); c != std::string::npos)
            line.resize(c);
        line = trim(line);
        if (line.empty())
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || section.empty())
        {
            log(ROR_PCSX2_LOG_WARNING, "pcsx2-ror: %s: ignored line \"%s\"", path, line.c_str());
            ++bad;
            continue;
        }
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        std::array<std::string, 3> setting{section, key, value};
        if (lower(section) == "gs" && !mapGsKey(key, value, setting))
        {
            log(ROR_PCSX2_LOG_WARNING, "pcsx2-ror: %s: [gs] %s = %s not understood", path, key.c_str(), value.c_str());
            ++bad;
            continue;
        }
        s_user_settings.push_back(setting);
        applied += fmt::format("{}{}/{} = {}", applied.empty() ? "" : ", ", setting[0], setting[1], setting[2]);
    }
    log(ROR_PCSX2_LOG_INFO, "pcsx2-ror: %s: %s", path, applied.empty() ? "no settings" : applied.c_str());
    return bad ? ROR_PCSX2_E_ARG : ROR_PCSX2_OK;
}

std::optional<WindowInfo> ror_pcsx2::renderWindow()
{
    std::lock_guard<std::mutex> lock(s_window_mutex);
    if (!s_window_set)
        return std::nullopt;
    return s_window;
}

void ror_pcsx2::countPresent()
{
    s_presents.fetch_add(1, std::memory_order_relaxed);
}

extern "C" int ror_pcsx2_gs_open(void *window, int width, int height, unsigned flags)
{
    if (!ror_pcsx2::isOpen() || s_gs_open)
        return ROR_PCSX2_E_STATE;
    if (width <= 0 || height <= 0 || (flags & ~ROR_PCSX2_OPEN_VIF1))
        return ROR_PCSX2_E_ARG;
    const bool vif1 = (flags & ROR_PCSX2_OPEN_VIF1) != 0;
    {
        std::lock_guard<std::mutex> lock(s_window_mutex);
        s_window = WindowInfo{};
        if (window)
        {
            s_window.type = WindowInfo::Type::Win32;
            s_window.window_handle = window;
        }
        s_window.surface_width = static_cast<u32>(width);
        s_window.surface_height = static_cast<u32>(height);
        s_window.surface_scale = 1.0f;
        s_window_set = true;
    }
    auto fail = [] {
        std::lock_guard<std::mutex> lock(s_window_mutex);
        s_window_set = false;
        return ROR_PCSX2_E_FAILED;
    };
    if (!initializeConfig())
        return fail();
    // VIF1: MTVU (VU1 on its own thread, as step 0b measured) and microVU1, as the gsrunner's -vif1.
    s_settings.SetBoolValue("EmuCore/Speedhacks", "vuThread", vif1);
    s_settings.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
    VMManager::ApplySettings();
    if (vif1 && !ror_pcsx2::vif1Open())
        return fail();
    s_vsyncs.store(0);
    s_presents.store(0);
    s_finish.store(0);
    s_signal.store(0);
    s_label.store(0);
    rememberRegs();
    if (!MTGS::WaitForOpen())
    {
        log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: the GS thread did not open (see PCSX2's messages above)");
        MTGS::ShutdownThread();
        ror_pcsx2::vif1Close();
        return fail();
    }
    s_gs_open = true;
    log(ROR_PCSX2_LOG_INFO, "pcsx2-ror: GS open%s, renderer setting %s, %s %dx%d, data folder %s",
        vif1 ? " with VIF1 + microVU1" : "", Pcsx2Config::GSOptions::GetRendererName(EmuConfig.GS.Renderer),
        window ? "window" : "no window (surfaceless)", width, height, EmuFolders::DataRoot.c_str());
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_gs_resize(int width, int height)
{
    if (!s_gs_open)
        return ROR_PCSX2_E_STATE;
    if (width <= 0 || height <= 0)
        return ROR_PCSX2_E_ARG;
    {
        std::lock_guard<std::mutex> lock(s_window_mutex);
        s_window.surface_width = static_cast<u32>(width);
        s_window.surface_height = static_cast<u32>(height);
    }
    MTGS::ResizeDisplayWindow(static_cast<u32>(width), static_cast<u32>(height), 1.0f);
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_gs_vsync(const void *regs, size_t size)
{
    if (!s_gs_open)
        return ROR_PCSX2_E_STATE;
    if (!regs || size > Ps2MemSize::GSregs)
        return ROR_PCSX2_E_ARG;
    countGsEvents();
    std::memcpy(PS2MEM_GS, regs, size);
    std::memset(PS2MEM_GS + size, 0, Ps2MemSize::GSregs - size);
    rememberRegs();
    MTGS::PostVsyncStart(true);
    s_vsyncs.fetch_add(1, std::memory_order_relaxed);
    return ROR_PCSX2_OK;
}

// The dump's state and transfers as GSDumpReplayer (and the gsrunner's -direct) feed them: registers, then
// MTGS::Freeze(Load) (it waits for the GS thread and gives it PS2MEM_GS); each packet staged in its path's GIF buffer
// and handed to MTGS as a completed packet.
extern "C" int ror_pcsx2_gs_load_state(const void *state, size_t size, const void *regs, size_t regs_size)
{
    if (!s_gs_open)
        return ROR_PCSX2_E_STATE;
    if (!state || size == 0 || size > INT_MAX || (regs_size && !regs) || regs_size > Ps2MemSize::GSregs)
        return ROR_PCSX2_E_ARG;
    if (regs_size)
        std::memcpy(PS2MEM_GS, regs, regs_size);
    std::memset(PS2MEM_GS + regs_size, 0, Ps2MemSize::GSregs - regs_size);
    rememberRegs();
    freezeData fd = {static_cast<int>(size), static_cast<u8 *>(const_cast<void *>(state))};
    MTGS::FreezeData mfd = {&fd, 0};
    MTGS::Freeze(FreezeAction::Load, mfd);
    if (mfd.retval != 0)
    {
        log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: GS state load failed (%d; %zu bytes)", mfd.retval, size);
        return ROR_PCSX2_E_FAILED;
    }
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_gs_transfer(int path, const void *data, size_t size)
{
    if (!s_gs_open)
        return ROR_PCSX2_E_STATE;
    if (path < 1 || path > 3 || !data || size == 0 || size % 16 || size > 0x40000000u)
        return ROR_PCSX2_E_ARG;
    if (path != 3 && ror_pcsx2::vif1IsOpen())
        return ROR_PCSX2_E_STATE; // PATH1/PATH2 buffers belong to PCSX2's VU1 (MTVU) and VIF1 then
    const GIF_PATH index = static_cast<GIF_PATH>(path - 1);
    Gif_Path &gifPath = gifUnit.gifPath[index];
    gifPath.CopyGSPacketData(static_cast<u8 *>(const_cast<void *>(data)), static_cast<u32>(size));
    GS_Packet gsPack;
    gsPack.offset = gifPath.curOffset;
    gsPack.size = static_cast<u32>(size);
    gifPath.curOffset += static_cast<u32>(size);
    Gif_AddCompletedGSPacket(gsPack, index);
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_gs_save_state(void *buf, size_t size, size_t *needed)
{
    if (!s_gs_open)
        return ROR_PCSX2_E_STATE;
    freezeData fd = {0, nullptr};
    MTGS::FreezeData mfd = {&fd, 0};
    MTGS::Freeze(FreezeAction::Size, mfd);
    if (needed)
        *needed = static_cast<size_t>(fd.size);
    if (!buf || size < static_cast<size_t>(fd.size))
        return ROR_PCSX2_E_BUFFER;
    // As GSRenderer's own dump writer: render targets live on the GPU, local memory gets them first.
    MTGS::RunOnGSThread([]() {
        if (g_gs_renderer)
            g_gs_renderer->ReadbackTextureCache();
    });
    fd.data = static_cast<u8 *>(buf);
    MTGS::Freeze(FreezeAction::Save, mfd);
    if (mfd.retval != 0)
    {
        log(ROR_PCSX2_LOG_ERROR, "pcsx2-ror: GS state save failed (%d)", mfd.retval);
        return ROR_PCSX2_E_FAILED;
    }
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_gs_read_fifo(void *buf, size_t size)
{
    if (!s_gs_open)
        return ROR_PCSX2_E_STATE;
    if (!buf || size == 0 || size % 16 || size / 16 > UINT_MAX)
        return ROR_PCSX2_E_ARG;
    MTGS::InitAndReadFIFO(static_cast<u8 *>(buf), static_cast<u32>(size / 16));
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_gs_screenshot(const char *path)
{
    if (!s_gs_open)
        return ROR_PCSX2_E_STATE;
    if (!path || !*path)
        return ROR_PCSX2_E_ARG;
    // Through the ring, not GSQueueSnapshot here: that sets the renderer's request from this thread, and whichever
    // VSync the GS thread happens to be on takes it. In the ring it lands right before the next VSync we send.
    MTGS::RunOnGSThread([p = std::string(path)]() { GSQueueSnapshot(p, 0); });
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_gs_get_counters(ror_pcsx2_gs_counters *out, size_t size)
{
    if (!out)
        return ROR_PCSX2_E_ARG;
    if (size < sizeof(ror_pcsx2_gs_counters))
        return ROR_PCSX2_E_BUFFER;
    out->vsyncs = s_vsyncs.load(std::memory_order_relaxed);
    out->presents = s_presents.load(std::memory_order_relaxed);
    ror_pcsx2::vif1Counters(out->vif1_streams, out->vif1_stalls);
    out->gs_finish = s_finish.load(std::memory_order_relaxed);
    out->gs_signal = s_signal.load(std::memory_order_relaxed);
    out->gs_label = s_label.load(std::memory_order_relaxed);
    return ROR_PCSX2_OK;
}

extern "C" void ror_pcsx2_gs_close(void)
{
    if (!s_gs_open)
        return;
    if (ror_pcsx2::vif1IsOpen())
        vu1Thread.WaitVU();
    MTGS::WaitGS();
    countGsEvents();
    GSJoinSnapshotThreads();
    MTGS::WaitForClose();
    MTGS::ShutdownThread();
    unsigned long long streams = 0, stalls = 0;
    ror_pcsx2::vif1Counters(streams, stalls);
    if (ror_pcsx2::vif1IsOpen())
        log(ROR_PCSX2_LOG_INFO, "pcsx2-ror: VIF1 closed after %llu streams, %llu stalls; VU1 output set FINISH %llu, "
            "SIGNAL %llu, LABEL %llu times", streams, stalls, s_finish.load(), s_signal.load(), s_label.load());
    ror_pcsx2::vif1Close();
    s_gs_open = false;
    {
        std::lock_guard<std::mutex> lock(s_window_mutex);
        s_window = WindowInfo{};
        s_window_set = false;
    }
    log(ROR_PCSX2_LOG_INFO, "pcsx2-ror: GS closed after %llu vsyncs, %llu frames presented", s_vsyncs.load(),
        s_presents.load());
}
