// Memory-card seam: libmc on host folders, one per card slot (notes/port.md milestone 6, "Saving").
//
//   ROR_MC_DIR (default memcard/ in the working folder, the run folder under build.py run)
//     slot1/   port 0: always inserted, formatted (created at the first call)
//     slot2/   port 1: inserted only when the folder exists
//
// A guest path /BESLES-54218xxx/file is the file slotN/BESLES-54218xxx/file on the host. The game's card module
// (0x188604..0x18a718, one wrapper per call) starts each call and then polls sceMcSync 0x24fb98 for its result, as
// libmc's asynchronous IOP side works; here every call completes at once and sceMcSync hands out the stored result.
// Result codes and command numbers are libmc's (0 succeed, -1 changed/new card, -2 not formatted, -4 no entry,
// -5 denied, -6 not empty, -7 too many handles; commands GetInfo 1 ... Rename 0x13). "No card" -10 UNVERIFIED (any
// other value is "error" to 0x188604). Directory entries: libmc's sceMcTblGetDir (64 bytes).
#include "ror.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr int32_t kNoCard = -10, kNoEntry = -4, kDenied = -5, kNotEmpty = -6, kTooManyFiles = -7;
    constexpr uint32_t kCardKb = 8000;  // free clusters (1 KB) of an empty 8 MB card, as the placeholder reported
    constexpr uint16_t kAttrDir = 0x8427, kAttrFile = 0x8497;

    struct Pending
    {
        bool pending = false;
        int32_t cmd = 0;
        int32_t result = 0;
    } g_mc;

    struct OpenFile
    {
        std::FILE *f = nullptr;
        std::string guest;
    };

    struct Listing
    {
        std::vector<std::vector<uint8_t>> entries;  // 64-byte sceMcTblGetDir records
        size_t next = 0;
    };

    bool g_ready = false, g_seen[2] = {};
    fs::path g_root;
    std::string g_cwd[2] = {"/", "/"};
    std::map<int32_t, OpenFile> g_files;
    Listing g_listing[2];

    void accept(R5900Context *ctx, int32_t cmd, int32_t result)
    {
        g_mc = {true, cmd, result};
        ror::ret(ctx, 0);
    }

    void init()
    {
        if (g_ready)
            return;
        g_ready = true;
        const char *dir = std::getenv("ROR_MC_DIR");
        g_root = fs::absolute(dir && *dir ? fs::path(dir) : fs::path("memcard"));
        std::error_code ec;
        fs::create_directories(g_root / "slot1", ec);
        ror::log("memcard: slot 1 = %s, slot 2 = %s%s", (g_root / "slot1").string().c_str(),
                 (g_root / "slot2").string().c_str(), fs::is_directory(g_root / "slot2") ? "" : " (not inserted)");
    }

    bool present(uint32_t port)
    {
        init();
        return port == 0 || fs::is_directory(g_root / "slot2");
    }

    fs::path slotRoot(uint32_t port) { return g_root / (port == 0 ? "slot1" : "slot2"); }

    // Guest path (absolute, or relative to the port's current directory) -> normalised "/a/b"; false if it leaves
    // the card.
    bool normalise(uint32_t port, const std::string &name, std::string &out)
    {
        std::vector<std::string> parts;
        const std::string full = (!name.empty() && name[0] == '/') ? name : g_cwd[port] + "/" + name;
        size_t i = 0;
        while (i <= full.size())
        {
            const size_t j = std::min(full.find('/', i), full.size());
            const std::string p = full.substr(i, j - i);
            i = j + 1;
            if (p.empty() || p == ".")
                continue;
            if (p == "..")
            {
                if (parts.empty())
                    return false;
                parts.pop_back();
                continue;
            }
            parts.push_back(p);
        }
        out.clear();
        for (const std::string &p : parts)
            out += "/" + p;
        if (out.empty())
            out = "/";
        return true;
    }

    fs::path hostPath(uint32_t port, const std::string &guest)
    {
        fs::path p = slotRoot(port);
        if (guest != "/")
            p /= fs::path(guest.substr(1));
        return p;
    }

    // libmc's wildcards: '*' any run, '?' one character.
    bool match(const char *pat, const char *s)
    {
        if (!*pat)
            return !*s;
        if (*pat == '*')
            return match(pat + 1, s) || (*s && match(pat, s + 1));
        return *s && (*pat == '?' || *pat == *s) && match(pat + 1, s + 1);
    }

    void putTime(std::vector<uint8_t> &e, size_t at, std::time_t t)
    {
        std::tm tm{};
        localtime_s(&tm, &t);
        e[at + 1] = static_cast<uint8_t>(tm.tm_sec);
        e[at + 2] = static_cast<uint8_t>(tm.tm_min);
        e[at + 3] = static_cast<uint8_t>(tm.tm_hour);
        e[at + 4] = static_cast<uint8_t>(tm.tm_mday);
        e[at + 5] = static_cast<uint8_t>(tm.tm_mon + 1);
        const uint16_t year = static_cast<uint16_t>(tm.tm_year + 1900);
        e[at + 6] = static_cast<uint8_t>(year);
        e[at + 7] = static_cast<uint8_t>(year >> 8);
    }

    std::time_t mtime(const fs::path &p)
    {
        std::error_code ec;
        const auto ft = fs::last_write_time(p, ec);
        if (ec)
            return std::time(nullptr);
        return std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(ft));
    }

    std::vector<uint8_t> entry(const std::string &name, const fs::path &p, bool dir)
    {
        std::vector<uint8_t> e(64, 0);
        const std::time_t t = mtime(p);
        putTime(e, 0, t);
        putTime(e, 8, t);
        std::error_code ec;
        const uint32_t size = dir ? 0 : static_cast<uint32_t>(fs::file_size(p, ec));
        for (int i = 0; i < 4; ++i)
            e[16 + i] = static_cast<uint8_t>(size >> (8 * i));
        const uint16_t attr = dir ? kAttrDir : kAttrFile;
        e[20] = static_cast<uint8_t>(attr);
        e[21] = static_cast<uint8_t>(attr >> 8);
        std::copy_n(name.begin(), std::min<size_t>(name.size(), 31), e.begin() + 32);
        return e;
    }

    // Clusters in use: each file rounded up to 1 KB, one per directory (UNVERIFIED: libmc's FAT keeps two entries
    // per cluster).
    uint32_t usedKb(uint32_t port)
    {
        uint64_t kb = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(slotRoot(port), ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
            kb += it->is_directory() ? 1 : (it->file_size() + 1023) / 1024;
        return static_cast<uint32_t>(std::min<uint64_t>(kb, kCardKb));
    }

    std::string guestStr(uint8_t *rdram, uint32_t addr) { return addr ? ror::str(rdram, addr, 1024) : std::string(); }
}

