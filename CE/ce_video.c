/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Part of PopNES, a QuickNES-core port for the SHARP Brain PW-G5300.
 * This file is MIT (CE/LICENSE); PopNES as a whole (AppMain.exe) is
 * distributed under GPL-2.0 - see CE/LICENSING.md and
 * CE/THIRDPARTY_LICENSES.txt.
 */
/*
 * Video Config dialog (IDD_VIDEOCONFIG in ce_res.rc) - see ce_video.h.
 *
 * Scale mode is a pure CE-frontend concern (ce_display.c's blit reads
 * CeVideoGetScaleMode() directly, no core involvement). No Sprite Limit
 * is a core feature already exposed by libretro/libretro.cpp's
 * check_variables() through the standard libretro core-options
 * environment call (quicknes_no_sprite_limit - see that function and
 * libretro_core_options.h) - CE answers that environment query from
 * ce_main.c's ce_environment() using the accessor below instead of
 * poking the core's Nes_Emu state directly, keeping this port a
 * libretro *frontend* (see CE/Makefile's scope notes -
 * core/libretro glue is not touched).
 *
 * There is no Frame Skip control in this dialog (see ce_res.rc's
 * IDD_VIDEOCONFIG comment): QuickNES exposes neither a "*_frameskip"
 * core option nor RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, so
 * CeVideoFrameSkipShouldForceRender()/NotifyRendered() below are
 * deliberately inert stubs, kept only so ce_main.c's existing (harmless,
 * because g_audioBuffStatusCb never becomes non-NULL for this core)
 * call sites don't need edits - see ce_video.h for the extension note if
 * real-hardware measurement ever shows QuickNES needs frame skipping.
 *
 * Beyond sprite limit, this module also answers QuickNES's audio-mode
 * and sample-rate options with CPU-conscious defaults (no UI for these -
 * see CeVideoEnvGetVariable below): this device's own audio path
 * (ce_audio.c) downmixes to mono in software regardless of what the core
 * renders, so there is no user-facing reason to trade CPU for QuickNES's
 * higher-fidelity audio modes here.
 */
#include "ce_video.h"
#include "ce_log.h"
#include "ce_config.h"
#include "ce_lang.h"
#include "ce_bmpfont.h"
#include "ce_fileopen.h"
#include "ce_resource.h"
#include "ce_audio.h"

#include <string.h>

static CeScaleMode s_scaleMode  = CE_SCALE_EXPAND; /* matches pre-Video-Config behaviour (always stretch-to-fill) */
/* "No Sprite Limit" - maps to the core's quicknes_no_sprite_limit
 * option: checked removes the real NES hardware's 8-sprite-per-scanline
 * limit (less flicker, but some games exploit the limit for effects).
 * Off by default, matching the core's own built-in default - this is
 * also the lighter-CPU choice (see libretro_core_options.h's own
 * description of the option), so CE doesn't override it. */
static int         s_noSpriteLimit = 0;

/* Real frame skip (see ce_video.h's CeVideoFrameSkipDecide comment) -
 * 0 = Off (always render), 1..CE_FRAMESKIP_MAX = ceiling on consecutive
 * skipped frames. 30 matches the sister PicoDrive/snes9x2002 template's
 * own 0..30 convention (round 31, user request - widened back up from
 * an initial 4-frame cap chosen out of caution before any hardware
 * numbers existed for this specific mechanism; round 30's hardware
 * test of the 0..4 range came back clean, so there was no longer a
 * concrete reason to stay more conservative than the sister ports).
 * Note this is still just a *ceiling*, not a fixed skip-every-Nth
 * cadence (see CeVideoFrameSkipDecide) - CeAudioGetBufferStatus()/
 * retro_run() timing have to actually be falling behind for anywhere
 * near this many consecutive frames to be skipped in practice; picking
 * a high ceiling only matters when the device is falling behind badly
 * enough that even that many skipped frames doesn't fully catch up. */
#define CE_FRAMESKIP_MAX 30
static int      s_frameSkipMax    = 0;
static unsigned s_frameSkipStreak = 0;

