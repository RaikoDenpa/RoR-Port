// The game window (port.ini, ror_options.cpp): raylib's window, which PCSX2's GS presents into through a child window
// that follows its client size (ror_pcsx2host_win32.cpp: pcsx2WindowLoop resizes the child, the GS resizes its swap
// chain at the next VSync). Main thread only, from the external presentation's init and loop callbacks (ror_gs.cpp).
//   window_width / window_height   client size at start, centred on the monitor (the runtime opens 640x448, resizable)
//   fullscreen = 1                 borderless fullscreen at start; F11 toggles it; the cursor hides while fullscreen
//   volume = 0..100                raylib's master volume (the game's sound mix and the movies' audio streams)
// The picture keeps the PS2's aspect inside the window: gs.ini's [EmuCore/GS] AspectRatio (PCSX2's setting).
#include "ror.h"

#include "raylib.h"

#include <algorithm>

namespace
{
    void applyCursor()
    {
        if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
            HideCursor();
        else
            ShowCursor();
    }
}

void ror::windowSetup()
{
    SetWindowMinSize(320, 224);
    const int w = optionInt("window_width", 0), h = optionInt("window_height", 0);
    const int m = GetCurrentMonitor();
    if (w >= 320 && h >= 224)
    {
        SetWindowSize(w, h);
        SetWindowPosition(std::max(0, (GetMonitorWidth(m) - w) / 2), std::max(32, (GetMonitorHeight(m) - h) / 2));
    }
    if (optionInt("fullscreen", 0) == 1)
        ToggleBorderlessWindowed();
    applyCursor();
    const int volume = optionInt("volume", 100);
    SetMasterVolume(static_cast<float>(volume < 0 ? 0 : volume > 100 ? 100 : volume) / 100.0f);
    ror::log("window %dx%d%s, volume %d%%", GetScreenWidth(), GetScreenHeight(),
             IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) ? " (fullscreen)" : "", volume);
}

void ror::windowLoop()
{
    if (IsKeyPressed(KEY_F11))
    {
        ToggleBorderlessWindowed();
        applyCursor();
    }
}