void ror_sceMcInit(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    init();
    ror::ret(ctx, 0);
}

// sceMcGetInfo(port, slot, type*, free*, format*): the first call on an inserted card says "new card" (-1), as libmc
// does after a card change; 0x188604 then retries and gets 0.
void ror_sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = ror::arg(ctx, 0) & 1;
    const uint32_t type = ror::arg(ctx, 2), freeOut = ror::arg(ctx, 3), format = ror::arg(ctx, 4);
    const bool in = present(port);
    if (type)
        ror::wr32(rdram, type, in ? 2 : 0);  // 2 = PS2 card
    if (freeOut)
        ror::wr32(rdram, freeOut, in ? kCardKb - usedKb(port) : 0);
    if (format)
        ror::wr32(rdram, format, in ? 1 : 0);
    const int32_t result = in ? (g_seen[port] ? 0 : -1) : kNoCard;
    g_seen[port] = in;
    accept(ctx, 1, result);
}

// sceMcOpen(port, slot, name, mode): mode 1 read, 2 write, 3 both, | 0x200 create. Result: a descriptor >= 0.
void ror_sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = ror::arg(ctx, 0) & 1, mode = ror::arg(ctx, 3);
    const std::string name = guestStr(rdram, ror::arg(ctx, 2));
    std::string guest;
    int32_t result = kNoEntry;
    if (!present(port))
        result = kNoCard;
    else if (normalise(port, name, guest) && guest != "/")
    {
        const fs::path p = hostPath(port, guest);
        std::error_code ec;
        if (fs::is_directory(p, ec))
            result = kDenied;
        else if (!fs::exists(p, ec) && (!(mode & 0x200) || !fs::is_directory(p.parent_path(), ec)))
            result = kNoEntry;
        else if (g_files.size() >= 32)
            result = kTooManyFiles;
        else
        {
            std::FILE *f = nullptr;
            if (!fs::exists(p, ec))
                _wfopen_s(&f, p.c_str(), L"w+b");
            else
                _wfopen_s(&f, p.c_str(), (mode & 2) ? L"r+b" : L"rb");
            if (f)
            {
                int32_t fd = 0;
                while (g_files.count(fd))
                    ++fd;
                g_files[fd] = {f, guest};
                result = fd;
            }
            else
                result = kDenied;
        }
    }
    ror::log("memcard: open %u:%s mode %#x -> %d", port + 1, guest.empty() ? name.c_str() : guest.c_str(), mode, result);
    accept(ctx, 2, result);
}

void ror_sceMcClose(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto it = g_files.find(ror::sarg(ctx, 0));
    if (it == g_files.end())
        return accept(ctx, 3, kNoEntry);
    std::fclose(it->second.f);
    g_files.erase(it);
    accept(ctx, 3, 0);
}

