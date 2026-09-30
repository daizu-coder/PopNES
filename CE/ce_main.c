/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Part of PopNES, a QuickNES-core port for the SHARP Brain PW-G5200.
 * This file is MIT (CE/LICENSE); PopNES as a whole (AppMain.exe) is
 * distributed under GPL-2.0 - see CE/LICENSING.md and
 * CE/THIRDPARTY_LICENSES.txt.
 */
/*
 * Windows CE libretro frontend template (the shared CE app template).
 *
 * Distilled from two hardware-validated CE ports (PicoDrive CE and
 * snes9x2002 CE) that converged on an identical shape: a thin shell that
 * drives retro_init/retro_load_game/retro_run and turns the five
 * retro_set_* callbacks into real GAPI/waveOut/key I/O, never patching
 * the core itself. The comments throughout this file record the
 * iteration history and hard-won lessons (GAPI/aygshell init order, waveOut quirks, single-
 * instance handling, etc.) that shaped this file.
 *
 * To port a new libretro core onto this shell:
 *   1. Fill in ce_app_config.h (app title, window class, mutex name,
 *      config/log filenames, registry key).
 *   2. Point the Makefile's libretro core object/library at your core.
 *   3. Adjust CeVideoEnvGetVariable() in ce_video.c to your core's own
 *      option keys (ce_app_config.h's CE_APP_CORE_OPT_PREFIX comment
 *      explains why this step can't be a mechanical find/replace).
 *   4. Adjust the ROM extension list in ce_fileopen.c and the joypad
 *      button set in ce_input.c/ce_res.rc to your system.
 */

#include "ce_app_config.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#include <libretro.h>

#include "ce_log.h"
#include "ce_display.h"
#include "ce_input.h"
#include "ce_audio.h"
#include "ce_video.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_fileopen.h"
#include "ce_bmpfont.h"
#include "ce_resource.h"

static const wchar_t kWndClassName[] = CE_APP_WND_CLASS;
static const wchar_t kMutexName[]    = CE_APP_MUTEX_NAME;
static const wchar_t kAppTitle[]     = CE_APP_TITLE;

static HWND   g_hwnd     = NULL;
static HANDLE g_mutex    = NULL;
static volatile int g_running = 0;

static wchar_t g_romPath[MAX_PATH] = L""; /* last successfully-loaded ROM's path, for save-state/SRAM file naming */

/* Last frame the core actually rendered (ce_video_refresh), for the main
 * menu's Screenshot button (ported from the sister PopSNES port). Points
 * into libretro.cpp's static video_buffer, which never moves and which
 * nothing overwrites while the menu has the game paused. Cleared on
 * every ROM load so a new game can't save the previous one's frame. */
static const void *g_lastFrame = NULL;
static unsigned g_lastFrameW = 0, g_lastFrameH = 0;
static size_t g_lastFramePitch = 0;

/* g_romLoaded: a game has been successfully retro_load_game()'d at
 * least once (stays true across File>Open reloads until exit).
 * g_paused: the touch-to-reveal menu is up right now - retro_run() is
 * not called and GAPI is not holding the display while this is true, so
 * normal GDI (the menu, WM_PAINT) can draw. */
static int  g_romLoaded = 0;
static int  g_paused    = 0;

/* Target frame rate for WinMain's pacing loop (see its own comment) -
 * rounded to the nearest whole fps since the pacer's accumulator is
 * plain integer math (this device's VFP is software-emulated, see
 * ce_audio.c's resampler comment - same reasoning applies to a value
 * touched every frame). QuickNES always reports exactly 60.000 (NTSC),
 * so the rounding costs nothing in practice; set from LoadRomPath once
 * a ROM's actually loaded, default matches that same value for the
 * (never paced - g_romLoaded is false) menu screen. */
static unsigned g_targetFpsRounded = 60;

/* Audio-backed frame pacing threshold (ms of buffered audio) - ported
 * from the sister PicoDrive CE (PopSG) port. See the main loop's own
 * comment (WinMain) for how this is used. */
#define CE_FRAME_PACING_HIGH_MS 64

/* Handles for a freshly-created replacement process (RestartProcess,
 * below) waiting to be resumed - see that function's comment for why
 * the resume has to be deferred until CeShutdown() has already released
 * g_mutex. NULL whenever no restart is pending. */
static HANDLE s_restartResumeThread  = NULL;
static HANDLE s_restartResumeProcess = NULL;

static void CeShutdown(int exitCode); /* used by MainMenuDlgProc, below */
static void CeShowShellChrome(HWND hwnd); /* used by CeShutdown, defined further below */
static void CeHideShellChrome(HWND hwnd); /* used by ShowMainMenuDialog and WinMain, defined further below */
static int  LoadRomPath(HWND hwnd, const wchar_t *romPath); /* used by WinMain (command-line hand-off) and LoadRomFlow, defined further below */

/* ------------------------------------------------------------------ */
/* libretro callbacks                                                  */
/* ------------------------------------------------------------------ */

/* Set by the core via RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK
 * whenever Video Config's Frame Skip is above 0 (ce_video.c) - NULL
 * otherwise (Frame Skip Off, or before retro_load_game()'s first
 * check_variables() call). Invoked once per frame from WinMain's main
 * loop, right before retro_run(), per that environment call's contract -
 * see the call site below. */
static retro_audio_buffer_status_callback_t g_audioBuffStatusCb = NULL;

/* Previous frame's retro_run() wall-clock time, in ms - written by the
 * perf-instrumentation block around the retro_run() call site in
 * WinMain's loop (below), read by ce_environment()'s
 * RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE handler as one of
 * CeVideoFrameSkipDecide()'s two "falling behind" signals - see
 * ce_video.h. That environment call fires from inside retro_run()
 * itself, before this frame's own emulation work happens, so the value
 * read there is always the *previous* completed frame's timing, never
 * this one's (which hasn't finished yet). Starts at 0 (never triggers a
 * skip) until the first frame has actually completed once. */
static unsigned s_lastRetroRunMs = 0;

static bool ce_environment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        enum retro_pixel_format *fmt = (enum retro_pixel_format *)data;
        /* This device's GAPI surface is RGB565 (confirmed on both prior
         * CE ports); refuse anything else so the core doesn't silently
         * assume a format we can't display. PicoDrive's retro_load_game()
         * hard-requires this to succeed (bails out with "RGB565 support
         * required" otherwise), so this must return true for RGB565. */
        return (*fmt == RETRO_PIXEL_FORMAT_RGB565);
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        /* Video Config's sprite-limit/frame-skip settings (ce_video.c)
         * ride the core's own existing core-options protocol
         * (picodrive_sprlim / picodrive_frameskip* - see
         * check_variables() in libretro/libretro.c) instead of a new
         * side channel - CE just needs to answer these two queries. */
        struct retro_variable *var = (struct retro_variable *)data;
        return CeVideoEnvGetVariable(var->key, &var->value) ? true : false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        /* Lets a Video Config change made mid-session (ROM already
         * loaded, retro_load_game() - which would otherwise be the only
         * point check_variables() re-reads these - not called again)
         * take effect on the very next retro_run() instead of needing a
         * File>Open reload. */
        *(bool *)data = CeVideoConsumeDirty() ? true : false;
        return true;

    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
    {
        /* Real frame skip - see ce_video.h's CeVideoFrameSkipDecide
         * comment. Unlike SET_AUDIO_BUFFER_STATUS_
         * CALLBACK just below (never called by this core, so
         * g_audioBuffStatusCb stays NULL forever), libretro.cpp's
         * retro_run() reads this environment call unconditionally,
         * every frame, regardless of whether CE answers it - leaving it
         * unanswered (falling through to `return false` like every
         * other unhandled cmd below) is exactly equivalent to reporting
         * every bit set, i.e. what happened before Frame Skip's dial
         * existed. Bit 1 (audio) is always reported enabled - only bit
         * 0 (video) is ever toggled off, and only when Video Config's
         * Frame Skip dial (ce_video.c) is above 0. Bits 2/3 (fast
         * savestates / hard-disable-audio) are left at 0 - this frontend
         * makes neither guarantee. */
        if (data)
        {
            int *flags = (int *)data;
            int render = CeVideoFrameSkipDecide(
                s_lastRetroRunMs,
                (g_targetFpsRounded > 0) ? (1000u / g_targetFpsRounded) : 17u);

            *flags = RETRO_AV_ENABLE_AUDIO | (render ? RETRO_AV_ENABLE_VIDEO : 0);
        }
        return true;
    }

    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
    {
        /* The core only asks for this when frame skip is "auto" (see
         * check_variables()/init_frameskip() in libretro/libretro.c) -
         * without answering it, that mode silently never skips anything
         * (retro_audio_buff_active stays false forever). data is NULL
         * when the core wants to unregister. */
        const struct retro_audio_buffer_status_callback *cb =
            (const struct retro_audio_buffer_status_callback *)data;
        g_audioBuffStatusCb = cb ? cb->callback : NULL;
        return true;
    }

    default:
        /* Everything else (GET_LOG_INTERFACE handled separately by the
         * core itself calling environ_cb before this is even wired up,
         * GET_SYSTEM_DIRECTORY for BIOS lookup, SET_CORE_OPTIONS*,
         * GET_INPUT_BITMASKS, ...) is optional per the libretro API
         * contract - returning false tells the core to use its built-in
         * defaults, which is exactly what we want until there is a
         * settings UI for each of those too. If a future core DOES need
         * GET_SYSTEM_DIRECTORY (a mandatory BIOS/firmware file, e.g.
         * gpSP/GBA), don't just answer it with AppMain.exe's own folder
         * converted to narrow - that's unreliable on this device
         * whenever that folder contains non-ASCII characters; copy the
         * file to a cache with the wide APIs instead, the pattern that
         * actually held up on hardware. */
        return false;
    }
}

static void ce_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    /* Keeps Video Config's Frame Skip consecutive-skip counter (see
     * ce_video.h) in sync with what the core actually did this frame:
     * data is NULL exactly when the core skipped rendering. */
    CeVideoFrameSkipNotifyRendered(data != NULL);

    if (!data)
        return; /* duplicate/skipped frame - nothing new to draw */

    g_lastFrame = data;
    g_lastFrameW = width;
    g_lastFrameH = height;
    g_lastFramePitch = pitch;

    /* Self-contained per call (width/height/pitch given fresh every
     * time, and always RGB565 per ce_environment()'s SET_PIXEL_FORMAT
     * handling above). */
    CeDisplayBlitRGB565(data, width, height, (unsigned)pitch);
}