/* "Enable Debug Logging" checkbox (IDC_VC_DEBUGLOG) - default off. While
 * off, CeLog() is a no-op and no log file is created/appended (see
 * ce_log.h). WinMain applies the persisted value once right after
 * CeConfigLoad(); this module keeps s_debugLog as the source of truth
 * for the checkbox and re-applies it via CeLogSetEnabled() whenever the
 * dialog is committed. */
static int         s_debugLog     = 0;

static int s_dirty = 0; /* consumed by CeVideoConsumeDirty() - see ce_video.h */

void CeVideoInit(void)
{
    int savedScaleMode;

    savedScaleMode  = CeConfigGetInt("VideoScaleMode", (int)s_scaleMode);
    switch (savedScaleMode)
    {
    case CE_SCALE_1TO1:
    case CE_SCALE_EXPAND:
    case CE_SCALE_FULLSCREEN:
    case CE_SCALE_HALFSTRETCH:
        s_scaleMode = (CeScaleMode)savedScaleMode;
        break;
    default:
        s_scaleMode = CE_SCALE_EXPAND; /* unrecognised value in an old/corrupt config file */
        break;
    }
    s_noSpriteLimit  = CeConfigGetInt("VideoNoSpriteLimit", s_noSpriteLimit);

    s_frameSkipMax = CeConfigGetInt("VideoFrameSkip", s_frameSkipMax);
    if (s_frameSkipMax < 0 || s_frameSkipMax > CE_FRAMESKIP_MAX)
        s_frameSkipMax = 0; /* unrecognised value in an old/corrupt config file */

    s_debugLog = CeConfigGetInt("VideoDebugLog", s_debugLog);
    CeLogSetEnabled(s_debugLog); /* WinMain already did this right after CeConfigLoad(); keep the two in sync */

    CeLog("CeVideoInit: loaded scaleMode=%d noSpriteLimit=%d frameSkip=%d debugLog=%d from config file",
          (int)s_scaleMode, s_noSpriteLimit, s_frameSkipMax, s_debugLog);
}

/* Registry-based persistence (samDesired/RegFlushKey lessons learned earlier) didn't survive an
 * actual power-off on this
 * device (round 9 user report) - now goes through ce_config.c's plain
 * config file instead, same as ce_input.c/ce_audio.c. */
static void CeVideoSaveConfig(void)
{
    CeConfigSetInt("VideoScaleMode", (int)s_scaleMode);
    CeConfigSetInt("VideoNoSpriteLimit", s_noSpriteLimit);
    CeConfigSetInt("VideoFrameSkip", s_frameSkipMax);
    CeConfigSetInt("VideoDebugLog", s_debugLog);
    CeConfigSave();

    /* Apply before the CeLog() line below so turning logging ON from
     * this dialog captures its own "saved" line, and turning it OFF
     * suppresses it. */
    CeLogSetEnabled(s_debugLog);

    CeLog("CeVideoSaveConfig: saved scaleMode=%d noSpriteLimit=%d frameSkip=%d debugLog=%d",
          (int)s_scaleMode, s_noSpriteLimit, s_frameSkipMax, s_debugLog);
}

CeScaleMode CeVideoGetScaleMode(void)
{
    return s_scaleMode;
}

/* QuickNES's own libretro core-option keys (libretro/libretro_core_
 * options.h's option_defs_us[]) - unlike the PicoDrive/snes9x2002
 * template this file started from, there is no per-system "region"
 * option (NES/Famicom has no PAL/NTSC switch equivalent QuickNES
 * exposes), so only sprite-limit and the two audio-performance
 * overrides below are answered here; everything else (palette,
 * overscan, turbo, up_down_allowed, EQ preset) is left to the core's
 * own upstream defaults (return 0 falls through to them - see
 * ce_main.c's ce_environment()). */
