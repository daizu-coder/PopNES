/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Part of PopNES, a QuickNES-core port for the SHARP Brain PW-G5200.
 * This file is MIT (CE/LICENSE); PopNES as a whole (AppMain.exe) is
 * distributed under GPL-2.0 - see CE/LICENSING.md and
 * CE/THIRDPARTY_LICENSES.txt.
 */
/*
 * ce_display.c - GDI display backend for PopNES.
 *
 * Replaces the old GAPI (gx.dll) backend (ce_gapi.c/ce_gapi.h, removed
 * by this change). Not a technical fix - a licensing one: the gx.dll
 * builds circulating for this device family are of unverifiable
 * provenance, so bundling or co-distributing one would leave its
 * licensing status unclear. The sister gnuboy CE port (same device,
 * same cegcc toolchain) reached the same conclusion and dropped the
 * GAPI dependency entirely rather than keep relying on it (see the dev notes). Two GAPI-free
 * alternatives were investigated there and ruled out on real hardware
 * before landing on plain GDI:
 *   - ExtEscape(GETRAWFRAMEBUFFER) - this device's display driver
 *     reports it unsupported (result=0);
 *   - DirectDraw (ddraw.dll) - DirectDrawCreate resolves, but calling
 *     into the object it returns crashed the device outright (a
 *     hardware BER report) before any UI could even appear; cegcc also
 *     ships no ddraw.h, so continuing down that path meant guessing at
 *     an un-headered COM ABI with no way to tell a wrong vtable slot
 *     from a genuinely broken driver.
 * GDI (CreateDIBSection/CreateCompatibleDC/BitBlt/PatBlt) is what's
 * left: part of coredll.dll like every other Win32 call this port
 * already makes, so it adds no extra DLL dependency and no licensing
 * question at all - confirmed at real-time speed on this exact device
 * family by both the gnuboy CE port and a separate small GDI-only
 * homebrew sample for this device family (GetDC/CreateCompatibleDC/
 * BitBlt/FillRect/SetPixel only).
 *
 * Unlike gnuboy (which renders scanline-by-scanline into a small fixed
 * native buffer - see that project's ce_video.c), QuickNES's own
 * retro_video_refresh_t callback (ce_video_refresh() in ce_main.c)
 * hands this module a complete already-RGB565 frame every call - the
 * exact same (src, srcW, srcH, srcPitchBytes) shape the old GAPI
 * backend's GapiBlitRGB565() consumed - so the scale-and-letterbox math
 * below (nearest-neighbor, 16.16 fixed-point stepping, one divide per
 * axis per frame, a precomputed per-column source-x lookup table) is
 * carried over unchanged from that file. Only the destination changed:
 * instead of writing straight into a GXBeginDraw() framebuffer pointer,
 * CeDisplayBlitRGB565() writes into a small scratch region of an
 * off-screen RGB565 DIB section (s_dibBits, sized to the full physical
 * screen but only ever written at rows 0..dstH-1 / columns 0..dstW-1 -
 * gnuboy's ce_video.c independently arrived at this same "scratch
 * corner of a screen-sized DIB, offset only at BitBlt time" shape,
 * confirmed fast on real hardware there too) and transfers just that
 * dstW x dstH rectangle onto the window with a single plain 1:1
 * BitBlt() at (dstX, dstY) - not StretchDIBits()/StretchBlt(), which
 * gnuboy's own investigation found is much slower than doing the scale
 * by hand on this device's driver (no fast path for arbitrary-ratio
 * stretching - see the dev notes).
 *
 * The old GAPI backend's raw-framebuffer offX-must-stay-even 32-bit
 * pixel-packing fast path (two RGB565 pixels per 32-bit store, only
 * valid when cbxPitch==2) doesn't carry over as-is - a DIB section
 * accessed as uint16_t* has no equivalent alignment hazard, since the
 * scratch region this module writes into always starts at DIB column 0
 * (dstX/dstY are applied only at BitBlt time, per the doc comment
 * above), and s_dibPitch is DWORD-aligned by construction. That made it
 * safe to reintroduce the same two-pixels-per-32-bit-store idea, without
 * the old parity guard, in the scaled (non-identity) branch of
 * CeDisplayBlitRGB565() below - see the comment there.
 */
