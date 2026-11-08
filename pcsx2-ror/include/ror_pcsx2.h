/* C API of pcsx2-ror.dll: PCSX2's GS (and, from step 3, its VIF1 + microVU1) for the Rule of Rose port
 * (notes/renderer.md 6.5a). The runner (src/ror_pcsx2host_win32.cpp) and the replay tool ror-gsreplay
 * (pcsx2-ror/gsreplay.cpp) are its clients.
 *
 * Boundary rules: plain C types only; every buffer is owned by the caller (the DLL never hands out memory to free,
 * the two sides may use different C runtimes); errors are return codes (ROR_PCSX2_E_*); the caller passes the API
 * version it was built against to ror_pcsx2_open, which refuses any other, so a stale DLL fails loudly. */
#ifndef ROR_PCSX2_H
#define ROR_PCSX2_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ROR_PCSX2_EXPORTS)
#define ROR_PCSX2_API __declspec(dllexport)
#else
#define ROR_PCSX2_API __declspec(dllimport)
#endif

/* Bumped on every change of a signature or of the meaning of an argument. */
#define ROR_PCSX2_API_VERSION 5

enum
{
    ROR_PCSX2_OK = 0,
    ROR_PCSX2_E_VERSION = -1, /* the caller's API version is not the DLL's */
    ROR_PCSX2_E_ARG = -2,     /* a null pointer or a bad value */
    ROR_PCSX2_E_BUFFER = -3,  /* the caller's buffer is too small; nothing was written */
    ROR_PCSX2_E_STATE = -4,   /* not open, or already open */
    ROR_PCSX2_E_FAILED = -5   /* PCSX2 failed; the reason went to the log sink */
};

enum
{
    ROR_PCSX2_LOG_ERROR = 0,
    ROR_PCSX2_LOG_WARNING = 1,
    ROR_PCSX2_LOG_INFO = 2
};

/* Log sink: every message the DLL and PCSX2's console write. May be called from any of PCSX2's threads (the GS
 * thread, the VU thread), so it must be thread-safe. `message` is valid during the call only. */
typedef void (*ror_pcsx2_log_fn)(void *user, int level, const char *message);

/* The API version this DLL implements: for a check before ror_pcsx2_open. */
ROR_PCSX2_API int ror_pcsx2_api_version(void);

/* Checks `api_version` against the DLL's and keeps the log sink (may be null). Nothing of PCSX2 starts yet. */
ROR_PCSX2_API int ror_pcsx2_open(int api_version, ror_pcsx2_log_fn log, void *user);

/* PCSX2's version ("v2.8.2"-style tag + revision) into buf, NUL-terminated. */
ROR_PCSX2_API int ror_pcsx2_version(char *buf, size_t size);

/* Closes the GS if it is still open, then forgets the log sink. */
ROR_PCSX2_API void ror_pcsx2_close(void);

/* ---- The GS (renderer plan step 2). PCSX2 feeds its GS thread through a single-producer ring, so every
 * ror_pcsx2_gs_* call must come from one thread, the caller's "CPU thread" (the runner's game thread). ---- */

/* ror_pcsx2_gs_open flags. */
#define ROR_PCSX2_OPEN_VIF1 1u /* also PCSX2's VIF1 + microVU1 on its MTVU thread: ror_pcsx2_vif1_*, ror_pcsx2_vu1_* */

/* PCSX2's GS settings from an ini file (config/gs.ini, notes/renderer.md 6.13), applied over PCSX2's defaults and the
 * DLL's own overrides. Only before the first ror_pcsx2_gs_open (settings are made once per process; any thread, as
 * it only stores them). `path` UTF-8.
 * Section [gs]: renderer = auto|d3d11|d3d12|vulkan|opengl|software, upscale = 1..12 (internal resolution multiplier),
 * texture_filtering = nearest|ps2|forced|forced_no_sprites. Any other section is passed through as PCSX2's own
 * (e.g. [EmuCore/GS] with its ini keys). ROR_PCSX2_E_ARG: a line or value not understood (logged; the rest applies). */
ROR_PCSX2_API int ror_pcsx2_gs_config(const char *path);

/* Settings from PCSX2's defaults, then MTGS (the GS thread) opens and presents into `window`, a Win32 HWND the caller
 * owns and keeps alive until ror_pcsx2_gs_close (a child window of the game window), `width` x `height` pixels.
 * `window` null: no window (surfaceless), nothing is presented, screenshots still work (the replay tool).
 * `flags`: ROR_PCSX2_OPEN_*. PCSX2's folders: its resources/ next to the exe; data (shader cache) next to the exe too
 * (portable.txt). */
ROR_PCSX2_API int ror_pcsx2_gs_open(void *window, int width, int height, unsigned flags);

/* The window's new client size in pixels (the caller resizes the window itself). */
ROR_PCSX2_API int ror_pcsx2_gs_resize(int width, int height);