// sceMcSeek(fd, offset, origin 0 set / 1 current / 2 end): the new position.
void ror_sceMcSeek(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto it = g_files.find(ror::sarg(ctx, 0));
    if (it == g_files.end())
        return accept(ctx, 4, kNoEntry);
    const int origin = ror::sarg(ctx, 2) == 1 ? SEEK_CUR : ror::sarg(ctx, 2) == 2 ? SEEK_END : SEEK_SET;
    std::fseek(it->second.f, ror::sarg(ctx, 1), origin);
    accept(ctx, 4, static_cast<int32_t>(std::ftell(it->second.f)));
}

// sceMcRead(fd, buffer, size) / sceMcWrite(fd, buffer, size): bytes transferred.
void ror_sceMcRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const auto it = g_files.find(ror::sarg(ctx, 0));
    const uint32_t size = ror::arg(ctx, 2);
    uint8_t *buf = ror::ptr(rdram, ror::arg(ctx, 1), size);
    if (it == g_files.end() || !buf)
        return accept(ctx, 5, kNoEntry);
    const size_t n = std::fread(buf, 1, size, it->second.f);
    ror::log("memcard: read %s: %zu of %u bytes", it->second.guest.c_str(), n, size);
    accept(ctx, 5, static_cast<int32_t>(n));
}

void ror_sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const auto it = g_files.find(ror::sarg(ctx, 0));
    const uint32_t size = ror::arg(ctx, 2);
    const uint8_t *buf = ror::ptr(rdram, ror::arg(ctx, 1), size);
    if (it == g_files.end() || !buf)
        return accept(ctx, 6, kNoEntry);
    const size_t n = std::fwrite(buf, 1, size, it->second.f);
    std::fflush(it->second.f);
    ror::log("memcard: write %s: %zu of %u bytes", it->second.guest.c_str(), n, size);
    accept(ctx, 6, static_cast<int32_t>(n));
}

void ror_sceMcFlush(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const auto it = g_files.find(ror::sarg(ctx, 0));
    if (it != g_files.end())
        std::fflush(it->second.f);
    accept(ctx, 0xa, it == g_files.end() ? kNoEntry : 0);
}

// 0x24f680(port, slot, name): libmc's mkdir (an open with the directory flag, command 0xb). An existing name: -4.
void ror_sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = ror::arg(ctx, 0) & 1;
    std::string guest;
    int32_t result = kNoEntry;
    std::error_code ec;
    if (!present(port))
        result = kNoCard;
    else if (normalise(port, guestStr(rdram, ror::arg(ctx, 2)), guest) && guest != "/")
    {
        const fs::path p = hostPath(port, guest);
        if (!fs::exists(p, ec) && fs::is_directory(p.parent_path(), ec) && fs::create_directory(p, ec))
            result = 0;
    }
    ror::log("memcard: mkdir %u:%s -> %d", port + 1, guest.c_str(), result);
    accept(ctx, 0xb, result);
}

// sceMcChdir(port, slot, new directory, current directory out): the out buffer gets the directory before the change.
void ror_sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = ror::arg(ctx, 0) & 1;
    if (const uint32_t cur = ror::arg(ctx, 3))
    {
        const std::string &c = g_cwd[port];
        if (uint8_t *p = ror::ptr(rdram, cur, static_cast<uint32_t>(c.size() + 1)))
            std::copy(c.c_str(), c.c_str() + c.size() + 1, p);
    }
    std::string guest;
    int32_t result = kNoEntry;
    std::error_code ec;
    const std::string name = guestStr(rdram, ror::arg(ctx, 2));
    if (!present(port))
        result = kNoCard;
    else if (name.empty())
        result = 0;
    else if (normalise(port, name, guest) && fs::is_directory(hostPath(port, guest), ec))
    {
        g_cwd[port] = guest;
        result = 0;
    }
    accept(ctx, 0xc, result);
}