#include "ce_display.h"
#include "ce_log.h"
#include "ce_video.h"

#include <stdint.h>
#include <string.h>

static HWND      s_hwnd      = NULL;
static int        s_physW     = 0;
static int        s_physH     = 0;
static HDC        s_memDC     = NULL;
static HBITMAP    s_dibBitmap = NULL;
static uint16_t   *s_dibBits  = NULL;
static int         s_dibPitch = 0;

/* GetDC/ReleaseDC on this device are documented WinCE performance traps
 * when called every frame (unlike GAPI's GXBeginDraw()/GXEndDraw(), which
 * were purpose-built for lightweight per-frame framebuffer access) - a
 * real-hardware comparison against the old GAPI backend found this
 * module's blits several times slower at the same scale mode.
 * s_cachedWndDC holds one HDC for s_hwnd across
 * frames instead of fetching/releasing it on every blit; CeDisplaySuspend()
 * releases it before a dialog takes the screen (matching the old GAPI
 * backend's suspend/resume pairing at those same call sites in
 * ce_main.c), and the next CeDisplayBlitRGB565()/CeDisplayForceRepaint()
 * call lazily re-acquires it via GetCachedWndDC(). */
static HDC s_cachedWndDC = NULL;

static HDC GetCachedWndDC(void)
{
    if (!s_cachedWndDC)
        s_cachedWndDC = GetDC(s_hwnd);
    return s_cachedWndDC;
}