int CeVideoEnvGetVariable(const char *key, const char **outValue)
{
    if (strcmp(key, "quicknes_no_sprite_limit") == 0)
    {
        *outValue = s_noSpriteLimit ? "enabled" : "disabled";
        return 1;
    }

    /* Performance defaults - no UI toggle for either of these (see this
     * file's header comment): this device's own audio path (ce_audio.c)
     * downmixes to mono in software regardless of what the core
     * renders, so there's no user-facing benefit to QuickNES's upstream
     * defaults here, only CPU cost.
     *
     * "linear" audio mode is documented by the core itself as the
     * explicit low-CPU choice (libretro_core_options.h: "reduces
     * quality but increases performance on low-end hardware"), instead
     * of upstream's own default "nonlinear". "stereo panning" (adds
     * reverb/echo/delay-variance DSP on top) is never selected here at
     * all - paying for a stereo effect only to fold it back down to
     * mono a moment later in ce_audio.c would be pure waste. */
    if (strcmp(key, "quicknes_audio_nonlinear") == 0)
    {
        *outValue = "linear";
        return 1;
    }

    /* "auto" would fall back to 48kHz on this build (no
     * RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE answer is wired up in
     * ce_main.c's ce_environment()) - force the lowest rate QuickNES
     * supports instead, to minimise both Blip_Buffer synthesis cost and
     * waveOut transfer volume. */
    if (strcmp(key, "quicknes_audio_samplerate") == 0)
    {
        *outValue = "32000";
        return 1;
    }

    /* Turbo A/Turbo B (X/Y - see ce_input.c/ce_res.rc's IDD_INPUTCONFIG)
     * only do anything inside the core when quicknes_turbo_enable says
     * so (libretro.cpp's update_input(): turbo_btn[]/turbomap[] are
     * still read every frame either way, but folded into pads[] only
     * when turbo_enabled[p] is set - default "none" means those two
     * buttons are otherwise dead weight, which is exactly what round 25
     * (user report) found). This frontend only ever drives port 0
     * (CeInputState() rejects any other port), so "player 1" is the
     * right scope - "both"/"player 2" would just be enabling turbo for
     * an input source nothing here ever feeds. quicknes_turbo_pulse_width
     * is left at the core's own upstream default ("3" frames on, 3 off -
     * a 60/(3+3)=10Hz autofire rate) since there's no CE UI for it yet;
     * see libretro_core_options.h for both options' full value lists. */
    if (strcmp(key, "quicknes_turbo_enable") == 0)
    {
        *outValue = "player 1";
        return 1;
    }

    if (strcmp(key, "quicknes_turbo_pulse_width") == 0)
    {
        *outValue = "3";
        return 1;
    }

    return 0;
}

int CeVideoConsumeDirty(void)
{
    int wasDirty = s_dirty;
    s_dirty = 0;
    return wasDirty;
}

/* Deliberately inert - see this file's header comment and ce_video.h.
 * Always "don't force" / no-op, since there is no s_frameSkip state to
 * consult any more; kept only so ce_main.c's existing call sites (safe
 * regardless, since g_audioBuffStatusCb never becomes non-NULL for this
 * core) don't need edits. */
int CeVideoFrameSkipShouldForceRender(void)
{
    return 0;
}

void CeVideoFrameSkipNotifyRendered(int rendered)
{
    (void)rendered;
}