static void ce_audio_sample_noop(int16_t left, int16_t right)
{
    /* The core only ever uses retro_set_audio_sample_batch (see
     * retro_set_audio_sample() in libretro.c - it's an intentional
     * no-op setter), but we still register a real callback rather than
     * NULL to avoid relying on that being true forever. */
    (void)left;
    (void)right;
}

/* Temporary perf instrumentation (same pattern as ce_display.c's existing
 * "perf: blit" logging - see CeDisplayBlitRGB565()): platform/libretro/
 * libretro.c calls audio_batch_cb() exactly once per retro_run(), so
 * this is directly comparable, per-frame, to retro_run's own timing
 * below and blit's. Lets retro_run_avg - blit_avg - audio_push_avg
 * stand in for "core CPU/PPU/sound-chip emulation alone", without
 * touching platform/libretro/libretro.c or anything under pico/. Remove
 * once the bottleneck is identified. */
static size_t ce_audio_sample_batch(const int16_t *data, size_t frames)
{
    static unsigned s_accumMs = 0, s_maxMs = 0, s_count = 0;
    DWORD t0 = GetTickCount();
    size_t ret = CeAudioPushSamples(data, frames);
    unsigned elapsed = (unsigned)(GetTickCount() - t0);

    s_accumMs += elapsed;
    if (elapsed > s_maxMs)
        s_maxMs = elapsed;
    if (++s_count >= 60)
    {
        CeLog("perf: audio_push avg=%ums max=%ums over %u frames",
              s_accumMs / s_count, s_maxMs, s_count);
        s_accumMs = 0;
        s_maxMs = 0;
        s_count = 0;
    }
    return ret;
}

static void ce_input_poll(void)
{
    CeInputPoll();
}

static int16_t ce_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return CeInputState(port, device, index, id);
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                         */
/* ------------------------------------------------------------------ */

/* Just resolves and validates the picked path - QuickNES's own
 * retro_get_system_info() sets need_fullpath = false (unlike the sister
 * PicoDrive/gpSP CE ports, which pass the path only and let the core
 * open/read it), so the actual malloc()+fread() preload happens in
 * LoadRomFlow() below, right before it builds the retro_game_info the
 * core actually reads (game.data/game.size). */
static int PickRom(HWND owner, wchar_t *outPath, size_t outPathCount)
{
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    memset(outPath, 0, outPathCount * sizeof(wchar_t));

    /* Custom listbox-based picker (ce_fileopen.c), not GetOpenFileNameW()
     * - the standard common dialog has no way to render Japanese folder/
     * file names on this device (see ce_fileopen.c's header comment). */
    if (!CeShowFileOpenDialog(owner, outPath, outPathCount))
    {
        CeLog("PickRom: file picker cancelled");
        return 0;
    }

    hFind = FindFirstFileW(outPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        CeLog("PickRom: selected file no longer exists");
        return 0;
    }
    FindClose(hFind);

    return 1;
}

/* ------------------------------------------------------------------ */
/* Battery-backed cartridge save (SRAM)                                */
/* ------------------------------------------------------------------ */

/* Loads "<romPath>.srm" into the core's SRAM, if this game has any
 * (RETRO_MEMORY_SAVE_RAM) and a save file already exists. This is what
 * makes a game's own in-cartridge save feature survive across app
 * restarts, same as a real battery-backed cartridge would - ported from
 * the sister snes9x2002 CE port's own CeLoadSram/CeSaveSram, which added
 * this after a real power-off test showed relying only on graceful
 * app-exit/ROM-switch checkpoints lost saves. No .srm file yet is the normal case for a new
 * game (or one with no SRAM at all) and isn't logged as an error. */
static void CeLoadSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;
    size_t got;

    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"rb");
    if (!f)
    {
        CeLog("CeLoadSram: no .srm file yet (new game, or none saved)");
        return;
    }

    /* Read at most `size` bytes - a mismatched-size .srm (shouldn't
     * happen for a given ROM, but don't overrun the core's buffer if it
     * somehow does) is truncated, not rejected outright. */
    got = fread(sram, 1, size, f);
    fclose(f);
    CeLog("CeLoadSram: loaded %lu of %lu bytes", (unsigned long)got, (unsigned long)size);
}

/* Writes the core's current SRAM out to "<romPath>.srm" - the other half
 * of CeLoadSram(). Called whenever a loaded game's SRAM is about to stop
 * being the live one (File>Open loading a different ROM, or app exit),
 * plus a pause-time checkpoint (ShowMainMenuDialog) and a periodic
 * autosave (WinMain's loop), same three checkpoints the sister
 * snes9x2002 CE port settled on. */
static void CeSaveSram(void)
{
    void *sram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    wchar_t sramPath[MAX_PATH + 8];
    FILE *f;

    if (!sram || size == 0)
        return; /* this game has no battery-backed SRAM - nothing to save */

    _snwprintf(sramPath, MAX_PATH + 8, L"%s.srm", g_romPath);
    f = _wfopen(sramPath, L"wb");
    if (!f)
    {
        CeLog("CeSaveSram: failed to open .srm file for write");
        return;
    }

    fwrite(sram, 1, size, f);
    fclose(f);
    CeLog("CeSaveSram: saved %lu bytes", (unsigned long)size);
}

/* Loads romPath into the core. Shared by LoadRomFlow (below, the Open
 * ROM dialog flow) and WinMain's command-line hand-off (a process
 * relaunched by RestartProcess() for a mid-session ROM switch - see that
 * function's comment). Safe to call both for the very first load and
 * with a game already running (unloads the previous game first).
 * Returns 1 on success, 0 on failure - on failure whatever was running
 * before (if anything) is left untouched. */
static int LoadRomPath(HWND hwnd, const wchar_t *romPath)
{
    struct retro_game_info game;
    struct retro_system_av_info avInfo;
    wchar_t title[MAX_PATH + 32];
    const wchar_t *base;
    FILE *romFile;
    long romSize;
    void *romData;
    int loadOk;

    /* Unlike PicoDrive/snes9x2002 (need_fullpath = true, path-only -
     * see PickRom's comment), QuickNES's retro_get_system_info() sets
     * need_fullpath = false: libretro/libretro.cpp's retro_load_game()
     * reads the whole ROM out of info->data/info->size via its own
     * Mem_File_Reader instead of opening info->path itself. This frontend
     * must therefore preload the file. Opened here with _wfopen() (wide,
     * not the core's own narrow fopen()), which also sidesteps the
     * CP_ACP/CP_UTF8 narrow-path encoding pitfall entirely for ROM
     * loading - there is no narrow path for
     * the core to mis-decode when it never sees one. */
    romFile = _wfopen(romPath, L"rb");
    if (!romFile)
    {
        CeLog("LoadRomPath: failed to open ROM file");
        MessageBoxW(hwnd, L"Failed to open ROM file.", kAppTitle, MB_OK);
        return 0;
    }
    fseek(romFile, 0, SEEK_END);
    romSize = ftell(romFile);
    fseek(romFile, 0, SEEK_SET);
    if (romSize <= 0)
    {
        fclose(romFile);
        CeLog("LoadRomPath: ROM file is empty or ftell failed");
        MessageBoxW(hwnd, L"Failed to open ROM file.", kAppTitle, MB_OK);
        return 0;
    }
    romData = malloc((size_t)romSize);
    if (!romData)
    {
        fclose(romFile);
        CeLog("LoadRomPath: malloc failed for %ld byte ROM", romSize);
        MessageBoxW(hwnd, L"Out of memory loading ROM.", kAppTitle, MB_OK);
        return 0;
    }
    if (fread(romData, 1, (size_t)romSize, romFile) != (size_t)romSize)
    {
        fclose(romFile);
        free(romData);
        CeLog("LoadRomPath: short read on ROM file");
        MessageBoxW(hwnd, L"Failed to read ROM file.", kAppTitle, MB_OK);
        return 0;
    }
    fclose(romFile);

    if (g_romLoaded)
    {
        CeSaveSram(); /* g_romPath/the core's SRAM still refer to the *previous* game here - new one isn't loaded yet */
        retro_unload_game();
    }

    memset(&game, 0, sizeof(game));
    game.path = NULL; /* not used - QuickNES reads game.data/size instead (need_fullpath=false) */
    game.data = romData;
    game.size = (size_t)romSize;

    loadOk = retro_load_game(&game);
    /* Nes_Cart::load_ines() (nes_emu/Nes_Cart.cpp) copies PRG/CHR banks
     * out of game.data into its own resize_prg()/resize_chr() buffers
     * during this call - safe to free our buffer immediately after,
     * regardless of success or failure. */
    free(romData);

    if (!loadOk)
    {
        CeLog("LoadRomPath: retro_load_game failed");
        MessageBoxW(hwnd, L"Failed to load ROM.", kAppTitle, MB_OK);
        g_romLoaded = 0;
        return 0;
    }

    g_romLoaded = 1;
    g_lastFrame = NULL;
    wcsncpy(g_romPath, romPath, MAX_PATH - 1);
    g_romPath[MAX_PATH - 1] = L'\0';

    CeLoadSram();

    retro_get_system_av_info(&avInfo);
    CeLog("LoadRomPath: loaded, geometry=%ux%u fps=%.3f sample_rate=%.0f",
          avInfo.geometry.base_width, avInfo.geometry.base_height,
          avInfo.timing.fps, avInfo.timing.sample_rate);
    CeAudioStart(avInfo.timing.sample_rate);
    g_targetFpsRounded = (avInfo.timing.fps > 0.5) ? (unsigned)(avInfo.timing.fps + 0.5) : 60;

    base = wcsrchr(romPath, L'\\');
    _snwprintf(title, MAX_PATH + 32, L"%s - %s", kAppTitle, base ? base + 1 : romPath);
    SetWindowTextW(g_hwnd, title); /* always the main window, even when
                                     * called with a dialog as `hwnd` */

    return 1;
}