int CeDisplayInit(HWND hwnd)
{
    HDC hdc;
    struct { BITMAPINFOHEADER h; DWORD masks[3]; } dibInfo;

    if (s_dibBitmap)
        return 1; /* already set up - see this function's own doc comment */

    s_hwnd  = hwnd;
    s_physW = GetSystemMetrics(SM_CXSCREEN);
    s_physH = GetSystemMetrics(SM_CYSCREEN);
    CeLog("CeDisplayInit: GetSystemMetrics cx=%d cy=%d", s_physW, s_physH);
    if (s_physW <= 0 || s_physH <= 0)
        return 0;

    hdc = GetDC(hwnd);
    if (!hdc)
    {
        CeLog("CeDisplayInit: GetDC(hwnd) failed");
        return 0;
    }

    /* Paint the physical screen black immediately - otherwise whatever
     * was in video memory before this process ever wrote to it stays
     * visible behind the menu/dialogs until the first real frame blits
     * (same reasoning, and same real-hardware-observed symptom, as
     * gnuboy's ce_video.c vid_init()). */
    PatBlt(hdc, 0, 0, s_physW, s_physH, BLACKNESS);

    memset(&dibInfo, 0, sizeof(dibInfo));
    dibInfo.h.biSize = sizeof(BITMAPINFOHEADER);
    dibInfo.h.biWidth = s_physW;
    dibInfo.h.biHeight = -s_physH; /* negative = top-down, matches the source frame's own row order */
    dibInfo.h.biPlanes = 1;
    dibInfo.h.biBitCount = 16;
    dibInfo.h.biCompression = BI_BITFIELDS;
    dibInfo.masks[0] = 0xF800; /* red:   5 bits @ bit 11 - RGB565, matches SET_PIXEL_FORMAT */
    dibInfo.masks[1] = 0x07E0; /* green: 6 bits @ bit 5 */
    dibInfo.masks[2] = 0x001F; /* blue:  5 bits @ bit 0 */

    s_dibBitmap = CreateDIBSection(hdc, (BITMAPINFO *)&dibInfo, DIB_RGB_COLORS, (void **)&s_dibBits, NULL, 0);
    if (!s_dibBitmap || !s_dibBits)
    {
        CeLog("CeDisplayInit: CreateDIBSection failed");
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    s_dibPitch = ((s_physW * 16 + 31) / 32) * 4; /* DIB rows are DWORD-aligned */

    s_memDC = CreateCompatibleDC(hdc);
    if (!s_memDC)
    {
        CeLog("CeDisplayInit: CreateCompatibleDC failed");
        DeleteObject(s_dibBitmap);
        s_dibBitmap = NULL;
        s_dibBits = NULL;
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    SelectObject(s_memDC, s_dibBitmap);

    ReleaseDC(hwnd, hdc);
    return 1;
}

void CeDisplaySuspend(void)
{
    /* Release the cached window DC before a dialog (Sound/Video Config,
     * Save/Load State, the ROM picker, ...) takes the screen - see
     * s_cachedWndDC's doc comment above. */
    if (s_cachedWndDC)
    {
        ReleaseDC(s_hwnd, s_cachedWndDC);
        s_cachedWndDC = NULL;
    }
}

void CeDisplayResume(void)
{
}

void CeDisplayShutdown(void)
{
    if (s_cachedWndDC)
    {
        ReleaseDC(s_hwnd, s_cachedWndDC);
        s_cachedWndDC = NULL;
    }
    if (s_memDC)
    {
        DeleteDC(s_memDC);
        s_memDC = NULL;
    }
    if (s_dibBitmap)
    {
        DeleteObject(s_dibBitmap);
        s_dibBitmap = NULL;
        s_dibBits = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Scale geometry cache                                                */
/* ------------------------------------------------------------------ */

/* Recomputed only when something that affects it actually changes
 * (Scale mode, or the source frame's own dimensions - QuickNES's are
 * fixed per ROM in practice, but nothing here assumes that), not every
 * frame - same reasoning as gnuboy's ce_video.c s_scaleGeomDirty: this
 * also gates the one-time PatBlt border clear below (a bordered mode's
 * letterbox area only needs clearing once, since this device's GDI
 * surface is a direct, persistent framebuffer - untouched pixels simply
 * stay put across frames, confirmed by gnuboy's ce_video.c on the same
 * device). CE_DISPLAY_MAX_DIM comfortably exceeds this device's physical
 * width (480, per gnuboy's hardware-confirmed GetSystemMetrics log) and
 * bounds s_dstW (destination pixels). */
#define CE_DISPLAY_MAX_DIM 512
static int      s_dstX, s_dstY, s_dstW, s_dstH;
static uint32_t s_xStep, s_yStep;
static int      s_identityX; /* 1 when s_dstW == srcW: no horizontal scaling, so each row is a straight copy */
static int      s_scale2x;   /* 1 when s_dstW == srcW*2: exact horizontal doubling (this device's common
                               * 480-wide-physical-screen-vs-240-wide-NES-frame Expand case) - takes a duplicate-and-store fast path below instead
                               * of the general DDA path (see CeDisplayBlitRGB565()'s write loop), since the
                               * fixed 2x ratio lets it pack two output pixels into one aligned 32-bit
                               * store, which the DDA path's per-pixel stores can't do. */
static int      s_scale3over2; /* 1 when s_dstW*2 == srcW*3 AND srcW%4==0: Full Screen 1:1's now-fixed
                               * 3/2 ratio (see ce_video.h's CE_SCALE_FULLSCREEN comment). srcW%4==0 guarantees the 4-source/6-dest unrolled cycle below
                               * divides srcW exactly, with no leftover pixels to special-case. */
static int      s_scale7over4; /* 1 when s_dstW*4 == srcW*7 AND srcW%8==0: Half Stretch's now-fixed 7/4
                               * ratio, same reasoning as s_scale3over2 but an 8-source/14-dest cycle. */

static int      s_geomValid  = 0;
static unsigned  s_lastSrcW   = 0;
static unsigned  s_lastSrcH   = 0;
static CeScaleMode s_lastScaleMode;

static void ComputeGeometry(unsigned srcW, unsigned srcH, CeScaleMode scaleMode)
{
    unsigned dstW = (unsigned)s_physW;
    unsigned dstH = (unsigned)s_physH;
    unsigned blitW, blitH;

    if (scaleMode == CE_SCALE_1TO1)
    {
        /* Centred, unscaled - crop rather than scale down if the source
         * is somehow larger than the physical display. */
        blitW = (srcW < dstW) ? srcW : dstW;
        blitH = (srcH < dstH) ? srcH : dstH;
    }
    else if (scaleMode == CE_SCALE_FULLSCREEN || scaleMode == CE_SCALE_HALFSTRETCH)
    {
        /* Fill the display top-to-bottom; horizontal scale is a FIXED
         * 3/2 (Full Screen) or 7/4 (Half Stretch) ratio - not derived
         * from dstH/srcH - so that CeDisplayBlitRGB565()'s write loop
         * can recognise it and take a fixed-cycle fast path below (see
         * s_scale3over2/s_scale7over4 and CeScaleMode in ce_video.h). */
        blitH = dstH;

        if (scaleMode == CE_SCALE_FULLSCREEN)
            blitW = (srcW * 3) / 2;
        else /* CE_SCALE_HALFSTRETCH */
            blitW = (srcW * 7) / 4;
    }
    else /* CE_SCALE_EXPAND */
    {
        blitW = dstW;
        blitH = dstH;
    }

    if (blitW > dstW) blitW = dstW;
    if (blitH > dstH) blitH = dstH;
    if (blitW < 1) blitW = 1;
    if (blitH < 1) blitH = 1;
    if (blitW > CE_DISPLAY_MAX_DIM)
        blitW = CE_DISPLAY_MAX_DIM; /* defensive clamp - see CE_DISPLAY_MAX_DIM's doc comment */

    s_dstW = (int)blitW;
    s_dstH = (int)blitH;
    s_dstX = (int)((dstW - blitW) / 2);
    s_dstY = (int)((dstH - blitH) / 2);

    s_xStep = (srcW << 16) / blitW;
    s_yStep = (srcH << 16) / blitH;

    s_identityX = ((unsigned)s_dstW == srcW);
    s_scale2x = (!s_identityX && (unsigned)s_dstW == srcW * 2);
    s_scale3over2 = (!s_identityX && !s_scale2x
                      && (unsigned)s_dstW * 2 == srcW * 3 && (srcW % 4) == 0);
    s_scale7over4 = (!s_identityX && !s_scale2x && !s_scale3over2
                      && (unsigned)s_dstW * 4 == srcW * 7 && (srcW % 8) == 0);
    /* Any ratio none of the above recognise (e.g. Full Screen/Half
     * Stretch clamped by the display-width cap in the caller, or a
     * source width the 3/2 or 7/4 cycle can't divide exactly) falls
     * through to CeDisplayBlitRGB565()'s DDA write loop directly from
     * s_xStep above - no precomputed table needed (see that loop's doc
     * comment). */
}

/* Cheap perf instrumentation (no profiler on this device - see
 * CeLog()'s doc comment): logs average/max blit time every 60 calls,
 * then resets - same "perf: blit" shape the old GAPI backend logged,
 * kept because ce_main.c's own "perf: retro_run"/"perf: audio_push"
 * comments still lean on it (retro_run_avg - blit_avg - audio_push_avg
 * standing in for "core CPU/PPU/sound-chip emulation alone").
 *
 * s_writeAccumMs/s_bitbltAccumMs split the total further, into just the
 * nearest-neighbor write loop below and just the BitBlt() call - added
 * after a real-hardware A/B test found that
 * caching the per-frame GetDC()/ReleaseDC() pair (see GetCachedWndDC()
 * above) made no measurable difference to the total, which meant the
 * ~31ms/frame gap against the old GAPI backend (~9ms/frame at the same
 * Expand scale mode) has to live in one of these two remaining pieces
 * instead. Both are folded into (and thus still counted by) the
 * existing total/count above; they just add a breakdown to the same log
 * line rather than replacing it. */
static unsigned s_blitAccumMs   = 0;
static unsigned s_blitMaxMs     = 0;
static unsigned s_blitCount     = 0;

/* Snapshot of the last 60-frame "perf: blit" avg/max, so ce_main.c's
 * retro_run perf block can fold it into a single cross-core-comparable
 * summary line ("perf: retro_run avg=... blit avg=...") without moving
 * the detailed breakdown line above. At most ~60 frames stale relative
 * to that block's own window - both counters advance 1:1 per frame so
 * they stay close. */
static unsigned s_blitLastAvgMs = 0;
static unsigned s_blitLastMaxMs = 0;

void CeDisplayGetBlitPerf(unsigned *avgMs, unsigned *maxMs)
{
    if (avgMs) *avgMs = s_blitLastAvgMs;
    if (maxMs) *maxMs = s_blitLastMaxMs;
}
static unsigned s_writeAccumMs  = 0;
static unsigned s_writeMaxMs    = 0;
static unsigned s_bitbltAccumMs = 0;
static unsigned s_bitbltMaxMs   = 0;

void CeDisplayBlitRGB565(const void *src, unsigned srcW, unsigned srcH, unsigned srcPitchBytes)
{
    const uint8_t *srcBytes = (const uint8_t *)src;
    CeScaleMode scaleMode;
    HDC hdc;
    uint32_t yAccum;
    int y;
    DWORD t0, elapsed;
    DWORD tWrite0, writeElapsed;
    DWORD tBlit0, bitbltElapsed;

    if (!s_dibBitmap || !srcW || !srcH || !srcPitchBytes)
        return;

    t0 = GetTickCount();

    scaleMode = CeVideoGetScaleMode();
    if (!s_geomValid || srcW != s_lastSrcW || srcH != s_lastSrcH || scaleMode != s_lastScaleMode)
    {
        /* Something that affects the destination rect changed since the
         * last frame (or this is the first frame) - clear the whole
         * physical screen once so a shrinking bordered mode doesn't
         * leave stale pixels from a previous, larger rect around the
         * new one (see this file's header comment on s_geomValid). */
        hdc = GetCachedWndDC();
        if (hdc)
            PatBlt(hdc, 0, 0, s_physW, s_physH, BLACKNESS);

        ComputeGeometry(srcW, srcH, scaleMode);
        s_lastSrcW = srcW;
        s_lastSrcH = srcH;
        s_lastScaleMode = scaleMode;
        s_geomValid = 1;
    }

    /* Nearest-neighbor stretch into the DIB's top-left dstW x dstH
     * corner (not offset by dstX/dstY within the DIB itself - the
     * offset is applied only once, below, at BitBlt time) - same
     * technique the old GAPI backend used directly on its raw
     * framebuffer pointer. One divide per axis per frame (in
     * ComputeGeometry above), never inside this pixel loop. */
    tWrite0 = GetTickCount();
    yAccum = 0;
    for (y = 0; y < s_dstH; y++)
    {
        unsigned srcY = (unsigned)(yAccum >> 16);
        const uint16_t *srcRow = (const uint16_t *)(srcBytes + (size_t)srcY * srcPitchBytes);
        uint16_t *dstRow = (uint16_t *)((uint8_t *)s_dibBits + (size_t)y * s_dibPitch);

        if (s_identityX)
            memcpy(dstRow, srcRow, (size_t)s_dstW * 2);
        else if (s_scale2x)
        {
            /* Exact horizontal doubling (s_dstW == srcW*2 - this
             * device's common 480-wide-physical-screen-vs-240-wide-
             * NES-frame Expand case, see s_scale2x's doc comment above)
             * - every source pixel maps to exactly two adjacent
             * destination pixels, in source order, so this needs
             * neither the s_xLut gather nor its per-pixel multiply:
             * just a sequential read and a duplicate-and-store. dstRow
             * is always 4-byte aligned here (see this file's header
             * comment), so both copies of a source pixel pack into one
             * aligned 32-bit store. A real-hardware A/B test measured the general gather path
             * below at ~27ms/frame for this exact case, against ~3-4ms
             * for the identityX memcpy path at the same resolution -
             * this path exists to close that gap. Assumes a
             * little-endian target, same as the gather path below. */
            unsigned sx;
            uint32_t *dstRow32 = (uint32_t *)dstRow;
            for (sx = 0; sx < srcW; sx++)
            {
                uint32_t px = srcRow[sx];
                dstRow32[sx] = px | (px << 16);
            }
        }
        else if (s_scale3over2)
        {
            /* Fixed 3/2 ratio (Full Screen 1:1 "x1.5" - see s_scale3over2's
             * doc comment): 4 source pixels become
             * 6 dest pixels, weighted {2,2,1,1} and packed into exactly 3
             * aligned 32-bit stores per cycle - (S0,S0)(S1,S1)(S2,S3) - so
             * every store here is a 32-bit pack, same as s_scale2x above,
             * unlike the generic DDA path's per-pixel 16-bit stores. dstRow
             * starts 4-byte aligned (see this file's header comment) and
             * each cycle advances the write pointer by 3 uint32_t (12
             * bytes, still a multiple of 4), so alignment holds across
             * cycles too - no unaligned 32-bit access ever occurs (see
             * the dev notes' -DNO_UNALIGNED_ACCESS note for why that
             * matters on this target). srcW%4==0 is
             * guaranteed by the s_scale3over2 flag check, so this divides
             * srcW with nothing left over. Assumes a little-endian target,
             * same as s_scale2x above. */
            unsigned sx;
            uint32_t *d32 = (uint32_t *)dstRow;
            for (sx = 0; sx < srcW; sx += 4)
            {
                uint32_t p0 = srcRow[sx];
                uint32_t p1 = srcRow[sx + 1];
                uint32_t p2 = srcRow[sx + 2];
                uint32_t p3 = srcRow[sx + 3];
                d32[0] = p0 | (p0 << 16);
                d32[1] = p1 | (p1 << 16);
                d32[2] = p2 | (p3 << 16);
                d32 += 3;
            }
        }
        else if (s_scale7over4)
        {
            /* Fixed 7/4 ratio (Half Stretch "Wide" - see s_scale7over4's
             * doc comment): 8 source pixels become 14 dest pixels,
             * weighted {2,2,2,1,2,2,2,1}, packed into exactly 7 aligned
             * 32-bit stores per cycle - (S0,S0)(S1,S1)(S2,S2)(S3,S4)(S4,S5)
             * (S5,S6)(S6,S7) - same alignment reasoning as s_scale3over2
             * above (each cycle advances the write pointer by 7 uint32_t =
             * 28 bytes, a multiple of 4); srcW%8==0 is guaranteed by the
             * flag check. */
            unsigned sx;
            uint32_t *d32 = (uint32_t *)dstRow;
            for (sx = 0; sx < srcW; sx += 8)
            {
                uint32_t p0 = srcRow[sx];
                uint32_t p1 = srcRow[sx + 1];
                uint32_t p2 = srcRow[sx + 2];
                uint32_t p3 = srcRow[sx + 3];
                uint32_t p4 = srcRow[sx + 4];
                uint32_t p5 = srcRow[sx + 5];
                uint32_t p6 = srcRow[sx + 6];
                uint32_t p7 = srcRow[sx + 7];
                d32[0] = p0 | (p0 << 16);
                d32[1] = p1 | (p1 << 16);
                d32[2] = p2 | (p2 << 16);
                d32[3] = p3 | (p4 << 16);
                d32[4] = p4 | (p5 << 16);
                d32[5] = p5 | (p6 << 16);
                d32[6] = p6 | (p7 << 16);
                d32 += 7;
            }
        }
        else
        {
            /* Generic nearest-neighbor upscale fallback - reached only
             * when none of identityX/scale2x/scale3over2/scale7over4
             * match: a source width the 3/2 or 7/4 fast paths can't
             * divide exactly (see their srcW%4==0 / srcW%8==0 guards),
             * or a Full Screen/Half Stretch width clamped by the
             * display-width cap in ComputeGeometry - not the normal
             * case at this device's 240-wide-NES-frame/480-wide-screen
             * geometry since round 53. Round 50
             * first tried replacing the old per-destination-pixel
             * s_xLut gather (the identified culprit
             * for Expand's ~27ms/frame) with a precomputed per-source-
             * column run-length table walked by a `while` loop - real
             * hardware measured that at essentially NO improvement over
             * the original gather for these ratios (round 51): the
             * data-dependent variable-trip-count inner loop turned out
             * to cost about as much as the double indirection it
             * replaced. This is a DDA (Bresenham-style) incremental
             * scan instead: walks the DESTINATION row once, same as the
             * old gather did (s_dstW iterations, still the unavoidable
             * minimum store count), but advances the SOURCE pointer via
             * a running 16.16 fixed-point error accumulator instead of
             * recomputing an index with a multiply+shift (or looking
             * one up) on every iteration - one add, one compare, and
             * only-when-needed pointer bump and reload, no table at
             * all. s_xStep < 0x10000 is guaranteed here (s_dstW > srcW
             * whenever this branch runs, since identityX/scale2x
             * already handled the ==/==*2 cases), so the accumulator
             * carries at most once per destination pixel in practice;
             * the `while` below is defensive, not because more than one
             * carry is expected. The loop stops one pixel short of
             * s_dstW and finishes with a plain store so the last
             * increment (which would land one past the final valid
             * source column) never actually dereferences out of
             * bounds. */
            const uint16_t *s = srcRow;
            uint16_t *d = dstRow;
            uint16_t px = *s;
            uint32_t err = 0;
            int i;
            for (i = 0; i < s_dstW - 1; i++)
            {
                *d++ = px;
                err += s_xStep;
                while (err >= 0x10000)
                {
                    err -= 0x10000;
                    s++;
                    px = *s;
                }
            }
            *d = px;
        }

        yAccum += s_yStep;
    }

    writeElapsed = (DWORD)(GetTickCount() - tWrite0);
    s_writeAccumMs += writeElapsed;
    if (writeElapsed > s_writeMaxMs)
        s_writeMaxMs = writeElapsed;

    hdc = GetCachedWndDC();
    if (hdc)
    {
        tBlit0 = GetTickCount();
        BitBlt(hdc, s_dstX, s_dstY, s_dstW, s_dstH, s_memDC, 0, 0, SRCCOPY);
        bitbltElapsed = (DWORD)(GetTickCount() - tBlit0);
        s_bitbltAccumMs += bitbltElapsed;
        if (bitbltElapsed > s_bitbltMaxMs)
            s_bitbltMaxMs = bitbltElapsed;
    }

    elapsed = (DWORD)(GetTickCount() - t0);
    s_blitAccumMs += elapsed;
    if (elapsed > s_blitMaxMs)
        s_blitMaxMs = elapsed;
    if (++s_blitCount >= 60)
    {
        s_blitLastAvgMs = s_blitAccumMs / s_blitCount;
        s_blitLastMaxMs = s_blitMaxMs;
        CeLog("perf: blit avg=%ums max=%ums (write avg=%ums max=%ums, bitblt avg=%ums max=%ums) over %u frames",
              s_blitLastAvgMs, s_blitLastMaxMs,
              s_writeAccumMs / s_blitCount, s_writeMaxMs,
              s_bitbltAccumMs / s_blitCount, s_bitbltMaxMs,
              s_blitCount);
        s_blitAccumMs = 0;
        s_blitMaxMs = 0;
        s_writeAccumMs = 0;
        s_writeMaxMs = 0;
        s_bitbltAccumMs = 0;
        s_bitbltMaxMs = 0;
        s_blitCount = 0;
    }
}

int CeDisplayGetLastImage(const void **bits, unsigned *width, unsigned *height, unsigned *pitchBytes)
{
    if (!s_dibBits || !s_geomValid)
        return 0;
    *bits = s_dibBits;
    *width = (unsigned)s_dstW;
    *height = (unsigned)s_dstH;
    *pitchBytes = (unsigned)s_dibPitch;
    return 1;
}

void CeDisplayForceRepaint(void)
{
    HDC hdc;

    if (!s_dibBitmap || !s_geomValid)
        return; /* nothing blitted yet - the caller's own black fill is all there is to show */

    /* Re-transfers the DIB's already-scaled top-left dstW x dstH corner
     * (last written by CeDisplayBlitRGB565() above, and never touched
     * since) - not the core's own frame pointer, which isn't guaranteed
     * to still be valid/current by the time a WM_PAINT happens to fire.
     * The caller is expected to have already painted the invalidated
     * region black first (ce_main.c's WM_PAINT, same as gnuboy's own
     * WndProc) - this only redraws the game rect on top of that. */
    hdc = GetCachedWndDC();
    if (!hdc)
        return;
    BitBlt(hdc, s_dstX, s_dstY, s_dstW, s_dstH, s_memDC, 0, 0, SRCCOPY);
}