/* See this function's own comment in ce_video.h. */
int CeVideoFrameSkipDecide(unsigned lastRetroRunMs, unsigned frameBudgetMs)
{
    int      active, underrunLikely;
    unsigned occupancyPercent;

    if (s_frameSkipMax <= 0)
    {
        s_frameSkipStreak = 0;
        return 1; /* Off - always render, matches pre-frame-skip behaviour */
    }

    if (s_frameSkipStreak >= (unsigned)s_frameSkipMax)
    {
        /* Ceiling already hit by however many consecutive skips this
         * dial allows - force a render regardless of how the signals
         * below read, same reasoning as the dead ShouldForceRender()'s
         * own cap-enforcement above (never let a game sit at a fraction
         * of its real frame rate indefinitely). */
        s_frameSkipStreak = 0;
        return 1;
    }

    CeAudioGetBufferStatus(&active, &occupancyPercent, &underrunLikely);
    (void)occupancyPercent;

    if ((active && underrunLikely) || (lastRetroRunMs > frameBudgetMs))
    {
        s_frameSkipStreak++;
        return 0;
    }

    s_frameSkipStreak = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Dialog                                                              */
/* ------------------------------------------------------------------ */

/* Display order for the Scale spinner (round 21, user request) - x1 =
 * CE_SCALE_1TO1, x1.5 = CE_SCALE_FULLSCREEN ("Full Screen 1:1"), Wide =
 * CE_SCALE_HALFSTRETCH ("Expand half"), Full = CE_SCALE_EXPAND. Cycles
 * with wraparound in both directions (unlike Frame Skip's 0..30 range
 * below, these are 4 unordered categorical choices, not a quantity with
 * a natural minimum/maximum), same left/right-only interaction as Sound
 * Config's Bits/Quality pairs. */
static const CeScaleMode kScaleOrder[4] = {
    CE_SCALE_1TO1, CE_SCALE_FULLSCREEN, CE_SCALE_HALFSTRETCH, CE_SCALE_EXPAND
};
static const wchar_t *kScaleLabels[4] = { L"x1", L"x1.5", L"Wide", L"Full" };
#define CE_SCALE_CHOICE_COUNT 4

static int ScaleModeToIndex(CeScaleMode m)
{
    int i;
    for (i = 0; i < CE_SCALE_CHOICE_COUNT; i++)
        if (kScaleOrder[i] == m)
            return i;
    return 0; /* fallback: x1 */
}

static void UpdateScaleLabel(HWND hDlg)
{
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE), kScaleLabels[ScaleModeToIndex(s_scaleMode)]);
}

static void StepScaleMode(HWND hDlg, int delta)
{
    int idx = (ScaleModeToIndex(s_scaleMode) + delta + CE_SCALE_CHOICE_COUNT) % CE_SCALE_CHOICE_COUNT;
    s_scaleMode = kScaleOrder[idx];
    UpdateScaleLabel(hDlg);
}

/* Frame Skip's value readout - "Off" for 0, otherwise the ceiling
 * itself (see CE_FRAMESKIP_MAX's comment). Clamps rather than wraps
 * (Left past 0 stays at 0, Right past the ceiling stays at the
 * ceiling) - a quantity with a real minimum/maximum, unlike Scale's 4
 * unordered categorical choices above, same distinction Sound Config's
 * own Volume spinner makes against its Bits/Quality pairs. */
static void UpdateFrameSkipLabel(HWND hDlg)
{
    wchar_t text[8];
    if (s_frameSkipMax <= 0)
        _snwprintf(text, 8, L"Off");
    else
        _snwprintf(text, 8, L"%d", s_frameSkipMax);
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_LABEL), text);
}

static void StepFrameSkip(HWND hDlg, int delta)
{
    int v = s_frameSkipMax + delta;
    if (v < 0) v = 0;
    if (v > CE_FRAMESKIP_MAX) v = CE_FRAMESKIP_MAX;
    s_frameSkipMax = v;
    UpdateFrameSkipLabel(hDlg);
}

/* IDC_VC_JAPANESE's own value readout (round 24, replacing a fixed
 * "English" CHECKBOX caption - see ce_resource.h). Originally shown as
 * romaji ("GAIKOKU-English"/"NIHON-Japanese") on the theory that this
 * button's caption used the OS-standard font, which couldn't render
 * Japanese glyphs unless jptahoma.ttc had loaded successfully - now that
 * this button is BS_OWNERDRAW and draws with the Shinonome bitmap font
 * (ce_bmpfont.c, baked into the binary - no load-failure case to guard
 * against any more), that constraint is gone, so this shows the actual
 * language name in both languages instead (ported from the sister
 * gnuboy CE project's own round-22 change). Same "leave it alone in
 * ApplyVideoConfigLanguage" treatment as the Scale/Frame Skip value
 * readouts (kScaleLabels, "Off"/a number) - see this dialog's own
 * ApplyVideoConfigLanguage. */
