/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Video Config: scale mode (1:1 vs stretch-to-fill), transparency
 * effects, and frame skip. Scale mode is consumed directly by
 * ce_display.c's blit; transparency/frame skip are core-side settings
 * exposed through the standard libretro core-options environment calls
 * (see ce_main.c's ce_environment) rather than by reaching into the
 * core's Settings struct directly, matching this port's libretro-
 * frontend-only architecture.
 */
#ifndef CE_VIDEO_H
#define CE_VIDEO_H

#include <windows.h>

typedef enum
{
    CE_SCALE_1TO1        = 0,
    CE_SCALE_EXPAND      = 1,
    /* Fills the display top-to-bottom (same as Expand/Half Stretch);
     * horizontal scale is a FIXED 3/2 (1.5x), not the aspect-correct
     * ratio this mode used before round 53 (~1.425x on this device's
     * 240x224-source/480x320-screen geometry - close, but irrational
     * with respect to the source width). Chosen because a ratio whose
     * denominator is a small power of two lets CeDisplayBlitRGB565()'s
     * write loop use a fixed-cycle, divide/multiply-free unrolled
     * duplicate-and-store path (same idea as Expand's exact-2x fast
     * path, see ce_display.c's s_scale3over2) instead of the general
     * DDA path every other ratio needs - real-hardware A/B testing
     * found this cut this mode's blit write time
     * roughly in half again versus the DDA path alone. The label stays
     * "x1.5" in the Video Config UI either way. */
    CE_SCALE_FULLSCREEN  = 2,
    /* Same vertical fill, fixed 7/4 (1.75x) horizontal scale - was
     * ~1.7125x (Full Screen's aspect-correct width, Expand's full-width
     * stretch) before round 53, for the same reason and with the same
     * ce_display.c fast path (s_scale7over4) as Full Screen above. */
    CE_SCALE_HALFSTRETCH = 3,
} CeScaleMode;

/* Loads any saved settings from the registry (falls back to built-in
 * defaults). Call once from WinMain before retro_init(). */
void CeVideoInit(void);

void CeShowVideoConfigDialog(HWND owner);

/* Consulted directly by ce_display.c's CeDisplayBlitRGB565 every frame. */
CeScaleMode CeVideoGetScaleMode(void);

/* ce_main.c's ce_environment() calls this for RETRO_ENVIRONMENT_GET_VARIABLE:
 * if key is one this module owns (snes9x2002_transparency,
 * snes9x2002_frameskip, snes9x2002_frameskip_interval), fills *outValue
 * with a string valid until the next call and returns 1; otherwise
 * returns 0 and leaves *outValue untouched (falls through to the core's
 * own built-in default for that option). */
int CeVideoEnvGetVariable(const char *key, const char **outValue);

/* ce_main.c's ce_environment() calls this for
 * RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: returns 1 (and clears the flag)
 * exactly once after CeShowVideoConfigDialog's OK handler changes
 * transparency/frame skip, so the core picks the new value up on the
 * very next retro_run() without needing a fresh retro_load_game(). */
int CeVideoConsumeDirty(void);

/* Dead code, kept only for the (harmless - g_audioBuffStatusCb never
 * becomes non-NULL for this core, see ce_main.c) existing call sites:
 * this pair was written for the sister PicoDrive/snes9x2002 template's
 * protocol (RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, an
 * "auto" numeric core option), which QuickNES's libretro.cpp never
 * implements (confirmed by grep). Real frame skip for
 * this core uses a completely different, genuinely-implemented
 * mechanism - see CeVideoFrameSkipDecide() below - do not confuse the
 * two. */
int CeVideoFrameSkipShouldForceRender(void);
void CeVideoFrameSkipNotifyRendered(int rendered);

/* Real, core-backed frame skip: QuickNES's libretro.cpp already has a
 * genuine "run CPU/APU normally, skip PPU pixel writes" path -
 * nes_emu/Nes_Emu.cpp's emulate_skip_frame() (called from retro_run()
 * whenever the environment call below reports video disabled) leaves
 * game speed and audio timing completely unaffected, and also skips the
 * palette-conversion loop and video_cb call in libretro.cpp - which in
 * turn means ce_video_refresh() (and therefore CeDisplayBlitRGB565()) never
 * runs for that frame either. This is a strict superset of a CE-side-
 * only "skip the GAPI blit" scheme: it saves the PPU/palette work
 * inside retro_run() too, not just the video transfer.
 *
 * CeVideoFrameSkipDecide() is called once per frame by ce_main.c's
 * ce_environment(), from its RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE
 * handler, right before answering that environment call.
 * lastRetroRunMs is the *previous* frame's measured retro_run() wall
 * time (0 before the first measurement exists yet); frameBudgetMs is
 * 1000 / target fps. Returns 1 if this frame should render normally, 0
 * if the core should be told to skip video this frame.
 *
 * Video Config's Frame Skip dial (persisted as VideoFrameSkip, 0..30 -
 * see ce_video.c's CE_FRAMESKIP_MAX) is the *ceiling* on consecutive
 * skipped frames, not a fixed skip-every-Nth cadence - 0 disables this
 * entirely (always render, byte-for-byte the same as before this
 * feature existed). Above 0, a frame is only ever skipped when at least
 * one of two independent "falling behind" signals says so:
 *
 *   - the audio ring buffer's own occupancy/underrun reading
 *     (ce_audio.c's CeAudioGetBufferStatus() - the same signal
 *     AUDIO_BUFFER_STATUS_CALLBACK would have used, had the core
 *     implemented it).
 *   - the previous frame's retro_run() wall time (lastRetroRunMs) having
 *     exceeded frameBudgetMs - i.e. the emulator/PPU work alone, never
 *     mind GAPI/audio I/O, didn't fit in one frame's time.
 *
 * Either signal alone can trigger a skip; neither being true resets the
 * consecutive-skip counter to 0 and forces a render regardless of the
 * ceiling, same as hitting the ceiling itself does. */
int CeVideoFrameSkipDecide(unsigned lastRetroRunMs, unsigned frameBudgetMs);

#endif
