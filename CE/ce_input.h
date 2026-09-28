/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Physical keyboard -> SNES joypad input for the CE frontend. Polling-
 * based (GetAsyncKeyState), not message-based, so it works the same way
 * whether the app has keyboard focus quirks or not, and so the Input
 * Config dialog (remap-by-press) can reuse the exact same primitive.
 */
#ifndef CE_INPUT_H
#define CE_INPUT_H

#include <windows.h>
#include <stdint.h>

/* Loads any saved key mapping from the registry (falls back to the
 * built-in defaults for anything not saved yet). Call once from
 * WinMain before the first CeInputPoll(). */
void CeInputInit(void);

/* retro_input_poll_t hook: samples all mapped keys once per frame. */
void CeInputPoll(void);

/* retro_input_state_t hook: id is a RETRO_DEVICE_ID_JOYPAD_* constant. */
int16_t CeInputState(unsigned port, unsigned device, unsigned index, unsigned id);

/* Modal native dialog (IDD_INPUTCONFIG): click a button, then press the
 * physical key to bind to that SNES button. OK persists the mapping to
 * the registry; Cancel discards changes made in this dialog session. */
void CeShowInputConfigDialog(HWND owner);

/* True while a remap button click is waiting for a key press (see
 * BeginWaitForKey in ce_input.c). ce_main.c's WndProc uses this to log
 * messages that reach the *main* window during that window - diagnostic
 * for the still-undetected "\x6c7a\x5b9a" (Decide/OK) button: if its
 * WM_KEYDOWN (or anything else) is being routed to the main frame window
 * instead of the modal IDD_INPUTCONFIG dialog, InputConfigDlgProc's own
 * diagnostic logging would never see it, but this would. */
int CeInputIsWaitingForKeyRemap(void);

/* Call right before gameplay (re)starts after a modal dialog closed via
 * the physical decide/OK key - Main Menu's Load State (most visibly,
 * since it resumes immediately) but really any decide-key dismissal,
 * including Open ROM and Resume/Cancel, and the process-restart
 * command-line hand-off (see ce_main.c's RestartProcess). Makes
 * CeInputPoll() report Start as not-pressed until the key is actually
 * observed released at least once, so a physical decide-key press still
 * in progress at the moment polling resumes can't be misread as a fresh
 * Start press and trigger the game's own in-game pause. Same root cause
 * and fix as the sister smsCE project's g_suppress_start_key
 * (confirmed on real hardware there); this port's default Start binding
 * ('M') differs from Decide's (VK_RETURN), so the collision only bites
 * when the user has rebound Start to the decide key themselves, but the
 * underlying "input polling was frozen while the dialog was up, so
 * there's no debounce history to filter a still-held key" mechanism is
 * identical regardless of which physical key is involved. */
void CeInputSuppressStartKey(void);

#endif