static void UpdateLanguageLabel(HWND hDlg)
{
    SetWindowTextW(GetDlgItem(hDlg, IDC_VC_JAPANESE),
                   CeLangIsJapanese() ? L"\x65e5\x672c\x8a9e" /* 日本語 */ : L"English");
}

static void  RefreshVideoConfigLanguage(HWND hDlg);
static void  ToggleLanguage(HWND hDlg);

/* Physical-key focus chain for this dialog: Scale -> Transparency ->
 * Frame Skip -> language spinner -> Debug Logging -> OK, wrapping
 * back to Scale - same technique and the same reason as an
 * earlier Brain port's MiscNeighbor/MiscCtrlProc:
 * this device's dialog manager doesn't reliably move focus with the
 * arrow keys between dissimilar control types, and always routes decide
 * (Enter) to the DEFPUSHBUTTON (OK) regardless of what's actually
 * focused unless a control claims WANTALLKEYS and handles it itself -
 * OK included, so it can participate in the wrap instead of being an
 * arrow-key dead end. */
#define WM_SETVIDEOFOCUS (WM_APP + 202)

static int VideoNeighborDown(int id)
{
    switch (id)
    {
    case IDC_VC_SCALE_VALUE:       return IDC_VC_TRANSPARENCY;
    case IDC_VC_TRANSPARENCY:      return IDC_VC_FRAMESKIP_LABEL;
    case IDC_VC_FRAMESKIP_LABEL:   return IDC_VC_JAPANESE;
    case IDC_VC_JAPANESE:          return IDC_VC_DEBUGLOG;
    case IDC_VC_DEBUGLOG:          return IDOK;
    case IDOK:                     return IDC_VC_SCALE_VALUE;
    }
    return id;
}

static int VideoNeighborUp(int id)
{
    switch (id)
    {
    case IDC_VC_SCALE_VALUE:       return IDOK;
    case IDC_VC_TRANSPARENCY:      return IDC_VC_SCALE_VALUE;
    case IDC_VC_FRAMESKIP_LABEL:   return IDC_VC_TRANSPARENCY;
    case IDC_VC_JAPANESE:          return IDC_VC_FRAMESKIP_LABEL;
    case IDC_VC_DEBUGLOG:          return IDC_VC_JAPANESE;
    case IDOK:                     return IDC_VC_DEBUGLOG;
    }
    return id;
}

static WNDPROC s_pVideoOrigProc = NULL;

/* Subclasses the four scale radios, Transparency (No Sprite Limit),
 * the language spinner, Debug Logging and OK - claims every
 * key unconditionally (DLGC_WANTARROWS | DLGC_WANTALLKEYS), same blanket
 * approach as the earlier port's MiscCtrlProc/SoundCtrlProc for the reasons given
 * in VideoNeighborDown's comment above. Unclaimed keys still fall
 * through to the native BUTTON control via CallWindowProc at the
 * bottom. */