// sceMcGetDir(port, slot, name, mode, maxent, table): mode 0 lists the entries matching name's last component (a
// subdirectory lists "." and ".." first, as a real card), mode 1 continues the listing; result = entries written.
void ror_sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = ror::arg(ctx, 0) & 1, mode = ror::arg(ctx, 3), table = ror::arg(ctx, 5);
    const int32_t maxent = ror::sarg(ctx, 4);
    if (!present(port))
        return accept(ctx, 0xd, kNoCard);
    Listing &l = g_listing[port];
    const std::string name = guestStr(rdram, ror::arg(ctx, 2));
    if (mode == 0)
    {
        l = {};
        const size_t slash = name.rfind('/');
        const std::string dir = slash == std::string::npos ? "" : name.substr(0, slash + 1);
        const std::string pat = slash == std::string::npos ? name : name.substr(slash + 1);
        std::string guest;
        std::error_code ec;
        if (!normalise(port, dir.empty() ? "." : dir, guest) || !fs::is_directory(hostPath(port, guest), ec))
            return accept(ctx, 0xd, kNoEntry);
        const fs::path hp = hostPath(port, guest);
        if (guest != "/")
            for (const char *dot : {".", ".."})
                if (match(pat.c_str(), dot))
                    l.entries.push_back(entry(dot, hp, true));
        std::vector<fs::directory_entry> found;
        for (const auto &de : fs::directory_iterator(hp, ec))
            found.push_back(de);
        std::sort(found.begin(), found.end());
        for (const auto &de : found)
        {
            const std::string n = de.path().filename().string();
            if (match(pat.c_str(), n.c_str()))
                l.entries.push_back(entry(n, de.path(), de.is_directory()));
        }
        ror::log("memcard: getdir %u:%s -> %zu entries", port + 1, name.c_str(), l.entries.size());
    }
    int32_t n = 0;
    for (; n < maxent && l.next < l.entries.size(); ++n, ++l.next)
        if (uint8_t *p = ror::ptr(rdram, table + 64 * n, 64))
            std::copy(l.entries[l.next].begin(), l.entries[l.next].end(), p);
    accept(ctx, 0xd, n);
}

// sceMcDelete(port, slot, name): a file, or an empty directory (else -6).
void ror_sceMcDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = ror::arg(ctx, 0) & 1;
    std::string guest;
    int32_t result = kNoEntry;
    std::error_code ec;
    if (!present(port))
        result = kNoCard;
    else if (normalise(port, guestStr(rdram, ror::arg(ctx, 2)), guest) && guest != "/")
    {
        const fs::path p = hostPath(port, guest);
        if (fs::is_directory(p, ec) && !fs::is_empty(p, ec))
            result = kNotEmpty;
        else if (fs::exists(p, ec) && fs::remove(p, ec))
            result = 0;
    }
    ror::log("memcard: delete %u:%s -> %d", port + 1, guest.c_str(), result);
    accept(ctx, 0xf, result);
}

// Format / unformat: our cards are always formatted; neither erases the host folder (logged).
void ror_sceMcFormat(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ror::log("memcard: format %u ignored (the host folder is kept)", (ror::arg(ctx, 0) & 1) + 1);
    accept(ctx, 0x10, present(ror::arg(ctx, 0) & 1) ? 0 : kNoCard);
}

void ror_sceMcUnformat(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ror::log("memcard: unformat %u ignored", (ror::arg(ctx, 0) & 1) + 1);
    accept(ctx, 0x11, present(ror::arg(ctx, 0) & 1) ? 0 : kNoCard);
}

// sceMcGetEntSpace(port, slot, path): free directory entries (UNVERIFIED count; the placeholder's 1000).
void ror_sceMcGetEntSpace(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    accept(ctx, 0x12, present(ror::arg(ctx, 0) & 1) ? 1000 : kNoCard);
}

// 0x250648(port, slot, old path, new name): libmc's rename (FileInfo RPC 0xe with the name flag 0x10, command 0x13).
void ror_sceMcRename(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t port = ror::arg(ctx, 0) & 1;
    const std::string newName = guestStr(rdram, ror::arg(ctx, 3));
    std::string guest;
    int32_t result = kNoEntry;
    std::error_code ec;
    if (!present(port))
        result = kNoCard;
    else if (normalise(port, guestStr(rdram, ror::arg(ctx, 2)), guest) && guest != "/" && !newName.empty() &&
             newName.find('/') == std::string::npos)
    {
        const fs::path p = hostPath(port, guest);
        if (fs::exists(p, ec))
        {
            fs::rename(p, p.parent_path() / fs::path(newName), ec);
            result = ec ? kDenied : 0;
        }
    }
    ror::log("memcard: rename %u:%s -> %s: %d", port + 1, guest.c_str(), newName.c_str(), result);
    accept(ctx, 0x13, result);
}

// sceMcSync(mode, cmd*, result*) 0x24fb98: 1 with the finished call's command and result, -1 if none is pending.
void ror_sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    if (!g_mc.pending)
    {
        ror::ret(ctx, static_cast<uint32_t>(-1));
        return;
    }
    if (const uint32_t cmd = ror::arg(ctx, 1))
        ror::wr32(rdram, cmd, static_cast<uint32_t>(g_mc.cmd));
    if (const uint32_t result = ror::arg(ctx, 2))
        ror::wr32(rdram, result, static_cast<uint32_t>(g_mc.result));
    g_mc.pending = false;
    ror::ret(ctx, 1);
}
