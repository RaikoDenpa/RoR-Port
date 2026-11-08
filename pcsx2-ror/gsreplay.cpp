// ror-gsreplay: replays a GS dump through pcsx2-ror.dll's C API, the code path the runner's live GS uses (renderer plan
// step 2, notes/renderer.md 6.5a: "one code path for replay and live"). A client of ror_pcsx2.h only, like the runner;
// it replaces the patched pcsx2-gsrunner's -direct and -vif1 for the regression replays (tools/port/gs_regress.py).
//
//   ror-gsreplay DUMP.gs [--vif1] [--loops N] [--shot V:OUT.png]... [--window] [--gs-ini GS.INI]
//
// DUMP.gs: an uncompressed PCSX2 GS dump (ours from ROR_GSDUMP, src/ror_gsdump.cpp, or PCSX2's own .gs). Per loop: the
// dump's state and registers (ror_pcsx2_gs_load_state), then its records: Transfer -> ror_pcsx2_gs_transfer, Registers
// kept for the next VSync, VSync -> ror_pcsx2_gs_vsync, ReadFIFO2 -> ror_pcsx2_gs_read_fifo. Our VIF1 records (0x40
// VU1 state, 0x41 VIF1 stream; ROR_GSDUMP_VIF1) are skipped: the dump's PATH1/PATH2 transfers stand for them.
// --vif1 (a VIF1 dump, gsdump_F_vif1.gs): the live port's path instead, PCSX2's VIF1 + microVU1 (ROR_PCSX2_OPEN_VIF1):
// 0x40 -> ror_pcsx2_vu1_set_state, 0x41 -> ror_pcsx2_vif1_transfer, PATH3 transfers as above, the dump's PATH1/PATH2
// skipped (VIF1 and VU1 make them).
// --shot V:OUT.png saves the frame VSync number V presents (1 = the dump's first VSync; counted on across loops;
// "last" = the replay's last VSync). PCSX2's hardware renderers show nothing until the first buffer the dump draws is
// displayed (a buffer that only exists in the loaded GS memory has no texture-cache target): our dumps' last VSync
// always shows a frame drawn inside the dump.
// No window by default (surfaceless); --window shows the replay at 50 Hz in a 640 x 448 window.
// --gs-ini: PCSX2's GS settings as the runner's config/gs.ini (ror_pcsx2_gs_config); none: PCSX2's defaults.
// Exit code 0 when the replay ran and every requested screenshot was written.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ror_pcsx2.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    enum : uint8_t { kTransfer = 0, kVSync = 1, kReadFifo2 = 2, kRegisters = 3 }; // GSDumpTypes::GSType
    enum : uint8_t { kVu1State = 0x40, kVif1Data = 0x41 };                         // ours (ROR_GSDUMP_VIF1)
    enum : uint8_t { kPath2 = 1, kPath3 = 2, kPath1New = 3 };                       // GSDumpTypes::GSTransferPath
    constexpr size_t kRegsSize = 8192;

    std::mutex g_logMutex;

    void logSink(void *, int level, const char *message)
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::printf("%s%s\n", level == ROR_PCSX2_LOG_ERROR ? "ERROR: " : level == ROR_PCSX2_LOG_WARNING ? "warning: " : "",
                    message);
        std::fflush(stdout);
    }

    std::string utf8(const std::wstring &w)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    struct Record
    {
        uint8_t type;
        uint8_t path; // Transfer: GSTransferPath
        uint32_t size; // Transfer, Registers, VIF1 records: bytes; ReadFIFO2: quadwords
        size_t data;   // offset in the file
    };

    struct Dump
    {
        std::vector<uint8_t> file;
        size_t state = 0, stateSize = 0, regs = 0;
        std::string serial;
        std::vector<Record> records;
        uint64_t vsyncs = 0;
    };

    bool readRecords(const fs::path &path, Dump &d, size_t p)
    {
        const size_t end = d.file.size();
        const uint8_t *f = d.file.data();
        while (p < end)
        {
            const size_t at = p;
            Record r{f[p++], 0, 0, 0};
            auto have = [&](size_t n) {
                if (p + n <= end)
                    return true;
                std::printf("ERROR: %s: record %02x at offset %zu runs past the end\n", path.string().c_str(), r.type, at);
                return false;
            };
            switch (r.type)
            {
            case kTransfer:
                if (!have(5))
                    return false;
                r.path = f[p];
                std::memcpy(&r.size, f + p + 1, 4);
                p += 5;
                break;
            case kVSync:
                r.size = 1;
                d.vsyncs++;
                break;
            case kReadFifo2:
                if (!have(4))
                    return false;
                std::memcpy(&r.size, f + p, 4);
                p += 4;
                r.data = p;
                d.records.push_back(r);
                continue;
            case kRegisters:
                r.size = kRegsSize;
                break;
            case kVu1State:
            case kVif1Data:
                if (!have(4))
                    return false;
                std::memcpy(&r.size, f + p, 4);
                p += 4;
                break;
            default:
                std::printf("ERROR: %s: unknown record %02x at offset %zu\n", path.string().c_str(), r.type, at);
                return false;
            }
            if (!have(r.size))
                return false;
            r.data = p;
            p += r.size;
            d.records.push_back(r);
        }
        return true;
    }

    // GSDumpFile::ReadFile's layout (pcsx2/GS/GSLzma.cpp): 0xFFFFFFFF, header size, GSDumpHeader (+ serial), state,
    // 8 KB of privileged registers, then the records.
    bool readDump(const fs::path &path, Dump &d)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            std::printf("ERROR: cannot open %s\n", path.string().c_str());
            return false;
        }
        d.file.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        uint32_t lead[2], header[9];
        if (d.file.size() < sizeof lead + sizeof header)
        {
            std::printf("ERROR: %s: too short for a GS dump\n", path.string().c_str());
            return false;
        }
        std::memcpy(lead, d.file.data(), sizeof lead);
        if (lead[0] != 0xffffffffu || lead[1] < sizeof header)
        {
            std::printf("ERROR: %s: not an uncompressed GS dump with a v2 header (a .gs.xz/.gs.zst: decompress first)\n",
                        path.string().c_str());
            return false;
        }
        std::memcpy(header, d.file.data() + sizeof lead, sizeof header);
        if (header[2] + header[3] <= lead[1]) // serial_offset, serial_size (relative to GSDumpHeader)
            d.serial.assign(reinterpret_cast<const char *>(d.file.data() + sizeof lead + header[2]), header[3]);
        d.state = sizeof lead + lead[1];
        d.stateSize = header[1];
        d.regs = d.state + d.stateSize;
        if (d.regs + kRegsSize > d.file.size())
        {
            std::printf("ERROR: %s: truncated (state %zu bytes)\n", path.string().c_str(), d.stateSize);
            return false;
        }
        return readRecords(path, d, d.regs + kRegsSize);
    }

    bool g_quit = false;

    LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_CLOSE)
        {
            g_quit = true;
            return 0;
        }
        if (msg == WM_ERASEBKGND)
            return 1;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND createWindow(int w, int h)
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.lpszClassName = L"RorGsReplay";
        RegisterClassExW(&wc);
        RECT r{0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        return CreateWindowExW(0, wc.lpszClassName, L"ror-gsreplay", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                               CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    }

    void pumpMessages()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    int usage()
    {
        std::printf("usage: ror-gsreplay DUMP.gs [--vif1] [--loops N] [--shot V:OUT.png]... [--window] "
                    "[--gs-ini GS.INI]\n"
                    "  V: VSync number from the start of the replay (1 = the dump's first), or \"last\"\n");
        return 2;
    }
}