static LRESULT CALLBACK VideoCtrlProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    int id = GetDlgCtrlID(hWnd);

    if (message == WM_GETDLGCODE)
    {
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
    }
    else if (message == WM_KEYDOWN)
    {
        switch (wParam)
        {
        case VK_UP:
            SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborUp(id)));
            return 0;

        case VK_DOWN:
            SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborDown(id)));
            return 0;

        case VK_LEFT:
        case VK_RIGHT:
            if (id == IDC_VC_SCALE_VALUE)
            {
                StepScaleMode(GetParent(hWnd), (wParam == VK_LEFT) ? -1 : 1);
            }
            else if (id == IDC_VC_FRAMESKIP_LABEL)
            {
                StepFrameSkip(GetParent(hWnd), (wParam == VK_LEFT) ? -1 : 1);
            }
            else if (id == IDC_VC_JAPANESE)
            {
                /* Two-state toggle - Left and Right both just flip it,
                 * same reasoning as ToggleLanguage's own comment. */
                ToggleLanguage(GetParent(hWnd));
            }
            return 0;

        case VK_RETURN:
            if (id == IDC_VC_SCALE_VALUE || id == IDC_VC_FRAMESKIP_LABEL || id == IDC_VC_JAPANESE)
            {
                /* Spinner, not a toggle - decide just moves on, same as
                 * Sound Config's own value spinners. */
                SetFocus(GetDlgItem(GetParent(hWnd), VideoNeighborDown(id)));
            }
            else if (id == IDC_VC_TRANSPARENCY)
            {
                /* Flips the real data directly and repaints via
                 * InvalidateRect - not BM_GETCHECK/BM_SETCHECK, which
                 * stopped working as a checkbox once this control became
                 * BS_OWNERDRAW (ce_res.rc): RC's "CHECKBOX ..., BS_OWNERDRAW"
                 * bitwise-ORs BS_CHECKBOX(2) with BS_OWNERDRAW(0x0B) into
                 * the low nibble that holds the button *type*, so the
                 * resulting style is BS_OWNERDRAW alone - the checkbox
                 * identity is gone and BM_GETCHECK/BM_SETCHECK no longer
                 * do anything useful (real-hardware-confirmed by the
                 * sister gnuboy CE project). s_noSpriteLimit is the
                 * checkbox's real ground truth now; WM_DRAWITEM below
                 * reads it directly via CeBmpFontDrawOwnerCheckbox(). */
                s_noSpriteLimit = !s_noSpriteLimit;
                InvalidateRect(hWnd, NULL, TRUE);
            }
            else if (id == IDC_VC_DEBUGLOG)
            {
                /* Flip the real data; CeLogSetEnabled() is applied (and
                 * the value persisted) by the IDOK handler below. */
                s_debugLog = !s_debugLog;
                InvalidateRect(hWnd, NULL, TRUE);
            }
            else if (id == IDOK)
            {
                /* IDOK is subclassed too now (for the Up/Down wrap), so
                 * its own decide press has to be forwarded explicitly
                 * instead of falling through with no effect. */
                SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)hWnd);
            }
            return 0;

        case VK_ESCAPE:
            /* Claiming WANTALLKEYS above means this control, not the
             * dialog manager, now sees the physical Back key too -
             * without this it would silently do nothing while focus was
             * on one of these controls, instead of committing and
             * closing like OK does (see IDOK/IDCANCEL below). */
            SendMessage(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), (LPARAM)hWnd);
            return 0;
        }
    }

    return CallWindowProc(s_pVideoOrigProc, hWnd, message, wParam, lParam);
}

/* Fixed LTEXT captions repainted by WM_PAINT via CeBmpFontPaintLabel()
 * (Shinonome bitmap-font migration, ported from the sister gnuboy CE
 * project) - IDC_VC_LBL_SCALE ("Scale:") is untranslated in both
 * languages (see ApplyVideoConfigLanguage's own comment) but still needs
 * a hide+repaint now that OS-native STATIC rendering is gone.
 * IDC_VC_TRANSPARENCY/IDC_VC_DEBUGLOG are CHECKBOX, not LTEXT -
 * BS_OWNERDRAW (ce_res.rc) and redrawn via WM_DRAWITEM's
 * CeBmpFontDrawOwnerCheckbox() instead. */
static const int kVideoLabelIds[] = {
    IDC_VC_LBL_SCALE, IDC_VC_LBL_FRAMESKIP,
};
#define CE_VIDEO_LABEL_COUNT (sizeof(kVideoLabelIds) / sizeof(kVideoLabelIds[0]))

/* One-shot at WM_INITDIALOG, and again right after IDC_VC_JAPANESE is
 * toggled so this still-open dialog reflects the change immediately
 * instead of only the next time it's reopened. The "Scale:" caption and
 * the four scale-mode radio captions (1:1/Expand/Full screen 1:1/Expand
 * half) are deliberately left untranslated even in Japanese mode. */
