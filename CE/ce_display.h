/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * GDI display backend for PopNES - see ce_display.c's header
 * comment for why this replaced the old GAPI (gx.dll) backend
 * (ce_gapi.c/ce_gapi.h, removed). Public API kept intentionally close
 * to the old GapiInit/GapiShutdown/GapiBlitRGB565 shape so ce_main.c's
 * call sites barely needed to change.
 */
#ifndef CE_DISPLAY_H
#define CE_DISPLAY_H

#include <windows.h>

/* One-time setup: creates an off-screen RGB565 DIB section plus its own
 * memory DC, both sized to the physical screen. Safe to call more than
 * once - every call after the first successful one is a cheap no-op
 * that just returns 1 again (see ce_main.c's call sites, unchanged from
 * the old GAPI backend's GapiInit() - they still call this once per
 * pause/resume cycle even though GDI itself only ever needs the real
 * work done once). Returns 1 on success, 0 on failure (not expected in
 * practice - unlike gx.dll, GDI is always present on this device). */
int CeDisplayInit(HWND hwnd);

/* No-ops, kept only so ce_main.c's existing per-dialog suspend/resume
 * call sites (modelled on the old GAPI backend's exclusive-full-screen-
 * surface contract, which genuinely needed releasing before any modal
 * dialog could safely draw on top of it) don't need removing - GDI has
 * no such concept, any number of dialogs already coexist fine with a
 * normally-drawn window. Real teardown is CeDisplayShutdown() below,
 * called once at actual app exit. */
void CeDisplaySuspend(void);
void CeDisplayResume(void);

/* Real teardown (DeleteDC/DeleteObject) - call once, from CeShutdown(). */
void CeDisplayShutdown(void);

/* Scale-and-letterbox blit of a tightly-described RGB565 source frame
 * (src, srcW x srcH pixels, srcPitchBytes bytes/row - given fresh every
 * call, exactly what retro_video_refresh_t hands ce_video_refresh())
 * onto hwnd's client area, honouring CeVideoGetScaleMode(). No-op if
 * CeDisplayInit() hasn't succeeded yet. */
void CeDisplayBlitRGB565(const void *src, unsigned srcW, unsigned srcH, unsigned srcPitchBytes);

/* Redraws whatever CeDisplayBlitRGB565() last drew, from the off-screen
 * DIB section (not the core's own frame pointer, which isn't guaranteed
 * to still be valid/current by the time this runs) - call from
 * WM_PAINT once a ROM is loaded, since GDI drawing (unlike the old GAPI
 * backend, which bypassed this window's own paint pipeline entirely) is
 * a normal part of this window's invalidation/repaint contract. No-op
 * before the first real blit. */
void CeDisplayForceRepaint(void);

/* The image CeDisplayBlitRGB565() last drew, exactly as scaled on screen
 * (depends on Video Config's Scale): top-down RGB565 rows in the
 * off-screen DIB, pitchBytes apart. Stays valid while a dialog has the
 * game paused (CeDisplaySuspend() is a no-op under GDI). Returns 0
 * before the first real blit. Used by the main menu's Screenshot button
 * (ported from the sister PopSNES port). */
int CeDisplayGetLastImage(const void **bits, unsigned *width, unsigned *height, unsigned *pitchBytes);

/* Last 60-frame "perf: blit" avg/max (ms). ce_main.c folds these into a
 * single cross-core-comparable summary line alongside its own retro_run
 * numbers; the detailed "perf: blit ... (write/bitblt ...)" line is
 * still logged separately. 0/0 until the first 60 frames have elapsed. */
void CeDisplayGetBlitPerf(unsigned *avgMs, unsigned *maxMs);

#endif
