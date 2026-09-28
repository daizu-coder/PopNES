/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Part of PopNES, a QuickNES-core port for the SHARP Brain PW-G5200.
 * This file is MIT (CE/LICENSE); PopNES as a whole (AppMain.exe) is
 * distributed under GPL-2.0 - see CE/LICENSING.md and
 * CE/THIRDPARTY_LICENSES.txt.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "ce_log.h"
#include "ce_app_config.h"

/* Every previous log line only carried a single build-date/time stamp
 * printed once at WinMain start - individual lines had no time
 * information at all, making it impossible to tell how far apart two
 * events were, or how long a glitch (e.g. an audio underrun burst)
 * lasted. Prefixed here with seconds.milliseconds elapsed since this
 * process's first CeLog() call (effectively process start), monotonic
 * and cheap (GetTickCount(), no float/RTC dependency) - enough to
 * correlate audio-thread events against each other and against
 * ROM-switch process restarts (round 11) without needing wall-clock
 * time.
 *
 * Round 36 fix: the format string below prints elapsedMs/1000 (whole
 * seconds) and elapsedMs%1000 (millisecond remainder) as "X.XXX", which
 * is a seconds.milliseconds value - but the original label read "ms",
 * making every line look 1000x shorter than it really was (e.g.
 * "[+ 367.020ms]" actually meant 367.020 seconds - about 6 minutes -
 * into the run, not 367 milliseconds). Caught by cross-checking this
 * prefix against an audio underrun burst's own independently-computed
 * "after Nms" duration (ce_audio.c) in a real hardware log: the two
 * disagreed by exactly a factor of 1000. Purely a display label bug -
 * the value itself was always correct seconds.milliseconds - but left
 * uncorrected it makes every log misleading to read by 3 orders of
 * magnitude. */
static DWORD s_logStartTick;
static int   s_logStarted = 0;

/* Off by default - see ce_log.h. Video Config's "Enable Debug Logging"
 * checkbox (persisted as "VideoDebugLog") turns it on; WinMain applies
 * the persisted value once right after CeConfigLoad(). While disabled,
 * CeLog() returns before touching the filesystem - no log file is
 * created or appended. */
static int s_enabled = 0;

void CeLogSetEnabled(int enabled)
{
    s_enabled = enabled ? 1 : 0;
}

int CeLogIsEnabled(void)
{
    return s_enabled;
}

void CeLog(const char *fmt, ...)
{
    wchar_t exePath[MAX_PATH];
    wchar_t logPath[MAX_PATH];
    wchar_t *slash;
    char msg[512];
    va_list ap;
    FILE *f;
    DWORD elapsedMs;

    if (!s_enabled)
        return;

    if (!s_logStarted)
    {
        s_logStartTick = GetTickCount();
        s_logStarted = 1;
    }
    elapsedMs = GetTickCount() - s_logStartTick;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);

    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    slash = wcsrchr(exePath, L'\\');
    if (slash)
        *slash = L'\0';
    _snwprintf(logPath, MAX_PATH, L"%s\\" CE_APP_LOG_FILENAME, exePath);

    f = _wfopen(logPath, L"a");
    if (!f)
        return;
    fprintf(f, "[+%8lu.%03lus] %s\n", (unsigned long)(elapsedMs / 1000), (unsigned long)(elapsedMs % 1000), msg);
    fclose(f);
}