static void ApplyVideoConfigLanguage(HWND hDlg)
{
    unsigned i;

    if (CeLangIsJapanese())
    {
        SetDlgItemTextW(hDlg, IDC_VC_TRANSPARENCY,   L"\x30b9\x30d7\x30e9\x30a4\x30c8\x5236\x9650\x89e3\x9664"); /* スプライト制限解除 */
        SetDlgItemTextW(hDlg, IDC_VC_LBL_FRAMESKIP,  L"\x30d5\x30ec\x30fc\x30e0\x30b9\x30ad\x30c3\x30d7\x3a"); /* フレームスキップ: */
        SetDlgItemTextW(hDlg, IDC_VC_DEBUGLOG,       L"\x30c7\x30d0\x30c3\x30b0\x30ed\x30b0\x3092\x6709\x52b9\x306b\x3059\x308b"); /* デバッグログを有効にする */
        SetDlgItemTextW(hDlg, IDOK,     L"\x6c7a\x5b9a");                            /* 決定 */
    }
    else
    {
        SetDlgItemTextW(hDlg, IDC_VC_TRANSPARENCY,   L"No Sprite Limit");
        SetDlgItemTextW(hDlg, IDC_VC_LBL_FRAMESKIP,  L"Frame Skip:");
        SetDlgItemTextW(hDlg, IDC_VC_DEBUGLOG,       L"Enable Debug Logging");
        SetDlgItemTextW(hDlg, IDOK,     L"OK");
    }

    for (i = 0; i < CE_VIDEO_LABEL_COUNT; i++)
        ShowWindow(GetDlgItem(hDlg, kVideoLabelIds[i]), SW_HIDE);
    InvalidateRect(hDlg, NULL, TRUE);
}

/* Re-run every time the language spinner is toggled ("-"/"+" tap or
 * decide-key path, both below) - also refreshes IDC_VC_JAPANESE's own
 * value text via UpdateLanguageLabel(). */
static void RefreshVideoConfigLanguage(HWND hDlg)
{
    UpdateLanguageLabel(hDlg);
    ApplyVideoConfigLanguage(hDlg);
}

/* Shared by the WM_COMMAND MINUS/PLUS handlers and the physical-key
 * Left/Right path in VideoCtrlProc below - a two-state toggle has no
 * real "-" vs. "+" direction, so both simply flip it, same as Frame
 * Skip's Up/Down naturally clamping instead of wrapping at the ends of
 * its own range. */
static void ToggleLanguage(HWND hDlg)
{
    CeLangSetJapanese(!CeLangIsJapanese());
    RefreshVideoConfigLanguage(hDlg);
}

