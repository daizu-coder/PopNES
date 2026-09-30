/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Per-port identity macros for the shared CE app template.
 *
 * This is the one file a new port is expected to edit before anything
 * else. Everything here used to be hardcoded (picodrive/snes9x literals
 * scattered across ce_main.c/ce_log.c/ce_config.c/ce_lang.c) in the two
 * real ports this template was distilled from - pulled into one place so
 * a new port only needs to touch one file to rebrand the shell.
 *
 * Fill in all seven macros below for your emulator, then leave the rest
 * of the CE/ frontend alone.
 */
#ifndef CE_APP_CONFIG_H
#define CE_APP_CONFIG_H

/* Human-readable name shown in message boxes, the title bar, and the
 * "no ROM loaded" placeholder window title. Keep it short - it's
 * concatenated with a filename in the title bar (see CE_APP_TITLE_FMT
 * usage in ce_main.c) on a 320px-wide screen. */
#define CE_APP_TITLE        L"PopNES"

/* Window class name. Must be unique per app on the device - reused as
 * the FindWindowW() target for single-instance detection, so two ports
 * sharing this template must NOT share this string. */
#define CE_APP_WND_CLASS     L"PopNESWnd"

/* Named mutex for the single-instance check in WinMain(). Same
 * uniqueness requirement as CE_APP_WND_CLASS. */
#define CE_APP_MUTEX_NAME    L"PopNES_SingleInstance"

/* Config file, written next to AppMain.exe via GetModuleFileNameW() +
 * truncate-to-last-backslash (see CeConfigGetPath() in ce_config.c). */
#define CE_APP_CONFIG_FILENAME  L"popnes.cfg"

/* Debug log file, same directory convention as the config file (see
 * CeLogInit() in ce_log.c). */
#define CE_APP_LOG_FILENAME     L"popnes_debug.log"

/* HKCU registry subkey the language picker persists to (see ce_lang.c).
 * Keep the "Software\\" prefix. */
#define CE_APP_LANG_REGKEY   L"Software\\PopNES\\Lang"

/* Prefix libretro core-option keys are expected to use, e.g. a core
 * exposing "myemu_frameskip" would set this to L"myemu_". ce_video.c's
 * CeVideoEnvGetVariable() currently hardcodes the *specific* option
 * names PicoDrive's core happens to expose (picodrive_sprlim,
 * picodrive_frameskip, picodrive_region) rather than deriving them from
 * this prefix - that function's option list is inherently core-specific
 * and needs a manual pass against your own core's retro_variable[]
 * table (see libretro.c's retro_set_environment()), not a mechanical
 * find/replace. This macro exists as a documented placeholder for that
 * pass, not as something the current code consumes.
 *
 * NOTE: this is deliberately NOT rebranded to "popnes_" - it is the
 * key prefix the bundled QuickNES libretro core actually publishes
 * (quicknes_no_sprite_limit, quicknes_turbo_enable, ...). It must match
 * the core, not the app name. */
#define CE_APP_CORE_OPT_PREFIX  "quicknes_"

#endif /* CE_APP_CONFIG_H */
