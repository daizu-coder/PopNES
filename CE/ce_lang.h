/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Japanese/English UI text toggle - a persisted on/off flag consulted
 * by every dialog's own ApplyXLanguage() function (ce_main.c/
 * ce_input.c/ce_audio.c/ce_video.c/ce_fileopen.c) plus the Shinonome
 * bitmap-font renderer (ce_bmpfont.c/.h, CE/ce_shinonome16.h) that
 * actually draws the text.
 *
 * This file used to also own a bundled "jptahoma.ttc" TrueType font -
 * AddFontResourceW() on a background thread, plus a registry flag
 * guarding against repeat-registration corruption ("tofu" glyphs) on
 * every relaunch within the same power-on session (see this file's
 * revision history for that design). jptahoma.ttc was removed: its
 * provenance could not be verified, which left shipping it in a
 * redistributable build a licensing question best avoided. It's
 * replaced by
 * CeBmpFontDrawTextW() (ce_bmpfont.c) - a Shinonome bitmap font baked
 * into the binary at build time instead of loaded from a file at
 * runtime, so there's no load-failure case to guard against and no
 * font resource to leak across relaunches any more. See
 * THIRDPARTY_LICENSES.txt at the repository root for the font's
 * license text and author credit. Ported from the sister gnuboy CE
 * project's own post-migration ce_lang.c/.h.
 */
#ifndef CE_LANG_H
#define CE_LANG_H

/* Loads the persisted UILanguageJapanese flag from CeConfigLoad()'s
 * table - call once from WinMain, after CeConfigLoad(). */
void CeLangInit(void);

/* Persisted preference (CeConfigLoad()'s table, key "UILanguageJapanese"),
 * default Japanese (1, user request) on a fresh config file - still a
 * user-visible toggle either way (Video Config's language spinner,
 * ce_video.c), it just starts on Japanese instead of English now. */
int  CeLangIsJapanese(void);
void CeLangSetJapanese(int japanese);

#endif