static INT_PTR CALLBACK VideoConfigDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        /* No CheckDlgButton() here any more - IDC_VC_TRANSPARENCY/
         * IDC_VC_DEBUGLOG are BS_OWNERDRAW (ce_res.rc) and read
         * their checked state directly from s_noSpriteLimit/
         * s_debugLog at draw time (WM_DRAWITEM below),
         * not from BM_GETCHECK/BM_SETCHECK - see VideoCtrlProc's own
         * comment on why those stopped working once BS_OWNERDRAW was
         * added. */
        UpdateScaleLabel(hDlg);
        UpdateFrameSkipLabel(hDlg);
        RefreshVideoConfigLanguage(hDlg);

        s_pVideoOrigProc = (WNDPROC)GetWindowLongPtrW(GetDlgItem(hDlg, IDOK), GWLP_WNDPROC);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE),       GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_TRANSPARENCY),      GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_FRAMESKIP_LABEL),   GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_JAPANESE),          GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDC_VC_DEBUGLOG),          GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);
        SetWindowLongPtrW(GetDlgItem(hDlg, IDOK),                     GWLP_WNDPROC, (LONG_PTR)VideoCtrlProc);

        /* Belt-and-suspenders initial focus, same pattern as every other
         * dialog in this port (ce_fileopen.c's WM_SETLISTFOCUS is the
         * first/most-documented instance): a synchronous SetFocus() from
         * WM_INITDIALOG alone doesn't always stick on this device. */
        SetActiveWindow(hDlg);
        SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
        PostMessage(hDlg, WM_SETVIDEOFOCUS, 0, 0);
        return FALSE;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        if (dis->CtlID == IDC_VC_TRANSPARENCY)
            CeBmpFontDrawOwnerCheckbox(dis, s_noSpriteLimit);
        else if (dis->CtlID == IDC_VC_DEBUGLOG)
            CeBmpFontDrawOwnerCheckbox(dis, s_debugLog);
        else
            CeBmpFontDrawOwnerButton(dis);
        return TRUE;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hDlg, &ps);
        unsigned i;
        for (i = 0; i < CE_VIDEO_LABEL_COUNT; i++)
            CeBmpFontPaintLabel(hdc, hDlg, kVideoLabelIds[i]);
        EndPaint(hDlg, &ps);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE)
        {
            SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
            PostMessage(hDlg, WM_SETVIDEOFOCUS, 0, 0);
        }
        break;

    case WM_SETVIDEOFOCUS:
        SetFocus(GetDlgItem(hDlg, IDC_VC_SCALE_VALUE));
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_VC_SCALE_MINUS:
            StepScaleMode(hDlg, -1);
            return TRUE;

        case IDC_VC_SCALE_PLUS:
            StepScaleMode(hDlg, 1);
            return TRUE;

        case IDC_VC_TRANSPARENCY:
            /* Explicit toggle of the real data, repainted via
             * InvalidateRect - see VideoCtrlProc's own comment on why
             * this doesn't touch BM_GETCHECK/BM_SETCHECK/CheckDlgButton
             * any more now that this control is BS_OWNERDRAW. */
            s_noSpriteLimit = !s_noSpriteLimit;
            InvalidateRect(GetDlgItem(hDlg, IDC_VC_TRANSPARENCY), NULL, TRUE);
            return TRUE;

        case IDC_VC_FRAMESKIP_DOWN:
            StepFrameSkip(hDlg, -1);
            return TRUE;

        case IDC_VC_FRAMESKIP_UP:
            StepFrameSkip(hDlg, 1);
            return TRUE;

        case IDC_VC_LANG_MINUS:
        case IDC_VC_LANG_PLUS:
            /* Same control on both IDs - see ToggleLanguage's comment
             * on why a two-state spinner has no real "-" vs. "+"
             * direction. IDC_VC_JAPANESE itself (the value readout in
             * between) intentionally has no case here, same as
             * IDC_VC_SCALE_VALUE above it - tapping the readout does
             * nothing, only "-"/"+" (or Left/Right when it has focus,
             * see VideoCtrlProc) change it. */
            ToggleLanguage(hDlg);
            return TRUE;

        case IDC_VC_DEBUGLOG:
            /* Explicit toggle of the real data; persisted and applied to
             * CeLogSetEnabled() by the IDOK handler below. */
            s_debugLog = !s_debugLog;
            InvalidateRect(GetDlgItem(hDlg, IDC_VC_DEBUGLOG), NULL, TRUE);
            return TRUE;

        case IDOK:
        case IDCANCEL:
            /* Physical Back (IDCANCEL) acts the same as touching OK
             * here - this device has no meaningful "discard changes"
             * gesture, only "go back", so both commit and close (same
             * philosophy as every settings dialog in an earlier Brain port).
             * Scale mode is already applied
             * live (ce_display.c reads it straight from
             * module state); this just persists everything to disk and
             * pokes the core to re-poll its two options. */
            CeVideoSaveConfig();
            s_dirty = 1;
            EndDialog(hDlg, LOWORD(wParam));
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
    return FALSE;
}

void CeShowVideoConfigDialog(HWND owner)
{
    DialogBoxW((HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE), MAKEINTRESOURCEW(IDD_VIDEOCONFIG),
               owner, VideoConfigDlgProc);
}
