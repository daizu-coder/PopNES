/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
#ifndef CE_RESOURCE_H
#define CE_RESOURCE_H

#define IDI_MAIN 100

/* Touch-to-reveal "menu" dialog (CE/ce_res.rc's IDD_MAINMENU): plain
 * DialogBoxW + PUSHBUTTON controls, not a real HMENU - this coredll
 * genuinely does not export SetMenu (checked with nm; CreateMenu/
 * AppendMenuW/DrawMenuBar exist but SetMenu/GetMenu don't - a real
 * limitation of this CE profile, not a mistake), and the Pocket-PC-style
 * alternative (SHCreateMenuBar) is both a different visual paradigm
 * (bottom command bar, not a desktop-style dropdown) and, given this
 * device's gx.dll/aygshell.dll are bundled as C++-mangled-export
 * binaries, an unverified risk. DialogBox is proven on this exact
 * hardware already (GetOpenFileNameW, the previous CE project's own
 * settings screens).
 *
 * Layout matches the sister smsCE project's IDD_MAINMENU control-for-
 * control (same rects, same 210x140 dialog size): Open ROM full width
 * (WS_GROUP - the one thing that makes Up/Down *wrap* at the first/last
 * button on this device, plain sequential move already works without
 * it), Save/Load State as a 2-up row, the three config dialogs as a
 * 3-up row (in the same left-to-right slot smsCE's Misc/Sound/Keys
 * occupy - Video Cfg is this project's "Misc" analogue, it's where the
 * language toggle lives, same reasoning as IDC_VC_JAPANESE below), Exit
 * full width, and a footer hint line instead of a separate Resume
 * button/About button - resuming is just the physical Back key
 * (IDCANCEL), matching smsCE exactly. */
#define IDD_MAINMENU      3000
#define IDC_MM_OPEN       3002
#define IDC_MM_EXIT       3003
#define IDC_MM_INPUT      3004
#define IDC_MM_SOUND      3005
#define IDC_MM_VIDEO      3006
#define IDC_MM_SAVESTATE  3008
#define IDC_MM_LOADSTATE  3009
#define IDC_MM_HINT       3010

/* Screenshot button (ce_main.c's CeSaveScreenshot, ported from the
 * sister PopSNES port, which uses 3011; 3011 was held here by a
 * since-removed font-comparison test) and the colorful main menu's mascot bitmap (CE/popnes_mascot.bmp,
 * drawn by MainMenuDlgProc's WM_PAINT). */
#define IDC_MM_SCREENSHOT 3012
#define IDB_MAINMENU      3013

/* Input Config dialog (CE/ce_res.rc's IDD_INPUTCONFIG): one PUSHBUTTON
 * per SNES joypad button, showing the currently-bound key - click it,
 * then press the physical key to bind (see ce_input.c). Native controls
 * again, same reasoning as IDD_MAINMENU above - a plain grid of
 * PUSHBUTTONs, so (like IDD_MAINMENU) it just needs WS_GROUP on the
 * first one for Up/Down wraparound, no per-control subclassing. No
 * Cancel button (see IDD_SOUNDCONFIG's comment on why) - only OK, and
 * the physical Back key (IDCANCEL) does the same thing OK does. No
 * Reset to Defaults button either (round 21, user request) - removed
 * along with its now-unused IDC_IC_RESET id. */
#define IDD_INPUTCONFIG   3100
#define IDC_IC_BTN_UP     3101
#define IDC_IC_BTN_DOWN   3102
#define IDC_IC_BTN_LEFT   3103
#define IDC_IC_BTN_RIGHT  3104
#define IDC_IC_BTN_SELECT 3105
#define IDC_IC_BTN_START  3106
#define IDC_IC_BTN_A      3107
#define IDC_IC_BTN_B      3108
#define IDC_IC_BTN_X      3109
#define IDC_IC_BTN_Y      3110
#define IDC_IC_BTN_L      3111
#define IDC_IC_BTN_R      3112