/* One VBlank: `regs` = the GS privileged registers as the EE sees them at 0x12000000 (PMODE, SMODE2, DISPFB, DISPLAY,
 * BGCOLOR, CSR, IMR, ...), `size` bytes up to 8192 (the rest is zero); then PCSX2's VSync (it presents a frame). */
ROR_PCSX2_API int ror_pcsx2_gs_vsync(const void *regs, size_t size);

/* Replaces the GS state: `state` = PCSX2's GS freeze block (GSState::Freeze, the state of a GS dump), `regs` = the
 * privileged registers that go with it (as ror_pcsx2_gs_vsync; may be null with `regs_size` 0). */
ROR_PCSX2_API int ror_pcsx2_gs_load_state(const void *state, size_t size, const void *regs, size_t regs_size);

/* GIF data as the GS receives it from GIF path `path` (1, 2 or 3): whole GIF packets, `size` a multiple of 16.
 * PATH1 and PATH2 only without ROR_PCSX2_OPEN_VIF1 (a GS dump's transfers; with VIF1 they come from PCSX2's VIF1 and
 * VU1); the live feed is PATH3. */
ROR_PCSX2_API int ror_pcsx2_gs_transfer(int path, const void *data, size_t size);

/* The GS state as a GS dump holds it (GSState::Freeze, after the hardware renderer's texture cache is read back to
 * local memory): into `buf`; `*needed` (may be null) gets the size. ROR_PCSX2_E_BUFFER when `buf` is null or smaller. */
ROR_PCSX2_API int ror_pcsx2_gs_save_state(void *buf, size_t size, size_t *needed);

/* A GS->host transfer (BITBLTBUF/TRXPOS/TRXREG and TRXDIR = 1 sent before): `size` bytes (a multiple of 16) of
 * local memory into `buf`, once the GS thread has drawn everything sent before. */
ROR_PCSX2_API int ror_pcsx2_gs_read_fifo(void *buf, size_t size);

/* Saves the frame the next ror_pcsx2_gs_vsync presents as a PNG at `path` (UTF-8), at PCSX2's internal resolution
 * (window size does not matter), written asynchronously: complete after ror_pcsx2_gs_close. One per VSync. */
ROR_PCSX2_API int ror_pcsx2_gs_screenshot(const char *path);

/* Counters since ror_pcsx2_gs_open. `size` = sizeof(ror_pcsx2_gs_counters) as the caller knows it. */
typedef struct ror_pcsx2_gs_counters
{
    unsigned long long vsyncs;       /* ror_pcsx2_gs_vsync calls */
    unsigned long long presents;     /* frames the GS thread presented to the window */
    unsigned long long vif1_streams; /* ror_pcsx2_vif1_transfer calls */
    unsigned long long vif1_stalls;  /* VIF1 stalls (FLUSH & co.) resolved by waiting for VU1 */
    unsigned long long gs_finish;    /* VSync intervals in which VU1's output (MTVU) set CSR.FINISH, SIGNAL, or */
    unsigned long long gs_signal;    /* changed LBLID: PCSX2 handles them, the EE is not told yet (a log line); */
    unsigned long long gs_label;     /* the game uses none in the references (notes/renderer.md 6.8) */
} ror_pcsx2_gs_counters;
ROR_PCSX2_API int ror_pcsx2_gs_get_counters(ror_pcsx2_gs_counters *out, size_t size);

/* Waits for the GS thread to finish its work, closes it and stops it (and VIF1/VU1 with ROR_PCSX2_OPEN_VIF1). */
ROR_PCSX2_API void ror_pcsx2_gs_close(void);

/* ---- VIF1 + VU1 (renderer plan step 3; ror_pcsx2_gs_open with ROR_PCSX2_OPEN_VIF1), same thread as the GS calls. ---- */

/* One VIF1 input stream as DMA channel 1 delivers it (VIFcodes and their data, `size` a multiple of 4): PCSX2's VIF1
 * decodes it, VU1 programs run on the MTVU thread, their XGKICKs and the DIRECTs go to the GS in order with the GS
 * calls. Returns when the VIF has consumed it all (a FLUSH stall waits for VU1). */
ROR_PCSX2_API int ror_pcsx2_vif1_transfer(const void *data, size_t size);

/* Waits until VU1 has run everything sent so far. */
ROR_PCSX2_API int ror_pcsx2_vu1_wait(void);

/* VU1 + VIF1 state in the layout of a VIF1 GS dump's record 0x40 (src/ror_gsdump.cpp): VU1 micro memory (16 KB), data
 * memory (16 KB), the VIF1 registers (23 u32: stat fbrst err mark cycle mode num mask code itops base ofst tops itop
 * top row[4] col[4]), VF[32][4] (float), VI[16] (u32), ACC[4], Q, P, I (float), R, MAC, clip, status (u32). */
#define ROR_PCSX2_VU1_STATE_SIZE 33480
ROR_PCSX2_API int ror_pcsx2_vu1_get_state(void *buf, size_t size);
ROR_PCSX2_API int ror_pcsx2_vu1_set_state(const void *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