/* Relaunches this app as a brand-new process with romPath as its whole
 * command line - WinMain's lpCmdLine receives it verbatim, no argv
 * splitting (confirmed for the sister gpSP CE port's identical
 * mechanism). Used for a mid-session
 * ROM switch (LoadRomFlow, below) so the new ROM gets a brand-new
 * process and a freshly-opened waveOut device, instead of reusing this
 * process's already-open one - a fresh process is worth trying here for
 * audio reasons.
 *
 * Created CREATE_SUSPENDED and deliberately NOT resumed here: see
 * CeShutdown()'s own comment for why the resume has to wait until this
 * process has released its single-instance mutex. Returns 1 and stashes
 * the handles in s_restartResumeThread/Process for CeShutdown() to
 * resume on success; returns 0 (nothing left running) on failure, so the
 * caller can fall back to loading in-process. */
static int RestartProcess(const wchar_t *romPath)
{
    wchar_t exePath[MAX_PATH];
    wchar_t cmdLine[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!GetModuleFileNameW(NULL, exePath, MAX_PATH))
    {
        CeLog("RestartProcess: GetModuleFileNameW failed, error=%lu", (unsigned long)GetLastError());
        return 0;
    }

    wcsncpy(cmdLine, romPath, MAX_PATH - 1);
    cmdLine[MAX_PATH - 1] = L'\0';

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(exePath, cmdLine, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
    {
        CeLog("RestartProcess: CreateProcessW failed, error=%lu", (unsigned long)GetLastError());
        return 0;
    }

    s_restartResumeThread  = pi.hThread;
    s_restartResumeProcess = pi.hProcess;
    CeLog("RestartProcess: new process created (suspended) for a mid-session ROM switch");
    return 1;
}

/* Picks a ROM (via PickRom) and loads it. If a game is already running
 * (mid-session ROM switch, as opposed to the very first load from the
 * "No ROM loaded" menu), hands off to a freshly-restarted process
 * instead of loading in-place - see RestartProcess()'s comment. Falls
 * back to the normal in-process load if the restart itself couldn't even
 * be started. Returns 1 on success, 0 if the user cancelled the picker
 * or the in-process load failed (the restart path never returns here on
 * success - CeShutdown(0) hands off and exits this process). */
static int LoadRomFlow(HWND hwnd)
{
    wchar_t romPath[MAX_PATH];

    if (!PickRom(hwnd, romPath, MAX_PATH))
        return 0; /* cancelled/failed - PickRom already logged why */

    if (g_romLoaded && RestartProcess(romPath))
        CeShutdown(0); /* never returns - the new process takes over from here */

    return LoadRomPath(hwnd, romPath);
}

/* ------------------------------------------------------------------ */
/* Generic message box (IDD_MSGBOX)                                    */
/* ------------------------------------------------------------------ */

/* MessageBoxW() draws with whatever system font Windows CE finds, and
 * this device has no CJK-capable one any more (jptahoma.ttc dropped for
 * licensing reasons) - Japanese text through it now
 * comes back as tofu. This dialog instead paints its own text with the
 * Shinonome bitmap font (ce_bmpfont.c), the same way every other piece
 * of Japanese UI text in this port already does. Used for the
 * Save/Load State result messages below - the only MessageBoxW() calls
 * in this port that ever carried Japanese text. Ported from the sister
 * gnuboy CE project's own CeShowMsgBox()/MsgBoxDlgProc. */
#define CE_MSGBOX_MAX_TEXT 128
static wchar_t s_msgBoxText[CE_MSGBOX_MAX_TEXT];

#define CE_MSGBOX_MAX_LINE 32

/* Splits text into at most two lines that each fit within maxWidth
 * (real pixels). If the whole string already fits, line2 comes back
 * empty and the caller draws a single centered line. */
static void WrapMsgBoxText(const wchar_t *text, int maxWidth,
                            wchar_t *line1, wchar_t *line2, int lineCap)
{
    int n = (int)wcslen(text);
    int split, i;
    wchar_t probe[CE_MSGBOX_MAX_LINE];

    if (CeBmpFontGetTextWidth(text) <= maxWidth)
    {
        wcsncpy(line1, text, lineCap - 1);
        line1[lineCap - 1] = L'\0';
        line2[0] = L'\0';
        return;
    }

    split = 1; /* always keep at least one character on line1, even if it alone overflows */
    for (i = 1; i <= n && i < CE_MSGBOX_MAX_LINE - 1; i++)
    {
        wcsncpy(probe, text, i);
        probe[i] = L'\0';
        if (CeBmpFontGetTextWidth(probe) > maxWidth)
            break;
        split = i;
    }

    wcsncpy(line1, text, split);
    line1[split] = L'\0';
    wcsncpy(line2, text + split, lineCap - 1);
    line2[lineCap - 1] = L'\0';
}

static WNDPROC s_pMsgBoxOkOrigProc = NULL;

/* Same DLGC_WANTALLKEYS/VK_RETURN/VK_SPACE/VK_ESCAPE subclass pattern as
 * every other BS_OWNERDRAW OK button in this port (MainMenuBtnCtrlProc
 * below, SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc) - BS_OWNERDRAW
 * breaks IsDialogMessage()'s normal DEFPUSHBUTTON Enter routing and
 * Escape-to-Cancel handling alike, so both have to be reimplemented by
 * hand here too. */
static LRESULT CALLBACK MsgBoxBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pMsgBoxOkOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MsgBoxDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        /* IDC_MB_TEXT stays a plain hidden LTEXT, same as IDC_MM_HINT
         * elsewhere in this port - repainted by WM_PAINT below instead
         * of drawn by the control itself. */
        ShowWindow(GetDlgItem(hDlg, IDC_MB_TEXT), SW_HIDE);
        s_pMsgBoxOkOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC, (LONG_PTR)MsgBoxBtnCtrlProc);
        return TRUE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t line1[CE_MSGBOX_MAX_LINE], line2[CE_MSGBOX_MAX_LINE];
        COLORREF fg;
        int rectW, rectH;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_MB_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
        rectW = rc.right - rc.left;
        rectH = rc.bottom - rc.top;

        WrapMsgBoxText(s_msgBoxText, rectW, line1, line2, CE_MSGBOX_MAX_LINE);

        fg = GetSysColor(COLOR_WINDOWTEXT);
        SetBkMode(hdc, TRANSPARENT);

        if (line2[0] == L'\0')
        {
            int x = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
            int y = rc.top + (rectH - CE_BMPFONT_HEIGHT) / 2;
            CeBmpFontDrawTextW(hdc, x, y, line1, fg);
        }
        else
        {
            int lineH = CE_BMPFONT_HEIGHT + 2;
            int y0 = rc.top + (rectH - (CE_BMPFONT_HEIGHT + lineH)) / 2;
            int x1 = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
            int x2 = rc.left + (rectW - CeBmpFontGetTextWidth(line2)) / 2;
            CeBmpFontDrawTextW(hdc, x1, y0, line1, fg);
            CeBmpFontDrawTextW(hdc, x2, y0 + lineH, line2, fg);
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
    return FALSE;
}

static void CeShowMsgBox(HWND owner, const wchar_t *text)
{
    wcsncpy(s_msgBoxText, text, CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
               MAKEINTRESOURCEW(IDD_MSGBOX), owner, MsgBoxDlgProc);
}

/* ------------------------------------------------------------------ */
/* Save State confirmation (IDD_SAVECONFIRM) - user request: Save      */
/* State asks first, and match the sister gnuboy CE project's dialog.  */
/*                                                                    */
/* Ported near-verbatim from gnuboy CE's IDD_CONFIRM/ConfirmDlgProc    */
/* (this replaced the shared CE app template's "-"/はい|いいえ/"+"    */
/* spinner). Two BS_OWNERDRAW buttons - はい/Yes (IDC_SF_YES) and      */
/* いいえ/No (IDC_SF_NO) - with Left/Right/Up/Down moving focus        */
/* between them and the decide key committing the focused one. Shares  */
/* WrapMsgBoxText()/s_msgBoxText with CeShowMsgBox above (the two are  */
/* never on screen at the same time) and the same BS_OWNERDRAW-button  */
/* key handling; just two buttons instead of one. Returns 1 for       */
/* はい/Yes, 0 for いいえ/No or Back.                                  */
/* ------------------------------------------------------------------ */

static int  CeSaveState(void);   /* defined below; used by SaveConfirmDlgProc */

static WNDPROC s_pSaveConfirmBtnOrigProc = NULL;
static int     s_sfPhase                 = 0; /* 0 = asking, 1 = showing result */

static LRESULT CALLBACK SaveConfirmBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int  id   = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDC_SF_NO, 0), (LPARAM)hWnd);
            return 0;

        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
            if (s_sfPhase == 0) /* the result phase has only the OK button */
                SetFocus(GetDlgItem(hDlg, id == IDC_SF_YES ? IDC_SF_NO : IDC_SF_YES));
            return 0;
        }
    }

    return CallWindowProc(s_pSaveConfirmBtnOrigProc, hWnd, message, wParam, lParam);
}

/* Asks, runs the save in place on Yes, then swaps its own text/buttons
 * to the result acknowledgement - never opening a second (nested) modal,
 * which is what made the main menu's "ステートセーブ" button flash to the
 * front during the old confirm -> message-box hand-off. */