/* Diagonal D-pad combos (ported from the sister picodrive-master CE
 * project's own Input Config, per user request): the one physical key
 * bound to each of these rows drives two directions at once - see
 * ce_input.c's CeInputMapEntry.idB and CeInputPoll(). Same ctrlId
 * numbering picodrive-master used for the equivalent four rows. */
#define IDC_IC_BTN_UPRIGHT   3113
#define IDC_IC_BTN_RIGHTDOWN 3114
#define IDC_IC_BTN_DOWNLEFT  3115
#define IDC_IC_BTN_LEFTUP    3116

/* Row captions on the left column (Up/Down/Left/Right/Select/Start) -
 * these were anonymous (-1) LTEXTs until the Japanese UI toggle needed
 * to address them individually via SetDlgItemTextW. The right column
 * (L/R dropped from this dialog entirely, round 25) translates in
 * Japanese mode: A/B -> fullwidth Ａ/Ｂ, TurboA/TurboB -> 連射Ａ/連射Ｂ.
 * The four diagonal-combo rows (Up R/R Down/Down L/L Up) translate too
 * (右上/右下/左下/左上). All are restored to their ASCII .rc text on a
 * toggle back to English. Either way the Shinonome bitmap-font migration
 * (below) needs a real control ID on *every* LTEXT regardless of
 * translation, since CeBmpFontPaintLabel() repaints a hidden STATIC via
 * GetDlgItem() - a plain "-1" LTEXT can't be addressed individually any
 * more. Ported from the sister gnuboy CE project's own round of new IDs
 * for the equivalent fixed labels. */
#define IDC_IC_LBL_UP     3120
#define IDC_IC_LBL_DOWN   3121
#define IDC_IC_LBL_LEFT   3122
#define IDC_IC_LBL_RIGHT  3123
#define IDC_IC_LBL_SELECT 3124
#define IDC_IC_LBL_START  3125
#define IDC_IC_LBL_A          3126
#define IDC_IC_LBL_B          3127
#define IDC_IC_LBL_TURBOA     3128
#define IDC_IC_LBL_TURBOB     3129
#define IDC_IC_LBL_UPRIGHT    3130
#define IDC_IC_LBL_RIGHTDOWN  3131
#define IDC_IC_LBL_DOWNLEFT   3132
#define IDC_IC_LBL_LEFTUP     3133

/* Sound Config dialog (CE/ce_res.rc's IDD_SOUNDCONFIG) - see
 * ce_audio.c. Volume/Rate/Bits/Quality are all "-/value/+" spinners now
 * (ported from the sister smsCE project's own Sound Settings dialog),
 * not a COMBOBOX + RADIOBUTTON pairs: the value lives on a WS_TABSTOP
 * PUSHBUTTON in the middle so it gets a native focus rectangle and
 * participates in the same physical-key Up/Down/Left/Right loop as
 * every other control here (see SoundConfigDlgProc's WM_GETDLGCODE
 * subclassing) - the "-"/"+" buttons on either side are touch-only
 * (no WS_TABSTOP), matching smsCE's IDC_FRAMESKIP_MINUS/PLUS. No
 * Cancel button: this device has no meaningful "discard changes"
 * gesture, only "go back" - the physical Back key (IDCANCEL) commits
 * and closes exactly like OK does, same as every settings dialog in
 * smsCE (see SoundConfigDlgProc's IDOK/IDCANCEL handling). */
#define IDD_SOUNDCONFIG      3200
#define IDC_SC_VOLUME_MINUS  3202
#define IDC_SC_VOLUME_VALUE  3203
#define IDC_SC_VOLUME_PLUS   3220
#define IDC_SC_RATE_MINUS    3221
#define IDC_SC_RATE_VALUE    3222
#define IDC_SC_RATE_PLUS     3223
#define IDC_SC_BITS_MINUS    3224
#define IDC_SC_BITS_VALUE    3225
#define IDC_SC_BITS_PLUS     3226
#define IDC_SC_QUALITY_MINUS 3227
#define IDC_SC_QUALITY_VALUE 3228
#define IDC_SC_QUALITY_PLUS  3229