int wmain(int argc, wchar_t **argv)
{
    fs::path dumpPath, gsIni;
    int loops = 1;
    bool windowed = false, vif1 = false;
    std::vector<std::pair<std::wstring, fs::path>> shotArgs;
    for (int i = 1; i < argc; i++)
    {
        const std::wstring a = argv[i];
        if (a == L"--loops" && i + 1 < argc)
            loops = _wtoi(argv[++i]);
        else if (a == L"--shot" && i + 1 < argc)
        {
            const std::wstring s = argv[++i];
            const size_t colon = s.find(L':');
            if (colon == std::wstring::npos || colon == 0)
                return usage();
            shotArgs.emplace_back(s.substr(0, colon), fs::absolute(s.substr(colon + 1)));
        }
        else if (a == L"--window")
            windowed = true;
        else if (a == L"--vif1")
            vif1 = true;
        else if (a == L"--gs-ini" && i + 1 < argc)
            gsIni = fs::absolute(argv[++i]);
        else if (dumpPath.empty() && a[0] != L'-')
            dumpPath = a;
        else
            return usage();
    }
    if (dumpPath.empty() || loops < 1)
        return usage();

    Dump dump;
    if (!readDump(dumpPath, dump))
        return 1;
    if (vif1 && std::none_of(dump.records.begin(), dump.records.end(), [](const Record &r) { return r.type == kVu1State; }))
    {
        std::printf("ERROR: --vif1: %s has no VU1 state record (written with ROR_GSDUMP_VIF1=1?)\n",
                    dumpPath.string().c_str());
        return 2;
    }
    const uint64_t total = dump.vsyncs * static_cast<uint64_t>(loops);
    std::map<uint64_t, std::pair<fs::path, std::string>> shots; // VSync -> path, its UTF-8 for the DLL
    for (auto &[v, path] : shotArgs)
    {
        const uint64_t n = v == L"last" ? total : std::wcstoull(v.c_str(), nullptr, 10);
        if (n == 0 || n > total)
        {
            std::printf("ERROR: --shot %ls: the replay has %llu vsyncs (%llu per loop)\n", v.c_str(),
                        static_cast<unsigned long long>(total), static_cast<unsigned long long>(dump.vsyncs));
            return 2;
        }
        std::error_code ec;
        fs::remove(path, ec);
        shots[n] = {path, utf8(path.wstring())};
    }

    const int version = ror_pcsx2_api_version();
    if (version != ROR_PCSX2_API_VERSION || ror_pcsx2_open(ROR_PCSX2_API_VERSION, logSink, nullptr) != ROR_PCSX2_OK)
    {
        std::printf("ERROR: pcsx2-ror.dll implements API v%d, ror-gsreplay was built for v%d\n", version,
                    ROR_PCSX2_API_VERSION);
        return 1;
    }
    if (!gsIni.empty() && ror_pcsx2_gs_config(utf8(gsIni.wstring()).c_str()) != ROR_PCSX2_OK)
    {
        std::printf("ERROR: --gs-ini %s: not applied (see the messages above)\n", gsIni.string().c_str());
        ror_pcsx2_close();
        return 2;
    }
    constexpr int kWidth = 640, kHeight = 448;
    HWND window = windowed ? createWindow(kWidth, kHeight) : nullptr;
    if (windowed && !window)
    {
        std::printf("ERROR: cannot create the window (error %lu)\n", GetLastError());
        ror_pcsx2_close();
        return 1;
    }
    int rc = ror_pcsx2_gs_open(window, kWidth, kHeight, vif1 ? ROR_PCSX2_OPEN_VIF1 : 0u);
    if (rc != ROR_PCSX2_OK)
    {
        std::printf("ERROR: ror_pcsx2_gs_open: %d\n", rc);
        ror_pcsx2_close();
        return 1;
    }

    const uint8_t *file = dump.file.data();
    std::vector<uint8_t> regs, fifo;
    uint64_t vsync = 0;
    bool failed = false;
    auto next = std::chrono::steady_clock::now();
    const auto t0 = next;
    for (int loop = 0; loop < loops && !failed && !g_quit; loop++)
    {
        regs.assign(file + dump.regs, file + dump.regs + kRegsSize);
        rc = ror_pcsx2_gs_load_state(file + dump.state, dump.stateSize, regs.data(), regs.size());
        if (rc != ROR_PCSX2_OK)
        {
            std::printf("ERROR: ror_pcsx2_gs_load_state: %d\n", rc);
            failed = true;
            break;
        }
        unsigned transfers[4] = {}, reads = 0, vifRecords = 0, skipped = 0;
        for (const Record &r : dump.records)
        {
            if (failed || g_quit)
                break;
            switch (r.type)
            {
            case kTransfer:
            {
                const int gifPath = r.path == kPath1New ? 1 : r.path == kPath2 ? 2 : r.path == kPath3 ? 3 : 0;
                if (!gifPath || (vif1 && gifPath != 3))
                {
                    skipped += r.size != 0; // VIF1 + VU1 make PATH1/PATH2; Path1Old and Dummy: not in our dumps
                    break;
                }
                if (r.size == 0)
                    break;
                rc = ror_pcsx2_gs_transfer(gifPath, file + r.data, r.size);
                if (rc != ROR_PCSX2_OK)
                {
                    std::printf("ERROR: ror_pcsx2_gs_transfer(PATH%d, %u bytes): %d\n", gifPath, r.size, rc);
                    failed = true;
                }
                transfers[gifPath]++;
                break;
            }
            case kVSync: // its field byte: CSR.FIELD in the registers says the same
                ++vsync;
                if (auto s = shots.find(vsync); s != shots.end())
                    ror_pcsx2_gs_screenshot(s->second.second.c_str());
                rc = ror_pcsx2_gs_vsync(regs.data(), regs.size());
                if (rc != ROR_PCSX2_OK)
                {
                    std::printf("ERROR: ror_pcsx2_gs_vsync: %d\n", rc);
                    failed = true;
                }
                if (window)
                {
                    pumpMessages();
                    next += std::chrono::milliseconds(20);
                    std::this_thread::sleep_until(next);
                }
                break;
            case kReadFifo2:
                fifo.resize(static_cast<size_t>(r.size) * 16);
                if (r.size && ror_pcsx2_gs_read_fifo(fifo.data(), fifo.size()) != ROR_PCSX2_OK)
                {
                    std::printf("ERROR: ror_pcsx2_gs_read_fifo(%u quadwords) failed\n", r.size);
                    failed = true;
                }
                reads++;
                break;
            case kRegisters:
                std::memcpy(regs.data(), file + r.data, kRegsSize);
                break;
            case kVu1State:
                if (vif1 && (rc = ror_pcsx2_vu1_set_state(file + r.data, r.size)) != ROR_PCSX2_OK)
                {
                    std::printf("ERROR: ror_pcsx2_vu1_set_state(%u bytes): %d\n", r.size, rc);
                    failed = true;
                }
                vifRecords++;
                break;
            default: // kVif1Data
                if (vif1 && (rc = ror_pcsx2_vif1_transfer(file + r.data, r.size)) != ROR_PCSX2_OK)
                {
                    std::printf("ERROR: ror_pcsx2_vif1_transfer(%u bytes): %d\n", r.size, rc);
                    failed = true;
                }
                vifRecords++;
                break;
            }
        }
        std::printf("loop %d: %llu vsyncs, %u PATH1 + %u PATH2 + %u PATH3 transfers, %u reads, %u VIF1 records %s, %u "
                    "transfer records skipped\n", loop, static_cast<unsigned long long>(dump.vsyncs), transfers[1],
                    transfers[2], transfers[3], reads, vifRecords, vif1 ? "fed" : "skipped", skipped);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    ror_pcsx2_gs_counters c{};
    ror_pcsx2_gs_get_counters(&c, sizeof c);
    ror_pcsx2_close(); // closes the GS: every queued screenshot is written by then
    if (window)
        DestroyWindow(window);
    std::printf("%s (serial %s): %llu vsyncs, %llu presented, %.0f ms", dumpPath.string().c_str(), dump.serial.c_str(),
                c.vsyncs, c.presents, ms);
    if (vif1)
        std::printf("; %llu VIF1 streams, %llu stalls", c.vif1_streams, c.vif1_stalls);
    std::printf("\n");

    for (auto &[v, s] : shots)
    {
        std::error_code ec;
        const unsigned long long n = v;
        if (v > vsync)
        {
            std::printf("ERROR: --shot %llu: the replay stopped after %llu vsyncs\n", n,
                        static_cast<unsigned long long>(vsync));
            failed = true;
        }
        else if (!fs::exists(s.first, ec))
        {
            std::printf("ERROR: vsync %llu: no screenshot written (%s); a frame shown before the dump's first drawn buffer "
                        "has none\n", n, s.first.string().c_str());
            failed = true;
        }
        else
            std::printf("vsync %llu -> %s\n", n, s.first.string().c_str());
    }
    return failed ? 1 : 0;
}