static INT_PTR CALLBACK SaveConfirmDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        s_sfPhase = 0;
        ShowWindow(GetDlgItem(hDlg, IDC_SF_TEXT), SW_HIDE);
        SetDlgItemTextW(hDlg, IDC_SF_YES, CeLangIsJapanese() ? L"\x306f\x3044"       /* はい */   : L"Yes");
        SetDlgItemTextW(hDlg, IDC_SF_NO,  CeLangIsJapanese() ? L"\x3044\x3044\x3048" /* いいえ */ : L"No");
        s_pSaveConfirmBtnOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_SF_YES), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SF_YES), GWLP_WNDPROC, (LONG_PTR)SaveConfirmBtnCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_SF_NO),  GWLP_WNDPROC, (LONG_PTR)SaveConfirmBtnCtrlProc);
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_SF_YES));
        return FALSE;

    case WM_DRAWITEM:
        CeBmpFontDrawOwnerButton((const DRAWITEMSTRUCT *)lParam);
        return TRUE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc;
        RECT rc;
        wchar_t line1[CE_MSGBOX_MAX_LINE], line2[CE_MSGBOX_MAX_LINE];
        COLORREF fg;
        int rectW, rectH;

        hdc = BeginPaint(hDlg, &ps);
        GetWindowRect(GetDlgItem(hDlg, IDC_SF_TEXT), &rc);
        MapWindowPoints(NULL, hDlg, (POINT *)&rc, 2);
        rectW = rc.right - rc.left;
        rectH = rc.bottom - rc.top;

        WrapMsgBoxText(s_msgBoxText, rectW, line1, line2, CE_MSGBOX_MAX_LINE);

        fg = GetSysColor(COLOR_WINDOWTEXT);
        SetBkMode(hdc, TRANSPARENT);

        if (line2[0] == L'\0')
        {
            int x = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
            int y = rc.top + (rectH - CE_BMPFONT_HEIGHT) / 2;
            CeBmpFontDrawTextW(hdc, x, y, line1, fg);
        }
        else
        {
            int lineH = CE_BMPFONT_HEIGHT + 2;
            int y0 = rc.top + (rectH - (CE_BMPFONT_HEIGHT + lineH)) / 2;
            int x1 = rc.left + (rectW - CeBmpFontGetTextWidth(line1)) / 2;
            int x2 = rc.left + (rectW - CeBmpFontGetTextWidth(line2)) / 2;
            CeBmpFontDrawTextW(hdc, x1, y0, line1, fg);
            CeBmpFontDrawTextW(hdc, x2, y0 + lineH, line2, fg);
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SF_YES:
            if (s_sfPhase == 0)
            {
                int ok = CeSaveState();
                wcsncpy(s_msgBoxText, CeLangIsJapanese()
                        ? (ok ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3057\x305f\x3002"                     /* セーブしました。 */
                              : L"\x30bb\x30fc\x30d6\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002")  /* セーブに失敗しました。 */
                        : (ok ? L"State saved." : L"Save failed."),
                        CE_MSGBOX_MAX_TEXT - 1);
                s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';
                s_sfPhase = 1;
                ShowWindow(GetDlgItem(hDlg, IDC_SF_NO), SW_HIDE);
                SetDlgItemTextW(hDlg, IDC_SF_YES, L"OK");
                {   /* recentre the lone OK button */
                    RECT rc, rb;
                    GetClientRect(hDlg, &rc);
                    GetWindowRect(GetDlgItem(hDlg, IDC_SF_YES), &rb);
                    MapWindowPoints(NULL, hDlg, (POINT *)&rb, 2);
                    SetWindowPos(GetDlgItem(hDlg, IDC_SF_YES), NULL,
                                 (rc.right - (rb.right - rb.left)) / 2, rb.top,
                                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                }
                InvalidateRect(hDlg, NULL, TRUE);
                SetFocus(GetDlgItem(hDlg, IDC_SF_YES));
            }
            else
            {
                EndDialog(hDlg, 1);
            }
            return TRUE;
        case IDC_SF_NO:
        case IDCANCEL:
            EndDialog(hDlg, s_sfPhase ? 1 : 0);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

/* Save State: asks, runs the save, acknowledges the result - all in one
 * self-drawn dialog (never a nested second modal). */
static void CeConfirmAndSaveState(HWND owner)
{
    wcsncpy(s_msgBoxText,
            CeLangIsJapanese() ? L"\x30bb\x30fc\x30d6\x3057\x307e\x3059\x304b\xff1f" /* セーブしますか？ */
                               : L"Save State?",
            CE_MSGBOX_MAX_TEXT - 1);
    s_msgBoxText[CE_MSGBOX_MAX_TEXT - 1] = L'\0';

    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE),
               MAKEINTRESOURCEW(IDD_SAVECONFIRM), owner, SaveConfirmDlgProc);
}

/* ------------------------------------------------------------------ */
/* Save state (single slot per ROM: "<romPath>.state")                */
/* ------------------------------------------------------------------ */

/* Returns 1 on success, 0 on failure. Shows no UI itself - the caller
 * (SaveConfirmDlgProc) folds the result into its own single dialog so no
 * second modal is ever nested. */
static int CeSaveState(void)
{
    size_t size;
    void *buffer;
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;

    size = retro_serialize_size();
    if (size == 0)
    {
        CeLog("CeSaveState: retro_serialize_size returned 0");
        return 0;
    }

    buffer = malloc(size);
    if (!buffer)
    {
        CeLog("CeSaveState: malloc(%lu) failed", (unsigned long)size);
        return 0;
    }

    if (!retro_serialize(buffer, size))
    {
        free(buffer);
        CeLog("CeSaveState: retro_serialize failed");
        return 0;
    }

    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"wb");
    if (!f)
    {
        free(buffer);
        CeLog("CeSaveState: failed to open state file for write");
        return 0;
    }

    fwrite(buffer, 1, size, f);
    fclose(f);
    free(buffer);
    CeLog("CeSaveState: saved %lu bytes", (unsigned long)size);
    return 1;
}

static int CeLoadState(HWND owner)
{
    wchar_t statePath[MAX_PATH + 8];
    FILE *f;
    long size;
    void *buffer;

    _snwprintf(statePath, MAX_PATH + 8, L"%s.state", g_romPath);
    f = _wfopen(statePath, L"rb");
    if (!f)
    {
        CeLog("CeLoadState: no state file found");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x304c\x898b\x3064\x304b\x308a\x307e\x305b\x3093\x3002" /* セーブデータが見つかりません。 */
                                                : L"No save state found.");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0)
    {
        fclose(f);
        CeLog("CeLoadState: empty/unreadable state file");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }

    buffer = malloc((size_t)size);
    if (!buffer)
    {
        fclose(f);
        CeLog("CeLoadState: malloc(%ld) failed", size);
        MessageBoxW(owner, L"Load failed.", kAppTitle, MB_OK);
        return 0;
    }

    if (fread(buffer, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        free(buffer);
        CeLog("CeLoadState: short read");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* ロードに失敗しました。 */
                                                : L"Load failed.");
        return 0;
    }
    fclose(f);

    if (!retro_unserialize(buffer, (size_t)size))
    {
        free(buffer);
        CeLog("CeLoadState: retro_unserialize failed");
        CeShowMsgBox(owner, CeLangIsJapanese() ? L"\x30ed\x30fc\x30c9\x306b\x5931\x6557\x3057\x307e\x3057\x305f(\x30bb\x30fc\x30d6\x30c7\x30fc\x30bf\x306e\x5f62\x5f0f\x304c\x7570\x306a\x308b\x53ef\x80fd\x6027\x304c\x3042\x308a\x307e\x3059)\x3002" /* ロードに失敗しました(セーブデータの形式が異なる可能性があります)。 */
                                                : L"Load failed (incompatible save?).");
        return 0;
    }

    free(buffer);
    CeLog("CeLoadState: loaded %ld bytes", size);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Menu (touch-to-reveal - hidden during gameplay, shown at startup     */
/* before any ROM is loaded and whenever the screen is tapped mid-game) */
/*                                                                      */
/* Implemented as a modal DialogBoxW (CE/ce_res.rc's IDD_MAINMENU) with */
/* plain PUSHBUTTON controls, not a real HMENU - see ce_resource.h for  */
/* why (this coredll doesn't export SetMenu).                          */
/* ------------------------------------------------------------------ */

static HINSTANCE g_hInstance = NULL;

/* ------------------------------------------------------------------ */
/* Screenshot                                                          */
/* ------------------------------------------------------------------ */

static void PutLE16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void PutLE32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Saves the paused game image as a 16-bit RGB565 BMP (BI_BITFIELDS -
 * the native pixel format of both sources below, so no conversion and no
 * compression: one fwrite per row). Uses the on-screen image at the
 * current Scale (x1/x1.5/Wide/Full, from ce_display.c's DIB) so the file
 * matches what the user sees; falls back to the core's unscaled frame
 * if the display has nothing. Written to "<exe-dir>\Screenshots\<ROM name>_NNN.bmp",
 * creating the folder on first use and taking the first unused number.
 * The header is built byte by byte rather than from BITMAPFILEHEADER so
 * struct packing can't shift it. Returns 1 on success. */
static int CeSaveScreenshot(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t romName[MAX_PATH];
    wchar_t path[MAX_PATH + 16];
    wchar_t *p;
    const wchar_t *base;
    unsigned n, y, rowBytes, padBytes;
    unsigned long imageBytes;
    unsigned char hdr[66];
    static const unsigned char pad[4] = { 0, 0, 0, 0 };
    const void *img;
    unsigned imgW, imgH, imgPitch;
    FILE *f;

    /* g_lastFrame doubles as "something was drawn since this ROM
     * loaded" - the display's DIB isn't cleared on ROM switch, so
     * without it a new game could save the previous game's image. */
    if (!g_lastFrame || g_lastFrameW == 0 || g_lastFrameH == 0)
    {
        CeLog("CeSaveScreenshot: no rendered frame yet");
        return 0;
    }
    if (!CeDisplayGetLastImage(&img, &imgW, &imgH, &imgPitch))
    {
        img = g_lastFrame;
        imgW = g_lastFrameW;
        imgH = g_lastFrameH;
        imgPitch = (unsigned)g_lastFramePitch;
    }

    if (!GetModuleFileNameW(NULL, dir, MAX_PATH))
        return 0;
    p = wcsrchr(dir, L'\\');
    if (!p)
        return 0;
    p[1] = L'\0';
    if (wcslen(dir) + 12 >= MAX_PATH)
        return 0;
    wcscat(dir, L"Screenshots");
    CreateDirectoryW(dir, NULL); /* already existing is fine */
    if (GetFileAttributesW(dir) == 0xFFFFFFFF)
    {
        CeLog("CeSaveScreenshot: can't create Screenshots folder");
        return 0;
    }

    base = wcsrchr(g_romPath, L'\\');
    wcsncpy(romName, base ? base + 1 : g_romPath, MAX_PATH - 1);
    romName[MAX_PATH - 1] = L'\0';
    p = wcsrchr(romName, L'.');
    if (p)
        *p = L'\0';

    for (n = 1; n <= 999; n++)
    {
        _snwprintf(path, MAX_PATH + 16, L"%s\\%s_%03u.bmp", dir, romName, n);
        path[MAX_PATH + 15] = L'\0';
        if (GetFileAttributesW(path) == 0xFFFFFFFF)
            break;
    }
    if (n > 999)
    {
        CeLog("CeSaveScreenshot: all 999 numbers used");
        return 0;
    }

    rowBytes = imgW * 2;
    padBytes = (4 - (rowBytes & 3)) & 3;
    imageBytes = (unsigned long)(rowBytes + padBytes) * imgH;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    PutLE32(hdr + 2, sizeof(hdr) + imageBytes);  /* file size */
    PutLE32(hdr + 10, sizeof(hdr));              /* pixel data offset */
    PutLE32(hdr + 14, 40);                       /* BITMAPINFOHEADER size */
    PutLE32(hdr + 18, imgW);
    PutLE32(hdr + 22, imgH);             /* positive = bottom-up */
    PutLE16(hdr + 26, 1);                        /* planes */
    PutLE16(hdr + 28, 16);                       /* bits per pixel */
    PutLE32(hdr + 30, 3);                        /* BI_BITFIELDS */
    PutLE32(hdr + 34, imageBytes);
    PutLE32(hdr + 38, 2835);                     /* 72 dpi */
    PutLE32(hdr + 42, 2835);
    PutLE32(hdr + 54, 0xF800);                   /* R mask */
    PutLE32(hdr + 58, 0x07E0);                   /* G mask */
    PutLE32(hdr + 62, 0x001F);                   /* B mask */

    f = _wfopen(path, L"wb");
    if (!f)
    {
        CeLog("CeSaveScreenshot: can't open output file");
        return 0;
    }
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = imgH; y-- > 0; )
    {
        fwrite((const unsigned char *)img + (size_t)y * imgPitch, 1, rowBytes, f);
        if (padBytes)
            fwrite(pad, 1, padBytes, f);
    }
    if (ferror(f))
    {
        fclose(f);
        DeleteFileW(path);
        CeLog("CeSaveScreenshot: write failed");
        return 0;
    }
    fclose(f);

    CeLog("CeSaveScreenshot: saved %ux%u as #%03u", imgW, imgH, n);
    return 1;
}

static void CeScreenshotAndReport(HWND owner)
{
    if (CeSaveScreenshot())
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x3092\x4fdd\x5b58\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットを保存しました。 */
            : L"Screenshot saved.");
    else
        CeShowMsgBox(owner, CeLangIsJapanese()
            ? L"\x30b9\x30af\x30ea\x30fc\x30f3\x30b7\x30e7\x30c3\x30c8\x306e\x4fdd\x5b58\x306b\x5931\x6557\x3057\x307e\x3057\x305f\x3002" /* スクリーンショットの保存に失敗しました。 */
            : L"Screenshot failed.");
}