/* "Buffer:" spinner - ring buffer capacity (KB), re-added from the
 * shared CE app template (round 18 removed an earlier version). */
#define IDC_SC_BUFFER_MINUS  3230
#define IDC_SC_BUFFER_VALUE  3231
#define IDC_SC_BUFFER_PLUS   3232

/* Static captions - were anonymous (-1) LTEXTs, need real IDs for the
 * Japanese UI toggle to address them individually. */
#define IDC_SC_LBL_VOLUME    3210
#define IDC_SC_LBL_RATE      3211
#define IDC_SC_LBL_BITS      3212
#define IDC_SC_LBL_QUALITY   3213
#define IDC_SC_LBL_BUFFER    3214

/* Video Config dialog (CE/ce_res.rc's IDD_VIDEOCONFIG) - see
 * ce_video.c. Scale mode (1:1 / Full Screen 1:1 / Expand half / Expand -
 * see CeScaleMode in ce_video.h) is consumed directly by ce_display.c's
 * blit; "No Sprite Limit" is forwarded to the core via the standard
 * libretro core-options environment call (quicknes_no_sprite_limit)
 * rather than poking Settings.* directly, keeping ce_video.c a
 * frontend, not a core patch. Frame Skip does NOT use that same
 * GET_VARIABLE channel (QuickNES has no "*_frameskip" option key to
 * answer) - it drives a different, genuinely-implemented core
 * mechanism instead (RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, answered
 * from ce_main.c's ce_environment() using CeVideoFrameSkipDecide() -
 * see ce_video.h). Scale is a "-/value/+" spinner (round 21, replacing
 * a 2x2 radio-button grid, user request) cycling through 4 states
 * (displayed as x1/x1.5/Wide/Full - see ce_video.c's kScaleOrder/
 * kScaleLabels), same "-/value/+" pattern as Sound Config's Volume/
 * Rate/Bits/Quality and this dialog's own Frame Skip row - the value
 * lives on a WS_TABSTOP PUSHBUTTON so it gets a native focus rectangle
 * and a place in the physical-key Up/Down/Left/Right loop; "-"/"+" are
 * touch-only (no WS_TABSTOP). No Cancel button - see IDD_SOUNDCONFIG's
 * comment above. */
#define IDD_VIDEOCONFIG          3300
#define IDC_VC_SCALE_MINUS       3301
#define IDC_VC_SCALE_VALUE       3302
#define IDC_VC_SCALE_PLUS        3303
#define IDC_VC_TRANSPARENCY      3305

/* Static "Scale:" caption - was anonymous (-1), same reasoning as
 * IDD_INPUTCONFIG's newly-IDed fixed labels above (Shinonome bitmap-font
 * migration needs a real ID to repaint a hidden STATIC via
 * CeBmpFontPaintLabel()). Untranslated in both languages, same as
 * before. */
#define IDC_VC_LBL_SCALE         3304
#define IDC_VC_FRAMESKIP_LABEL   3306
#define IDC_VC_FRAMESKIP_DOWN    3307
#define IDC_VC_FRAMESKIP_UP      3308

/* Static "Frame Skip:" caption - distinct from IDC_VC_FRAMESKIP_LABEL
 * above (the live "Off"/1..30 readout). Translated in Japanese mode
 * (ce_video.c's ApplyVideoConfigLanguage), same treatment Sound
 * Config gives its own Rate:/Bits: captions - only the *value* readouts
 * (Off/a digit, kScaleLabels' x1/x1.5/Wide/Full, ...) are left
 * untranslated, same call smsCE made for its own output-rate radio
 * captions (11KHz/22KHz/44KHz). */
#define IDC_VC_LBL_FRAMESKIP     3309

