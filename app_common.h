/*
 * app_common.h — shared HUD, power-management helpers
 *
 * Provides:
 *   • AppConfig  — brightness + auto-off timeout read from setup.txt
 *   • extern XPowersPMU power  — the single PMU owned by the sketch (.ino)
 *   • common_init / common_activity / common_tick  — idle auto-off
 *   • draw_battery_p / draw_watermark_p  — portrait (368×448, direct SH8601)
 *   • draw_battery_l / draw_watermark_l  — landscape (448×368, via Canvas)
 *   • draw_pill_label — hardware-button pill labels (rotated, anchored to buttons)
 *   • utf8_to_cp437 / printUtf8 — German Umlaut support
 */

#pragma once

#include <stdint.h>
#include "pin_config.h"          // must precede XPowersLib — defines XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#include "Arduino_GFX_Library.h"
#include "canvas/Arduino_Canvas.h"

// ── Display corner safe zone ─────────────────────────────────────────────────
#define CORNER_R        50    // AMOLED display corner radius in px

// ── Physical hardware button positions (portrait, right edge of device) ──────
#define BOOT_BTN_Y_P    90    // "talk" pill (portrait) — nudged up 5 px
#define PWR_BTN_Y_P    355    // "new"  pill (portrait) — nudged down 10 px
#define BOOT_BTN_X_L    95    // BOOT mapped to landscape x (rotation=1)
#define PWR_BTN_X_L    345    // PWR  mapped to landscape x (rotation=1)

// ── HUD colours ──────────────────────────────────────────────────────────────
#define HUD_COL_BAT  0x2104   // dim grey (battery icon)
#define HUD_COL_WMK  0x18C3   // very dim grey (watermark)
#define HUD_PILL_BG  0x1082   // pill background fill
#define HUD_PILL_BD  0xAD55   // pill border — medium-light grey
#define HUD_PILL_TX  0xAD55   // pill label text — medium-light grey

// ── Shared config (set by sketch after reading setup.txt) ────────────────────
struct AppConfig {
    uint16_t brightness;   // 0–255
    uint32_t timeout_s;    // idle seconds before auto power-off; 0 = disabled
};

extern AppConfig  g_config;   // defined in app_common.cpp
extern XPowersPMU power;      // defined in the sketch (.ino)

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void common_init();
void common_activity();
void common_tick();

// Arms the BOOT+PWR back-to-menu trigger inside common_tick(). Called by the
// launcher right before invoking an app's setup(); the launcher menu itself
// stays disarmed.
void common_enter_app();

// Persists the "show menu on next boot" flag and reboots. Used by the
// BOOT+PWR shortcut and any app that wants an explicit "exit to menu" action.
void common_exit_to_menu();

// PWR short-press consumer. common_tick() owns the AXP2101 IRQ status: it
// captures any short-press into a sticky flag and clears the IRQ register
// itself. Apps must NOT call power.getIrqStatus / power.clearIrqStatus —
// instead they call common_consume_pwr_short(), which returns true once
// per press and resets the flag.
//
// Why: if an app clears IRQ status itself, it can race with common_tick's
// long-press detection and silently wipe the LONG_PRESS flag, breaking the
// universal back-to-launcher gesture.
bool common_consume_pwr_short();

// Drop any pending PWR press — both the latched software flag and any AXP
// IRQ bits accumulated while the MCU was busy. Use after a long blocking
// call (e.g. an HTTP request) so a stray press during the wait doesn't
// register as an action when the app's loop resumes.
void common_drain_pwr();

// ── HUD helpers — portrait 368×448 (direct Arduino_SH8601) ───────────────────
void draw_battery_p   (Arduino_SH8601 *gfx);
void draw_watermark_p (Arduino_SH8601 *gfx);

// ── HUD helpers — landscape 448×368 (via Arduino_Canvas) ─────────────────────
void draw_battery_l   (Arduino_Canvas *canvas);
void draw_watermark_l (Arduino_Canvas *canvas);

// ── HUD helpers — generic (any Arduino_GFX subclass, explicit dimensions) ─────
void draw_battery_g   (Arduino_GFX *gfx, int16_t w, int16_t h);
void draw_watermark_g (Arduino_GFX *gfx, int16_t w, int16_t h);

// ── Pill-shaped hardware-button labels ───────────────────────────────────────
// rotation: 0 = portrait canvas, 1 = landscape canvas, 0xFF = portrait direct gfx
// button:   0 = BOOT, 1 = PWR
// action:   short label text, e.g. "next", "reset", "+1"
void draw_pill_label(Arduino_GFX *gfx, uint8_t rotation, uint8_t button,
                     const char *action);

// ── Rotated text (90° CW — reads top-to-bottom on right edge in portrait) ───
void drawTextRot(Arduino_GFX *gfx, int16_t x, int16_t y, const char *str,
                 uint16_t color, uint8_t sz, uint8_t pxSz = 0);

// ── Rotated text (90° CCW — reads bottom-to-top; anchor is bottom of block) ─
void drawTextRotCCW(Arduino_GFX *gfx, int16_t x, int16_t y, const char *str,
                    uint16_t color, uint8_t sz, uint8_t pxSz = 0);

// ── Antialiased primitives (RGB565 alpha blend over known bg colour) ────────
// `bg` is the colour the helper blends edge pixels against. Pass the actual
// background fill at that location — these helpers do NOT read pixels back.
uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha);
void fillCircleAA(Arduino_GFX *gfx, int16_t cx, int16_t cy, int16_t r,
                  uint16_t col, uint16_t bg);
void drawCircleAA(Arduino_GFX *gfx, int16_t cx, int16_t cy, int16_t r,
                  uint16_t col, uint16_t bg);

// ── MIC indicator pill (red, vertical, anchored bottom-left) ────────────────
// Apps that use the microphone must call this every frame to advertise the
// active mic. Reserved zone: x in [4, 32], y in [h-92, h-12]. UI must not
// draw in that strip.
void draw_mic_pill(Arduino_GFX *gfx, int16_t w, int16_t h);

// ── Screenshot — dump canvas framebuffer over USB serial ─────────────────────
// Called automatically inside common_tick().
// Send "SCREENSHOT\n" over USB serial to trigger a dump.
// Only works for canvas-based apps (rotation 0 or 1); no-op for direct-gfx apps.
extern Arduino_Canvas *g_canvas;   // owned by the sketch (.ino)

// ── UTF-8 → CP437 conversion for German Umlaute ─────────────────────────────
size_t utf8_to_cp437(char *dst, size_t cap, const char *src);
void   printUtf8(Arduino_GFX *gfx, const char *str);

// ── Shared WiFi connect helper ──────────────────────────────────────────────
// Tries each credential in order. Returns index of connected network,
// or -1 if none succeeded. Aborts a network early on WL_CONNECT_FAILED
// or WL_NO_SSID_AVAIL (bad password / AP not in range), so the caller
// always falls through to the next SSID instead of hanging on the
// perNetTimeoutMs wait.
struct WifiCred { const char *ssid; const char *pass; };
int wifi_try_connect(const WifiCred *list, int n, uint32_t perNetTimeoutMs = 15000);