/* Swaps every IDD_MAINMENU button caption between English (the .rc
 * template's own text) and Japanese, gated by CeLangIsJapanese() - see
 * ce_lang.h. Called once from WM_INITDIALOG, and again right after the
 * IDC_MM_VIDEO case returns (Video Config is where the toggle itself
 * lives), so flipping it and returning to this still-open dialog updates
 * it immediately instead of only the next time the menu happens to be
 * recreated. The title bar is deliberately never touched - it's
 * non-client area the OS paints with its own caption font (confirmed to
 * render as tofu boxes on this device by an earlier Brain port).
 *
 * The PUSHBUTTONs above are BS_OWNERDRAW (ce_res.rc) and repaint
 * themselves from the text just set (WM_DRAWITEM below, via
 * CeBmpFontDrawOwnerButton() - see ce_bmpfont.c). IDC_MM_HINT is a plain
 * LTEXT - STATIC controls have no ownerdraw style - so it's hidden here
 * instead and repainted by this dialog's own WM_PAINT via
 * CeBmpFontPaintLabel(), which still reads the text set above with
 * GetWindowTextW() even though the control itself is hidden.
 *
 * Ported from the sister gnuboy CE project: CeBmpFontPaintLabel() only
 * SetPixel()s the "on" bits of each glyph with a transparent background
 * - it never clears the label's rect first, so a stale glyph from the
 * previous language can survive underneath the new one unless the whole
 * dialog gets a full erase+redraw first. Forcing that unconditionally
 * here (every time this text can change) avoids depending on whatever
 * incidental repaint a sub-dialog closing over part of this one happens
 * to trigger. */
static void ApplyMainMenuLanguage(HWND hDlg)
{
    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"ROM\x3092\x958b\x304f...");                             /* ROMを開く... */
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"\x30b9\x30c6\x30fc\x30c8\x30bb\x30fc\x30d6");            /* ステートセーブ */
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"\x30b9\x30c6\x30fc\x30c8\x30ed\x30fc\x30c9");            /* ステートロード */
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"\x30dc\x30bf\x30f3\x8a2d\x5b9a");                        /* ボタン設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"\x30b5\x30a6\x30f3\x30c9\x8a2d\x5b9a");                  /* サウンド設定 */
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"\x753b\x9762\x8a2d\x5b9a");                              /* 画面設定 */
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"\x753b\x9762\x4fdd\x5b58\x3059\x308b");                  /* 画面保存する */
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"\x7d42\x4e86");                                          /* 終了 */
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"\x623b\x308b\x30ad\x30fc\x3067\x30b2\x30fc\x30e0\x518d\x958b"); /* 戻るキーでゲーム再開 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_MM_OPEN,      L"Open ROM...");
        SetDlgItemTextW(hDlg, IDC_MM_SAVESTATE, L"Save State");
        SetDlgItemTextW(hDlg, IDC_MM_LOADSTATE, L"Load State");
        SetDlgItemTextW(hDlg, IDC_MM_INPUT,     L"Input Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SOUND,     L"Sound Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_VIDEO,     L"Video Cfg");
        SetDlgItemTextW(hDlg, IDC_MM_SCREENSHOT, L"Screenshot");
        SetDlgItemTextW(hDlg, IDC_MM_EXIT,      L"Exit");
        SetDlgItemTextW(hDlg, IDC_MM_HINT,      L"Press Back to resume the game.");
    }

    ShowWindow(GetDlgItem(hDlg, IDC_MM_HINT), SW_HIDE);
    InvalidateRect(hDlg, NULL, TRUE);
}

/* Reading order matching ce_res.rc's IDD_MAINMENU layout (top-to-bottom,
 * left-to-right within a row): Open (full width), Save/Load State
 * (2-up), Video/Sound/Input (3-up), Screenshot/Exit (2-up). Used both for
 * WM_INITDIALOG's subclassing loop and MainMenuNeighbor()'s wraparound
 * arrow-key cycling below. */
static const int kMainMenuButtonIds[] = {
    IDC_MM_OPEN, IDC_MM_SAVESTATE, IDC_MM_LOADSTATE,
    IDC_MM_VIDEO, IDC_MM_SOUND, IDC_MM_INPUT, IDC_MM_SCREENSHOT, IDC_MM_EXIT,
};
#define CE_MAINMENU_BUTTON_COUNT (sizeof(kMainMenuButtonIds) / sizeof(kMainMenuButtonIds[0]))

/* Skips disabled buttons (Save/Load State while g_romLoaded is still 0) -
 * EnableWindow() alone only blocks activation, not this dialog's own
 * custom arrow-key cycling below. Bounded to CE_MAINMENU_BUTTON_COUNT
 * steps so it can't spin forever if every button were ever disabled at
 * once (never happens in practice - Open/Exit are always enabled).
 * Ported from the sister gnuboy CE project's own MainMenuNeighbor(). */
static int MainMenuNeighbor(HWND hDlg, int id, int delta)
{
    int idx, step;
    for (idx = 0; idx < (int)CE_MAINMENU_BUTTON_COUNT; idx++)
        if (kMainMenuButtonIds[idx] == id)
            break;
    if (idx >= (int)CE_MAINMENU_BUTTON_COUNT)
        return id;

    for (step = 1; step <= (int)CE_MAINMENU_BUTTON_COUNT; step++)
    {
        int nextIdx = ((idx + delta * step) % (int)CE_MAINMENU_BUTTON_COUNT + (int)CE_MAINMENU_BUTTON_COUNT) % (int)CE_MAINMENU_BUTTON_COUNT;
        int nextId = kMainMenuButtonIds[nextIdx];
        if (IsWindowEnabled(GetDlgItem(hDlg, nextId)))
            return nextId;
    }
    return id;
}

/* Colorful rounded-button skin, ported from the sister PopSG port's main
 * menu (user request): pastel fills with a darker same-hue outline drawn
 * by CeBmpFontDrawOwnerButtonTheme() (ce_bmpfont.c), a cream client
 * background (WM_ERASEBKGND below) and the mascot bitmap in the footer.
 * Only this dialog uses it - every other dialog keeps the plain gray
 * CeBmpFontDrawOwnerButton() look. Colors match PopSG's table; the
 * Screenshot button gets PopSNES's lavender pair. Ported from PopSNES. */
#define CE_MENU_BG_CREAM   RGB(0xF0, 0xE1, 0xBC)
#define CE_MENU_TEXT_DARK  RGB(0x2A, 0x2C, 0x30)

typedef struct { int id; COLORREF bg, border; CeMenuIcon icon; int stacked; } CeMenuButtonTheme;