/* Japanese/English UI toggle (see ce_lang.h) - lives here rather than a
 * new dialog because this one already has exactly this UI pattern (a
 * row of display options), and is this project's closest analogue to
 * smsCE's Misc dialog (which houses the same toggle plus Open Last
 * Folder below). Round 24 (user request) turned this from a CHECKBOX
 * captioned "English" into a third "-/value/+" spinner (same pattern
 * as Scale/Frame Skip above): the value readout (still IDC_VC_JAPANESE,
 * not renamed, for the same reason smsCE kept its own control as
 * IDC_JAPAN even after relabelling it) originally showed romaji
 * ("GAIKOKU-English"/"NIHON-Japanese") because this button's caption
 * used the OS-standard font, which couldn't render Japanese glyphs
 * unless jptahoma.ttc had loaded successfully. Now that this button is
 * BS_OWNERDRAW and draws with the Shinonome bitmap font (ce_bmpfont.c,
 * baked into the binary), it shows the actual language name
 * ("English"/"日本語") in both languages instead - see ce_video.c's
 * UpdateLanguageLabel. IDC_VC_LANG_MINUS/PLUS are the "-"/"+" touch
 * buttons flanking it; being a two-state toggle, both simply flip it,
 * same as Frame Skip's Up/Down at the 0/30 ends of its own range
 * naturally clamping instead of wrapping. */
#define IDC_VC_JAPANESE          3310
#define IDC_VC_LANG_MINUS        3312
#define IDC_VC_LANG_PLUS         3313

/* "Enable Debug Logging" checkbox (ce_video.c) - while unchecked (the
 * default) CeLog() is a no-op and no log file is created; see ce_log.h.
 * Same BS_OWNERDRAW checkbox pitfall as IDC_VC_TRANSPARENCY -
 * ce_video.c keeps s_debugLog as the source of
 * truth and paints it via CeBmpFontDrawOwnerCheckbox(). */
#define IDC_VC_DEBUGLOG          3314

/* Custom ROM picker (CE/ce_res.rc's IDD_FILEOPEN, CE/ce_fileopen.c) -
 * replaces GetOpenFileNameW(), which has no way to show Japanese folder/
 * file names (see ce_fileopen.c). Ported from the sister smsCE project's
 * own IDD_FILEOPEN/DLGFileOpen. */
#define IDD_FILEOPEN      3400
#define IDC_FO_PATH       3401
#define IDC_FO_LIST       3402

/* Generic message box (CE/ce_res.rc's IDD_MSGBOX, CE/ce_main.c's
 * CeShowMsgBox()/MsgBoxDlgProc) - ported from the sister gnuboy CE
 * project. MessageBoxW() draws with whatever system font Windows CE
 * finds, and this device has no CJK-capable one any more now that
 * jptahoma.ttc is gone; this dialog instead paints its own text with
 * the Shinonome bitmap font, the same way every other piece of
 * Japanese UI text in this port already does. Used only for the
 * Save/Load State result messages (CE/ce_main.c) - the only
 * MessageBoxW() calls in this port that ever carried Japanese text;
 * every other MessageBoxW() call (ROM load failures, fatal startup
 * errors) is English-only and stays a plain MessageBoxW(). */
#define IDD_MSGBOX        3500
#define IDC_MB_TEXT       3501

/* Save State confirmation dialog (CE/ce_res.rc's IDD_SAVECONFIRM,
 * CE/ce_main.c's SaveConfirmDlgProc/CeConfirmSaveState) - shown before
 * Save State overwrites its single "<romPath>.state" slot. A "セーブ
 * しますか？"/"Save State?" label plus two BS_OWNERDRAW はい/いいえ
 * (Yes/No) buttons; same owner-draw + Shinonome bitmap-font treatment
 * as IDD_MSGBOX. Ported from the sister gnuboy CE project's IDD_CONFIRM
 * (this replaced the shared CE app template's "-"/はい|いいえ/"+"
 * spinner - user request to match gnuboy's dialog). */
#define IDD_SAVECONFIRM   3600
#define IDC_SF_TEXT       3601
#define IDC_SF_YES        3602
#define IDC_SF_NO         3603

#endif
