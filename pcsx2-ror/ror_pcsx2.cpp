// pcsx2-ror.dll: the PCSX2 side of the Rule of Rose port (notes/renderer.md 6.5a), built inside PCSX2's own CMake
// (tools/pcsx2-patches/0003) and linked against its core. This file: the versioned C API's open/close and the log
// sink, which also receives PCSX2's console; the GS is gs.cpp (step 2), the VIF1 feed follows in step 3.
#include "pcsx2/PrecompiledHeader.h"

#include "ror_internal.h"
#include "ror_pcsx2.h"

#include "common/Console.h"

#include "pcsx2/BuildVersion.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{
    std::mutex s_mutex;
    bool s_open = false;
    ror_pcsx2_log_fn s_log = nullptr;
    void *s_log_user = nullptr;

    // PCSX2's console (Console.WriteLn & co., any thread) into the sink, one line per message.
    void consoleSink(LOGLEVEL level, ConsoleColors, std::string_view message)
    {
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
            message.remove_suffix(1);
        if (message.empty())
            return;
        const int l = level <= LOGLEVEL_ERROR ? ROR_PCSX2_LOG_ERROR : level == LOGLEVEL_WARNING ? ROR_PCSX2_LOG_WARNING :
                                                                                               ROR_PCSX2_LOG_INFO;
        ror_pcsx2::log(l, "PCSX2: %.*s", static_cast<int>(message.size()), message.data());
    }
} // namespace

bool ror_pcsx2::isOpen()
{
    return s_open;
}

void ror_pcsx2::log(int level, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (s_log)
        s_log(s_log_user, level, line);
}

using ror_pcsx2::log;

extern "C" int ror_pcsx2_api_version(void)
{
    return ROR_PCSX2_API_VERSION;
}

extern "C" int ror_pcsx2_open(int api_version, ror_pcsx2_log_fn log_fn, void *user)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (api_version != ROR_PCSX2_API_VERSION)
    {
        if (log_fn)
        {
            char line[160];
            std::snprintf(line, sizeof(line), "pcsx2-ror: caller built for API v%d, this DLL is v%d", api_version,
                ROR_PCSX2_API_VERSION);
            log_fn(user, ROR_PCSX2_LOG_ERROR, line);
        }
        return ROR_PCSX2_E_VERSION;
    }
    if (s_open)
        return ROR_PCSX2_E_STATE;
    s_log = log_fn;
    s_log_user = user;
    s_open = true;
    Log::SetHostOutputLevel(LOGLEVEL_INFO, consoleSink);
    log(ROR_PCSX2_LOG_INFO, "pcsx2-ror: API v%d, PCSX2 %s (%s)", ROR_PCSX2_API_VERSION, BuildVersion::GitTag,
        BuildVersion::GitRev);
    return ROR_PCSX2_OK;
}

extern "C" int ror_pcsx2_version(char *buf, size_t size)
{
    if (!buf || size == 0)
        return ROR_PCSX2_E_ARG;
    const int n = std::snprintf(nullptr, 0, "%s (%s)", BuildVersion::GitTag, BuildVersion::GitRev);
    if (n < 0 || static_cast<size_t>(n) + 1 > size)
        return ROR_PCSX2_E_BUFFER;
    std::snprintf(buf, size, "%s (%s)", BuildVersion::GitTag, BuildVersion::GitRev);
    return ROR_PCSX2_OK;
}

extern "C" void ror_pcsx2_close(void)
{
    ror_pcsx2_gs_close();
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_open)
        return;
    log(ROR_PCSX2_LOG_INFO, "pcsx2-ror: closed");
    Log::SetHostOutputLevel(LOGLEVEL_NONE, nullptr);
    s_open = false;
    s_log = nullptr;
    s_log_user = nullptr;
}