static const CeMenuButtonTheme kMainMenuTheme[] = {
    { IDC_MM_OPEN,       RGB(0x6F, 0xA8, 0xDC), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_OPEN,       0 }, /* blue         */
    { IDC_MM_SAVESTATE,  RGB(0xF5, 0xEC, 0x9E), RGB(0x6E, 0x66, 0x12), CE_MENU_ICON_SAVE,       0 }, /* pastel lemon */
    { IDC_MM_LOADSTATE,  RGB(0xBF, 0xE3, 0xD0), RGB(0x1D, 0x5A, 0x3C), CE_MENU_ICON_LOAD,       0 }, /* mint         */
    { IDC_MM_VIDEO,      RGB(0xC9, 0xE4, 0xB0), RGB(0x3C, 0x5A, 0x1B), CE_MENU_ICON_VIDEO,      1 }, /* green        */
    { IDC_MM_SOUND,      RGB(0xB9, 0xD7, 0xEE), RGB(0x1D, 0x4A, 0x70), CE_MENU_ICON_SOUND,      1 }, /* blue         */
    { IDC_MM_INPUT,      RGB(0xF2, 0xB8, 0xC6), RGB(0x7A, 0x2E, 0x4C), CE_MENU_ICON_INPUT,      1 }, /* pink         */
    { IDC_MM_SCREENSHOT, RGB(0xD9, 0xCC, 0xF0), RGB(0x4E, 0x34, 0x80), CE_MENU_ICON_SCREENSHOT, 0 }, /* lavender     */
    { IDC_MM_EXIT,       RGB(0xF0, 0xA9, 0xA0), RGB(0x7A, 0x23, 0x18), CE_MENU_ICON_EXIT,       0 }, /* coral        */
};

/* CE/icon/popnes_mascot.bmp (IDB_MAINMENU, ce_res.rc). Loaded in
 * WM_INITDIALOG, blitted by WM_PAINT into the blank strip right of the
 * IDC_MM_HINT text, freed in WM_DESTROY. */
static HBITMAP s_hMainMenuBmp = NULL;

static WNDPROC s_pMainMenuOrigProc = NULL;

/* PUSHBUTTON's built-in WM_KEYDOWN -> BN_CLICKED conversion for
 * VK_RETURN/VK_SPACE (and IsDialogMessage()'s own Up/Down/Left/Right
 * focus-cycling) stops working once these buttons become BS_OWNERDRAW
 * (ce_res.rc) - real-hardware-confirmed by the sister gnuboy CE project:
 * touch/stylus taps kept working (WM_LBUTTONUP is unaffected) but the
 * physical decide key did nothing on a focused button, and a first
 * attempt at fixing this via plain WM_KEYDOWN handling (no WM_GETDLGCODE
 * override) still didn't work - IsDialogMessage() apparently never
 * delivers WM_KEYDOWN to an owner-draw button at all without this
 * subclass explicitly claiming DLGC_WANTARROWS | DLGC_WANTALLKEYS, the
 * same claim SoundCtrlProc/VideoCtrlProc/InputBtnCtrlProc already make
 * for their own "-/value/+" buttons - so arrow-key navigation is
 * reimplemented here too via MainMenuNeighbor() above instead of leaning
 * on the standard dialog navigation that claim steals control of.
 * Claiming DLGC_WANTALLKEYS also steals the physical Back key (Escape)
 * away from IsDialogMessage()'s normal Cancel-key handling, so it's
 * forwarded to IDCANCEL by hand below, same as those three dialogs
 * already do for their own OK button. */
static LRESULT CALLBACK MainMenuBtnCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);
    HWND hDlg = GetParent(hWnd);

    if (message == WM_GETDLGCODE)
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
        case VK_LEFT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, -1)));
            return 0;

        case VK_DOWN:
        case VK_RIGHT:
            SetFocus(GetDlgItem(hDlg, MainMenuNeighbor(hDlg, id, 1)));
            return 0;

        case VK_RETURN:
        case VK_SPACE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)hWnd);
            return 0;

        case VK_ESCAPE:
            SendMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pMainMenuOrigProc, hWnd, message, wParam, lParam);
}

static INT_PTR CALLBACK MainMenuDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        unsigned i;
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SAVESTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_LOADSTATE), g_romLoaded);
        EnableWindow(GetDlgItem(hDlg, IDC_MM_SCREENSHOT), g_romLoaded);
        ApplyMainMenuLanguage(hDlg);

        s_pMainMenuOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDC_MM_OPEN), GWLP_WNDPROC);
        for (i = 0; i < CE_MAINMENU_BUTTON_COUNT; i++)
            SetWindowLongPtrW(GetDlgItem(hDlg, kMainMenuButtonIds[i]), GWLP_WNDPROC, (LONG_PTR)MainMenuBtnCtrlProc);

        if (!s_hMainMenuBmp)
            s_hMainMenuBmp = LoadBitmapW(g_hInstance, MAKEINTRESOURCEW(IDB_MAINMENU));
        return TRUE;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        unsigned i;
        for (i = 0; i < sizeof(kMainMenuTheme) / sizeof(kMainMenuTheme[0]); i++)
        {
            if (kMainMenuTheme[i].id == (int)dis->CtlID)
            {
                CeBmpFontDrawOwnerButtonTheme(dis, kMainMenuTheme[i].bg, kMainMenuTheme[i].border, CE_MENU_TEXT_DARK,
                                              kMainMenuTheme[i].icon, kMainMenuTheme[i].stacked, CE_MENU_BG_CREAM);
                return TRUE;
            }
        }
        CeBmpFontDrawOwnerButton(dis); /* fallback, no control here should hit it */
        return TRUE;
    }

    /* Cream client background, scoped to just this dialog. The brush is
     * created once and kept for the process lifetime (this fires on
     * every erase, e.g. each time a sub-dialog closes over this one). */
    case WM_ERASEBKGND:
    {
        static HBRUSH s_hCreamBrush = NULL;
        RECT rc;
        if (!s_hCreamBrush)
            s_hCreamBrush = CreateSolidBrush(CE_MENU_BG_CREAM);
        GetClientRect(hDlg, &rc);
        FillRect((HDC)wParam, &rc, s_hCreamBrush);
        return TRUE;
    }

    case WM_DESTROY:
        if (s_hMainMenuBmp)
        {
            DeleteObject(s_hMainMenuBmp);
            s_hMainMenuBmp = NULL;
        }
        return FALSE;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        CeBmpFontPaintLabel(hdc, hDlg, IDC_MM_HINT);

        /* Mascot in the blank strip right of the hint text: starts just
         * past the text, runs to the dialog's right/bottom edge, source
         * aspect ratio preserved, bottom-right aligned (same as PopSG). */
        if (s_hMainMenuBmp)
        {
            HWND hHint = GetDlgItem(hDlg, IDC_MM_HINT);
            RECT rcHint, rcClient;
            wchar_t hintText[128];
            BITMAP bm;

            hintText[0] = 0;
            GetWindowTextW(hHint, hintText, 128);
            GetWindowRect(hHint, &rcHint);
            MapWindowPoints(NULL, hDlg, (POINT *)&rcHint, 2);
            GetClientRect(hDlg, &rcClient);

            if (GetObject(s_hMainMenuBmp, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0)
            {
                long boxL = rcHint.left + CeBmpFontGetTextWidth(hintText) + 8;
                long boxR = rcClient.right - 4;
                long boxT = rcHint.top;
                long boxB = rcClient.bottom - 2;
                long boxW = boxR - boxL;
                long boxH = boxB - boxT;

                if (boxW > 8 && boxH > 8)
                {
                    long drawW = boxW;
                    long drawH = drawW * bm.bmHeight / bm.bmWidth;
                    HDC memDC;
                    HGDIOBJ oldBmp;

                    if (drawH > boxH)
                    {
                        drawH = boxH;
                        drawW = drawH * bm.bmWidth / bm.bmHeight;
                    }

                    /* No SetStretchBltMode() - this coredll doesn't export
                     * it; CE's default (COLORONCOLOR) is fine here. */
                    memDC = CreateCompatibleDC(hdc);
                    oldBmp = SelectObject(memDC, s_hMainMenuBmp);
                    StretchBlt(hdc, (int)(boxR - drawW), (int)(boxB - drawH),
                               (int)drawW, (int)drawH,
                               memDC, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(memDC, oldBmp);
                    DeleteDC(memDC);
                }
            }
        }

        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_MM_OPEN:
            /* Stays open on cancel/failure (LoadRomFlow already showed
             * a MessageBox explaining why); closes only on success, so
             * the caller knows to resume gameplay. */
            if (LoadRomFlow(hDlg))
                EndDialog(hDlg, IDC_MM_OPEN);
            return TRUE;

        case IDC_MM_EXIT:
            CeShutdown(0); /* never returns */
            return TRUE;

        case IDC_MM_SAVESTATE:
            /* Confirm (IDD_SAVECONFIRM), run the save, and acknowledge -
             * all in one self-drawn dialog; いいえ/No or Back stays on
             * the menu. */
            if (g_romLoaded)
                CeConfirmAndSaveState(hDlg);
            return TRUE; /* stays open either way, like Input/Sound Config */

        case IDC_MM_LOADSTATE:
            /* Closes and resumes on success (like Resume Game) so the
             * user immediately sees the loaded state; stays open on
             * failure (CeLoadState already showed why). */
            if (g_romLoaded && CeLoadState(hDlg))
                EndDialog(hDlg, IDC_MM_LOADSTATE);
            return TRUE;

        case IDC_MM_SCREENSHOT:
            if (g_romLoaded)
                CeScreenshotAndReport(hDlg);
            return TRUE; /* stays open, like Save State */

        case IDC_MM_INPUT:
            CeShowInputConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_SOUND:
            CeShowSoundConfigDialog(hDlg);
            return TRUE;

        case IDC_MM_VIDEO:
            CeShowVideoConfigDialog(hDlg);
            /* Video Config is where the Japanese/English toggle lives -
             * re-apply here so switching it and returning to this
             * still-open menu updates it immediately. */
            ApplyMainMenuLanguage(hDlg);
            return TRUE;

        case IDCANCEL:
            /* Hardware Back / OS close gesture: same as Resume if a
             * game is already running (nothing to lose by dismissing),
             * otherwise ignored - there's nothing to go back to yet. */
            if (g_romLoaded)
                EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

/* Pauses (if a game is running), shows the menu dialog modally (blocks
 * until closed), then resumes if a game is loaded when it returns -
 * whether that's the game that was already running, or one just picked
 * via Open ROM. Also how the very first "no ROM loaded" screen is shown
 * from WinMain, where wasPlaying is simply false. */
static void ShowMainMenuDialog(HWND hwnd)
{
    int wasPlaying = g_romLoaded && !g_paused;

    if (wasPlaying)
    {
        g_paused = 1;
        CeAudioSetPaused(1);
        CeDisplaySuspend(); /* no-op under GDI - see ce_display.h */

        /* Autosave SRAM at every pause, not just at graceful shutdown/
         * ROM switch - a real power-off doesn't run CeShutdown() at all,
         * so relying only on those two checkpoints misses that case
         * entirely (lesson from the sister snes9x2002 CE port's round 9).
         * Opening the touch-to-reveal menu is a frequent, cheap, natural
         * checkpoint to also save at. */
        CeSaveSram();
    }

    /* Explicitly forces a repaint only for the very first "No ROM
     * loaded" screen - by the time this runs, WinMain's own
     * ShowWindow()/UpdateWindow() calls have already consumed whatever
     * pending paint region a freshly-created window starts with, so
     * without this the black background would never actually get
     * painted before this first dialog covers it. Every other call
     * doesn't need it: closing this dialog naturally invalidates
     * whatever screen area it covered, and WM_PAINT below now redraws
     * that properly under GDI (black fill, then the current frame on
     * top via CeDisplayForceRepaint() - see that handler; the old GAPI
     * backend never needed this because it bypassed window painting
     * entirely while active). */
    if (!g_romLoaded)
        InvalidateRect(hwnd, NULL, TRUE);
    DialogBoxW(g_hInstance, MAKEINTRESOURCEW(IDD_MAINMENU), hwnd, MainMenuDlgProc);

    if (g_romLoaded)
    {
        g_paused = 0;
        CeAudioSetPaused(0);

        /* The physical decide key that dismissed this dialog (Load
         * State, most visibly, since it resumes immediately - but also
         * Open ROM and plain Resume/Cancel) may still be physically
         * held down at this exact moment - input polling was frozen
         * for the whole time the dialog was up, so there's no debounce
         * history to tell "still held from the dialog" apart from "a
         * brand-new press" once polling resumes. See
         * CeInputSuppressStartKey's own comment (ce_input.h) - same
         * root cause and fix as an earlier Brain port's
         * g_suppress_start_key, confirmed on real hardware there. */
        CeInputSuppressStartKey();

        if (!CeDisplayInit(hwnd))
            CeLog("ShowMainMenuDialog: CeDisplayInit failed - continuing without video output");
        CeDisplayResume(); /* no-op under GDI - see ce_display.h */

        /* Re-assert taskbar hiding every time gameplay is (re-)entered,
         * not just once at WinMain startup - the sister snes9x2002 CE
         * port found this necessary on this device (see that project's
         * ce_main.c comment): a custom DialogBoxW becoming the
         * foreground window (this menu) may cause the shell to restore
         * the taskbar, so it needs re-hiding every time control returns
         * from the menu dialog to gameplay. */
        CeHideShellChrome(hwnd);
    }
}

/* ------------------------------------------------------------------ */
/* Window / shutdown                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        g_running = 0;
        PostQuitMessage(0);
        return 0;

    case WM_LBUTTONDOWN:
        /* Touch-to-reveal, same interaction both prior CE ports use.
         * ShowMainMenuDialog() is modal, so this can't re-enter while
         * already showing (input goes to the dialog, not this window). */
        ShowMainMenuDialog(hwnd);
        return 0;

    case WM_PAINT:
    {
        /* GDI video output (ce_display.c) draws straight into this
         * window's own client area, so - unlike the old GAPI backend,
         * which bypassed window painting entirely while it owned the
         * display - a WM_PAINT here (e.g. a modal dialog covering part
         * of the game window, then closing) needs the last rendered
         * frame redrawn, or the exposed region stays black until the
         * next real emulated frame (which won't happen at all while
         * retro_run() is paused for that same dialog). Always erase the
         * invalidated region to black first (covers both the very first
         * "No ROM loaded" screen and any letterbox border around the
         * game rect), then, if a ROM is loaded, redraw the current frame
         * on top via CeDisplayForceRepaint() - see that function's own
         * comment for why it re-blits from the off-screen DIB rather
         * than the core's own (possibly stale by now) frame pointer. */
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        if (g_romLoaded)
            CeDisplayForceRepaint();
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void CeShutdown(int exitCode)
{
    /* Lesson from both prior CE ports on this device: a plain
     * `return` from WinMain lets the
     * normal exit path run, which on this device/toolchain combination
     * can itself crash or hang threads mid-teardown. Always terminate
     * via ExitProcess, called directly from here (not via WM_CLOSE/
     * WM_DESTROY/PostQuitMessage, which an earlier Brain port traced a real
     * hang to on this device). CeAudioStop() joins the audio thread
     * cleanly before we ever get here, so ExitProcess() isn't tearing
     * down a thread still mid-waveOutWrite. */
    CeAudioStop();
    if (g_romLoaded)
        CeSaveSram();
    retro_unload_game();
    retro_deinit();

    CeDisplayShutdown();
    CeShowShellChrome(g_hwnd);

    if (g_mutex)
        CloseHandle(g_mutex);

    /* If this shutdown is a hand-off to a freshly-created replacement
     * process (RestartProcess() - a mid-session ROM switch), only resume
     * its main thread now that g_mutex is already closed, above. This
     * app enforces a single-instance mutex at WinMain startup; resuming
     * any earlier risks the new process's own CreateMutexW seeing
     * ERROR_ALREADY_EXISTS while this process is still tearing down,
     * which would make it think another instance is already running and
     * exit immediately instead of loading the ROM. Same ordering fix the
     * sister gpSP CE port settled on for an analogous
     * resource race (that one over dynarec memory, this one over the
     * mutex) - see RestartProcess()'s own comment below. */
    if (s_restartResumeThread)
    {
        ResumeThread(s_restartResumeThread);
        CloseHandle(s_restartResumeThread);
        CloseHandle(s_restartResumeProcess);
        s_restartResumeThread = NULL;
        s_restartResumeProcess = NULL;
    }

    ExitProcess((UINT)exitCode);
}

/* "HHTaskBar" is the standard window class of the Windows CE Explorer
 * taskbar on this device - FindWindow+ShowWindow(HIDE) on it is what
 * both prior CE ports on this hardware settled on after aygshell.dll's
 * SHFullScreen proved unreliable/fragile. Needs nothing beyond
 * coredll.dll. */
static HWND CeFindTaskBarWindow(void)
{
    return FindWindowW(L"HHTaskBar", NULL);
}

static void CeHideShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
    {
        ShowWindow(hTaskBar, SW_HIDE);
        CeLog("CeHideShellChrome: hid HHTaskBar window directly");
    }
    else
    {
        CeLog("CeHideShellChrome: HHTaskBar window not found");
    }
}

/* Restores shell chrome on exit - this is a shared-shell CE device, not
 * a single-purpose game handheld, so leaving the taskbar hidden after
 * this app closes would affect the user's other apps until reboot. */
static void CeShowShellChrome(HWND hwnd)
{
    HWND hTaskBar = CeFindTaskBarWindow();
    (void)hwnd;
    if (hTaskBar)
        ShowWindow(hTaskBar, SW_SHOW);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSW wc;
    MSG msg;

    (void)hPrevInstance;
    (void)nCmdShow;

    CeLog("WinMain start, build " __DATE__ " " __TIME__);

    g_hInstance = hInstance;

    g_mutex = CreateMutexW(NULL, TRUE, kMutexName);
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(kWndClassName, NULL);
        if (existing)
        {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CeLog("WinMain: another instance is already running, exiting");
        ExitProcess(0);
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWndClassName;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN));

    if (!RegisterClassW(&wc))
    {
        CeLog("WinMain: RegisterClassW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"RegisterClassW failed", kAppTitle, MB_OK);
        CeShutdown(1);
    }

    /* WS_POPUP (not just WS_VISIBLE): both prior CE ports on this
     * hardware confirmed a plain overlapped window is still managed by
     * the shell as a regular window and doesn't reliably reclaim the
     * taskbar's screen space once CeHideShellChrome() hides it. */
    g_hwnd = CreateWindowW(kWndClassName, CE_APP_TITLE L" - No ROM loaded", WS_VISIBLE | WS_POPUP,
                            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                            NULL, NULL, hInstance, NULL);
    if (!g_hwnd)
    {
        CeLog("WinMain: CreateWindowW failed, error=%lu", (unsigned long)GetLastError());
        MessageBoxW(NULL, L"CreateWindowW failed", kAppTitle, MB_OK);
        CeShutdown(1);
    }

    CeHideShellChrome(g_hwnd);
    /* Re-assert visibility/layout after hiding the taskbar - without
     * this the window doesn't re-layout to cover the space the taskbar
     * just vacated (confirmed by both prior CE ports). */
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    CeConfigLoad(); /* before any *_Init() below - they read their settings out of this shared table */
    CeLogSetEnabled(CeConfigGetInt("VideoDebugLog", 0)); /* Video Config's "Enable Debug Logging" checkbox - default off (see ce_log.h) */
    CeLangInit(); /* loads the persisted UILanguageJapanese flag; see ce_lang.h */
    CeInputInit();
    CeAudioInit();
    CeVideoInit();
    CeFileOpenInit();

    retro_set_environment(ce_environment);
    retro_set_video_refresh(ce_video_refresh);
    retro_set_audio_sample(ce_audio_sample_noop);
    retro_set_audio_sample_batch(ce_audio_sample_batch);
    retro_set_input_poll(ce_input_poll);
    retro_set_input_state(ce_input_state);

    retro_init();
    CeLog("WinMain: retro_init done");

    /* Eagerly open waveOut now, before the user has even picked a ROM,
     * instead of waiting for the first LoadRomFlow() to call
     * CeAudioStart(). QuickNES's core sample rate is always exactly
     * 32000Hz regardless of which ROM loads (CeVideoEnvGetVariable()
     * always answers "32000" for quicknes_audio_samplerate), so this
     * doesn't need a loaded game to know the rate.
     * Round-14 hardware logs showed a sustained ~10s audio ring-overrun
     * storm (thousands of dropped frames) immediately after every fresh
     * waveOutOpen(), which then fully cleared up and stayed clean for
     * every later ROM switch in the same session (those reuse the
     * already-open device - CeAudioStart() is a no-op when the core
     * rate hasn't changed). That pattern - broken only right after
     * opening, fine forever after - matches this device's audio
     * driver/DAC needing a few seconds to physically warm up post-open,
     * not an emulation performance problem. Opening here instead moves
     * that warm-up window onto the "no ROM loaded" menu screen, which
     * the user is already looking at before any game audio should be
     * playing, rather than onto their first several seconds of actual
     * gameplay. CeAudioStart() re-opening at the same rate later is
     * already a safe no-op, so this is not expected to change anything
     * for ROMs loaded after the first. Needs real-hardware confirmation
     * either way. */
    CeAudioStart(32000.0);

    if (lpCmdLine && lpCmdLine[0] != L'\0')
    {
        /* Relaunched by RestartProcess() for a mid-session ROM switch -
         * lpCmdLine is the whole ROM path verbatim (see that function's
         * comment), not a normal argv. Load it directly instead of
         * showing the "No ROM loaded" menu first, mirroring what
         * ShowMainMenuDialog() itself does once a load succeeds
         * (CeDisplayInit + re-hide shell chrome). */
        CeLog("WinMain: loading ROM from command line (process-restart hand-off)");
        if (LoadRomPath(g_hwnd, lpCmdLine))
        {
            /* Same reasoning as ShowMainMenuDialog's own call to this -
             * the decide key that dismissed the old process's ROM
             * picker (PickRom(), just before RestartProcess()) may
             * still be physically held here in the new process; global
             * hardware key state isn't scoped per-process. See
             * CeInputSuppressStartKey's comment (ce_input.h). */
            CeInputSuppressStartKey();

            if (!CeDisplayInit(g_hwnd))
                CeLog("WinMain: CeDisplayInit failed after command-line load - continuing without video output");
            CeHideShellChrome(g_hwnd);
        }
        else
        {
            CeLog("WinMain: command-line ROM load failed, falling back to main menu");
            ShowMainMenuDialog(g_hwnd);
        }
    }
    else
    {
        /* Start on the menu ("No ROM loaded", black background) - blocks
         * here until the user opens a ROM (ShowMainMenuDialog only returns
         * once g_romLoaded is true, or the app has already exited via Exit
         * inside the dialog). */
        ShowMainMenuDialog(g_hwnd);
    }

    g_running = 1;
    while (g_running)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running)
            break;

        if (!g_romLoaded || g_paused)
        {
            /* No game running (still on the "No ROM loaded" screen) or
             * the touch-to-reveal menu is up: nothing to emulate/blit
             * this tick. Sleep instead of spinning the message pump at
             * 100% CPU for no reason. */
            Sleep(10);
            continue;
        }

        if (g_audioBuffStatusCb)
        {
            /* Contractually "called right before retro_run() every
             * frame" (see the RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_
             * STATUS_CALLBACK doc in libretro.h) - only set while Video
             * Config's Frame Skip is above 0 (ce_video.c), so this is a
             * no-op call when it's Off. */
            int active, underrunLikely;
            unsigned occupancyPercent;

            if (CeVideoFrameSkipShouldForceRender())
            {
                /* Frame Skip's own cap (see ce_video.h) has already been
                 * hit: report "buffer not active" so the core's
                 * FRAMESKIP_AUTO logic (which only skips when told the
                 * buffer is active *and* underrunning) renders this one
                 * frame regardless of the real audio state. */
                active = 0;
                occupancyPercent = 0;
                underrunLikely = 0;
            }
            else
            {
                CeAudioGetBufferStatus(&active, &occupancyPercent, &underrunLikely);
            }
            g_audioBuffStatusCb(active ? true : false, occupancyPercent, underrunLikely ? true : false);
        }

        {
            /* Temporary perf instrumentation - see ce_audio_sample_batch()
             * and ce_display.c's "perf: blit" for the matching per-frame
             * counters. retro_run()'s own wall time already includes both
             * of those (the core calls ce_video_refresh/
             * ce_audio_sample_batch synchronously from inside it), so
             * comparing this average against "perf: blit" + "perf:
             * audio_push" shows how much is core emulation vs. CE-side
             * I/O. Remove once the bottleneck is identified. */
            static unsigned s_accumMs = 0, s_maxMs = 0, s_count = 0;
            DWORD t0 = GetTickCount();
            unsigned elapsed;

            retro_run();

            elapsed = (unsigned)(GetTickCount() - t0);
            s_lastRetroRunMs = elapsed; /* consumed by ce_environment()'s GET_AUDIO_VIDEO_ENABLE handler - see its declaration above */
            s_accumMs += elapsed;
            if (elapsed > s_maxMs)
                s_maxMs = elapsed;
            if (++s_count >= 60)
            {
                unsigned blitAvg = 0, blitMax = 0;
                CeDisplayGetBlitPerf(&blitAvg, &blitMax);

                CeLog("perf: retro_run avg=%ums max=%ums over %u frames",
                      s_accumMs / s_count, s_maxMs, s_count);

                /* Unified one-liner in the common cross-core format
                 * (every CE port emits this exact shape for side-by-side
                 * comparison). The detailed breakdown lines above/in
                 * ce_display.c ("perf: blit ... (write/bitblt ...)",
                 * "perf: audio_push ...", underrun counts) are kept
                 * as-is. blit avg/max is the last 60-frame snapshot from
                 * ce_display.c (CeDisplayGetBlitPerf). */
                CeLog("perf: retro_run avg=%ums max=%ums blit avg=%ums max=%ums",
                      s_accumMs / s_count, s_maxMs, blitAvg, blitMax);

                s_accumMs = 0;
                s_maxMs = 0;
                s_count = 0;
            }
        }

        /* Frame-rate pacing - this loop previously called retro_run()
         * back-to-back with no throttling at all, so its speed was
         * whatever the device's momentary CPU/GAPI-blit load happened to
         * allow (user-reported "uneven game speed", 2026-08-14
         * hardware). Real-hardware perf logs show
         * retro_run() averaging ~15ms/frame during normal play - a
         * hair *faster* than the 16.67ms/frame NTSC target - so on top
         * of the speed-consistency problem, this was also steadily
         * outrunning the audio ring buffer (built at the NES's real
         * sample rate) by about 10%, which the ring-buffer size alone
         * can't fix no matter how large: eventually it always fills up
         * and overruns.
         *
         * Two paths, ported from the sister PicoDrive CE (PopSG) port:
         * whenever a waveOut device is actually open (the overwhelming
         * common case - only unavailable if waveOutOpen itself failed),
         * pace off the audio ring's own fill level instead of the wall
         * clock. The drain thread (ce_audio.c) consumes that ring at
         * exactly the configured output rate = real time, so holding
         * retro_run() here until the ring has drained below
         * CE_FRAME_PACING_HIGH_MS worth of buffered audio locks
         * emulation speed to actual playback automatically - no fps
         * assumption, no rounding, and (the actual motivation) it can
         * never let the ring grow large enough to force the kind of
         * fill-then-overrun cycle described above. A title too heavy to
         * reach full speed just keeps the ring near-empty and never
         * sleeps here.
         *
         * The GetTickCount-based Bresenham accumulator below is now only
         * the no-audio-device fallback (add 1000ms worth of "frame
         * budget" every g_targetFpsRounded frames, carry the remainder,
         * rather than a fixed 16/17ms alternation, so there's no
         * long-term drift from the rounding - same technique as
         * ce_audio.c's fixed-point resampler, chosen for the same reason:
         * no per-frame floating point on this device's software-emulated
         * VFP). Only *slows down* a too-fast frame (Sleep); a slow one
         * (GAPI stall, heavy scene) is never held back further - there's
         * no frame-skip to catch up with here, so an already-late frame
         * just proceeds immediately. */
        if (CeAudioIsActive())
        {
            unsigned guard;
            for (guard = 0; guard < 120; guard++)
            {
                if (CeAudioGetBufferedMs() <= CE_FRAME_PACING_HIGH_MS)
                    break;
                Sleep(1);
            }
        }
        else
        {
            static DWORD    s_deadlineMs   = 0;
            static unsigned s_fracAccumMs  = 0;
            static int      s_deadlineInit = 0;
            DWORD nowMs = GetTickCount();
            LONG  aheadMs;

            if (!s_deadlineInit)
            {
                s_deadlineMs = nowMs;
                s_fracAccumMs = 0;
                s_deadlineInit = 1;
            }

            s_fracAccumMs += 1000;
            s_deadlineMs  += s_fracAccumMs / g_targetFpsRounded;
            s_fracAccumMs %= g_targetFpsRounded;

            /* Signed subtraction so this still works correctly across
             * GetTickCount()'s ~49.7-day wraparound. */
            aheadMs = (LONG)(s_deadlineMs - nowMs);
            if (aheadMs > 0)
            {
                Sleep((DWORD)aheadMs);
            }
            else if (aheadMs < -500)
            {
                /* Fell more than 500ms behind schedule (a long GAPI
                 * stall, a slow scene, or just the ROM having freshly
                 * loaded) - resync to "now" instead of letting the
                 * pacer try to silently burn through a backlog of
                 * owed frames once things speed back up, which would
                 * just reproduce the very unevenness this is meant to
                 * smooth out. */
                CeLog("WinMain: frame pacing fell %ldms behind schedule, resyncing", (long)-aheadMs);
                s_deadlineMs = nowMs;
            }
        }

        /* Periodic SRAM autosave - the pause-time save in
         * ShowMainMenuDialog() only helps if the menu actually gets
         * opened before the device is powered off; this covers a
         * straight-through play session that never touches the menu at
         * all. ~30s is arbitrary (same margin the sister snes9x2002 CE
         * port uses) - frequent enough to bound how much an in-game save
         * could be lost, infrequent enough that a `.srm` write is not
         * worth timing/skipping for. */
        {
            static DWORD s_lastSramSaveTick = 0;
            DWORD now = GetTickCount();
            if (s_lastSramSaveTick == 0)
                s_lastSramSaveTick = now; /* first frame of gameplay - start the 30s window now, not at an immediate save */
            else if (now - s_lastSramSaveTick >= 30000)
            {
                CeSaveSram();
                s_lastSramSaveTick = now;
            }
        }
    }

    CeLog("WinMain: normal shutdown");
    CeShutdown(0);
    return 0; /* unreachable - CeShutdown() calls ExitProcess() */
}
