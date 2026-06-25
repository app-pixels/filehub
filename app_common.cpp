/*
 * app_common.cpp — shared HUD and power-management implementation
 *
 * Corner radius on the SH8601 AMOLED is ~50 px.
 * Safe-zone rule (conservative):
 *   No UI elements where BOTH x < 50 AND y < 50 (or mirrored corners).
 *   The exclusion is circular: dist from corner centre < 50 px.
 *
 * Battery HUD — 3-bar monochrome icon, top-right, outside R=50 zone.
 * Watermark   — "www.app-pixels.com" centred at bottom.
 * Pill labels — anchored to physical button positions, 90° rotated in portrait.
 */

#include "app_common.h"
#include "pin_config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include <stdio.h>
#include <WiFi.h>
#include "HWCDC.h"
#include "font/glcdfont.h"    // 5×7 bitmap font for rotated text

extern USBCDC USBSerial;

// ── Global config (defaults; overwritten by launcher from setup.txt) ──────────
AppConfig g_config = { 200, 300 };

// ── Idle-timeout state ────────────────────────────────────────────────────────
static uint32_t s_lastActivity = 0;

// ── Back-to-menu state ────────────────────────────────────────────────────────
static bool s_armBackToMenu = false;   // true once an app has been entered

// ── PWR IRQ ownership ─────────────────────────────────────────────────────────
// common_tick() reads & clears the AXP2101 IRQ status itself, latching any
// short-press into this sticky flag for the running app to consume via
// common_consume_pwr_short(). This avoids a race where an app's own
// clearIrqStatus() wipes the LONG_PRESS bit before common_tick can act on it.
static volatile bool s_pwrShortPending = false;

bool common_consume_pwr_short() {
    if (s_pwrShortPending) {
        s_pwrShortPending = false;
        return true;
    }
    return false;
}

void common_drain_pwr() {
    s_pwrShortPending = false;
    power.getIrqStatus();
    power.clearIrqStatus();
}

void common_init() {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    s_lastActivity = millis();
}

void common_activity() {
    s_lastActivity = millis();
}

void common_enter_app() {
    s_armBackToMenu = true;
    s_lastActivity  = millis();
}

void common_exit_to_menu() {
    Preferences p;
    p.begin("launcher", false);
    p.putBool("gotoMenu", true);
    p.end();
    delay(50);
    ESP.restart();
}

// ── Screenshot over USB serial ───────────────────────────────────────────────
static char s_serialBuf[16];
static uint8_t s_serialIdx = 0;

static void screenshot_check() {
    while (USBSerial.available()) {
        char c = (char)USBSerial.read();
        if (c == '\n' || c == '\r') {
            s_serialBuf[s_serialIdx] = '\0';
            if (strcmp(s_serialBuf, "SCREENSHOT") == 0 && g_canvas) {
                // Framebuffer is always stored in native orientation (LCD_WIDTH x LCD_HEIGHT).
                // Send native dims + rotation so the receiver can rotate.
                uint16_t w = LCD_WIDTH;   // 368
                uint16_t h = LCD_HEIGHT;  // 448
                uint8_t  rot = g_canvas->getRotation();
                uint16_t *fb = g_canvas->getFramebuffer();
                // Header: magic(4) + width(2) + height(2) + rotation(1) = 9 bytes
                USBSerial.write((const uint8_t *)"SCRN", 4);
                USBSerial.write((const uint8_t *)&w, 2);
                USBSerial.write((const uint8_t *)&h, 2);
                USBSerial.write(&rot, 1);
                // Pixel data: RGB565 little-endian
                size_t total = (size_t)w * h * 2;
                const uint8_t *p = (const uint8_t *)fb;
                size_t sent = 0;
                while (sent < total) {
                    size_t chunk = total - sent;
                    if (chunk > 4096) chunk = 4096;
                    USBSerial.write(p + sent, chunk);
                    USBSerial.flush();
                    sent += chunk;
                }
            }
            s_serialIdx = 0;
        } else if (s_serialIdx < sizeof(s_serialBuf) - 1) {
            s_serialBuf[s_serialIdx++] = c;
        } else {
            s_serialIdx = 0;  // overflow, reset
        }
    }
}

void common_tick() {
    screenshot_check();

    // ── Back-to-menu shortcut ──────────────────────────────────────────────────
    // Universal gesture: long-press PWR (≥1.5 s, AXP2101 default). Backup:
    // BOOT held + PWR short-press. We own the AXP IRQ status here — read
    // once per tick, clear once per tick, and stash the short-press for
    // the app to consume via common_consume_pwr_short().
    // PWR-IRQ handling. The latch (s_pwrShortPending) must always run so
    // standalone apps — which never call common_enter_app() — can still read
    // short-presses via common_consume_pwr_short(). Only the exit-to-menu
    // actions are gated on s_armBackToMenu, since they only make sense from
    // within the launcher.
    power.getIrqStatus();
    bool pwrLong  = power.isPekeyLongPressIrq();
    bool pwrShort = power.isPekeyShortPressIrq();
    bool boot     = (digitalRead(0) == LOW);
    power.clearIrqStatus();

    if (s_armBackToMenu) {
        if (pwrLong) {
            common_exit_to_menu();
            return;
        }
        if (boot && pwrShort) {
            common_exit_to_menu();
            return;
        }
    }
    if (pwrShort) {
        s_pwrShortPending = true;
    }

    if (g_config.timeout_s == 0) return;
    if (millis() - s_lastActivity >= (uint32_t)g_config.timeout_s * 1000UL) {
        power.shutdown();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// UTF-8 → CP437 conversion for German Umlaute
// ═════════════════════════════════════════════════════════════════════════════

// ── German umlauts as canonical ASCII transliteration ──────────────────────
// The default 5×7 GLCD font has CP437 glyph slots for ä/ö/ü/ß, but on this
// build chain the high-bit chars don't reliably render. Use the standard
// German transliteration (ä→ae, ö→oe, ü→ue, ß→ss) so the text is always
// readable regardless of font.
static void emit2(char *dst, size_t &di, size_t cap, char a, char b) {
    if (di < cap - 1) dst[di++] = a;
    if (di < cap - 1) dst[di++] = b;
}

size_t utf8_to_cp437(char *dst, size_t cap, const char *src) {
    size_t di = 0;
    for (const char *p = src; *p && di < cap - 1; ) {
        uint8_t b = (uint8_t)*p;
        if (b == 0xC3 && p[1]) {
            uint8_t b2 = (uint8_t)p[1];
            switch (b2) {
                case 0xA4: emit2(dst, di, cap, 'a', 'e'); break; // ä
                case 0xB6: emit2(dst, di, cap, 'o', 'e'); break; // ö
                case 0xBC: emit2(dst, di, cap, 'u', 'e'); break; // ü
                case 0x84: emit2(dst, di, cap, 'A', 'e'); break; // Ä
                case 0x96: emit2(dst, di, cap, 'O', 'e'); break; // Ö
                case 0x9C: emit2(dst, di, cap, 'U', 'e'); break; // Ü
                case 0x9F: emit2(dst, di, cap, 's', 's'); break; // ß
                default:   if (di < cap - 1) dst[di++] = '?';     break;
            }
            p += 2;
        } else if (b == 0xC2 && p[1]) {
            // C2 xx — Latin-1 supplement (degree sign etc.)
            dst[di++] = (char)(uint8_t)p[1];
            p += 2;
        } else if (b < 0x80) {
            dst[di++] = *p++;
        } else {
            // skip unknown multi-byte sequence
            p++;
            while (*p && ((uint8_t)*p & 0xC0) == 0x80) p++;
            if (di < cap - 1) dst[di++] = '?';
        }
    }
    dst[di] = '\0';
    return di;
}

void printUtf8(Arduino_GFX *gfx, const char *str) {
    char buf[256];
    utf8_to_cp437(buf, sizeof(buf), str);
    gfx->print(buf);
}

// ═════════════════════════════════════════════════════════════════════════════
// Shared WiFi connect — iterates fallback credentials with early-fail
// ═════════════════════════════════════════════════════════════════════════════

int wifi_try_connect(const WifiCred *list, int n, uint32_t perNetTimeoutMs) {
    for (int i = 0; i < n; i++) {
        const char *ssid = list[i].ssid;
        if (!ssid || !ssid[0]) continue;
        const char *pass = list[i].pass ? list[i].pass : "";

        WiFi.disconnect(true, true);   // drop any cached credentials
        delay(50);
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, pass);

        uint32_t start = millis();
        while (millis() - start < perNetTimeoutMs) {
            wl_status_t st = WiFi.status();
            if (st == WL_CONNECTED) return i;
            // Early-fail states — move on to next SSID immediately.
            if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) break;
            delay(200);
        }
        WiFi.disconnect(true, true);
        delay(100);
    }
    return -1;
}

// ═════════════════════════════════════════════════════════════════════════════
// Rotated text — 90° CW (reads top-to-bottom on the right edge in portrait)
// ═════════════════════════════════════════════════════════════════════════════

void drawTextRot(Arduino_GFX *gfx, int16_t x, int16_t y, const char *str,
                 uint16_t color, uint8_t sz, uint8_t pxSz) {
    // 90° CW rotation: text reads top-to-bottom.
    // sz  = stride (spacing between font pixels)
    // pxSz = drawn pixel size (0 = same as sz)
    if (pxSz == 0) pxSz = sz;
    int16_t curY = y;
    const int16_t glyphW = 8 * (int16_t)sz;
    for (const char *p = str; *p; p++) {
        uint8_t ch = (uint8_t)*p;
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t bits = pgm_read_byte(&font[ch * 5 + col]);
            for (uint8_t row = 0; row < 8; row++) {
                if (bits & (1 << row)) {
                    int16_t px = x + (glyphW - 1) - row * (int16_t)sz;
                    int16_t py = curY + col * (int16_t)sz;
                    if (pxSz <= 1) gfx->drawPixel(px, py, color);
                    else           gfx->fillRect(px, py, pxSz, pxSz, color);
                }
            }
        }
        curY += 6 * (int16_t)sz;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Rotated text — 90° CCW (reads bottom-to-top; anchor (x,y) is bottom-left)
// ═════════════════════════════════════════════════════════════════════════════

void drawTextRotCCW(Arduino_GFX *gfx, int16_t x, int16_t y, const char *str,
                    uint16_t color, uint8_t sz, uint8_t pxSz) {
    if (pxSz == 0) pxSz = sz;
    int16_t curY = y;
    for (const char *p = str; *p; p++) {
        curY -= 6 * (int16_t)sz;
        uint8_t ch = (uint8_t)*p;
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t bits = pgm_read_byte(&font[ch * 5 + col]);
            for (uint8_t row = 0; row < 8; row++) {
                if (bits & (1 << row)) {
                    int16_t px = x + row * (int16_t)sz;
                    int16_t py = curY + (4 - col) * (int16_t)sz;
                    if (pxSz <= 1) gfx->drawPixel(px, py, color);
                    else           gfx->fillRect(px, py, pxSz, pxSz, color);
                }
            }
        }
    }
}

// Horizontal pixel-art text: stride and pixel size decoupled
static void drawTextPx(Arduino_GFX *gfx, int16_t x, int16_t y, const char *str,
                       uint16_t color, uint8_t stride, uint8_t pxSz) {
    int16_t curX = x;
    for (const char *p = str; *p; p++) {
        uint8_t ch = (uint8_t)*p;
        for (uint8_t col = 0; col < 5; col++) {
            uint8_t bits = pgm_read_byte(&font[ch * 5 + col]);
            for (uint8_t row = 0; row < 8; row++) {
                if (bits & (1 << row)) {
                    int16_t px = curX + col * (int16_t)stride;
                    int16_t py = y + row * (int16_t)stride;
                    if (pxSz <= 1) gfx->drawPixel(px, py, color);
                    else           gfx->fillRect(px, py, pxSz, pxSz, color);
                }
            }
        }
        curX += 6 * (int16_t)stride;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Pill-shaped hardware-button labels
// ═════════════════════════════════════════════════════════════════════════════

void draw_pill_label(Arduino_GFX *gfx, uint8_t rotation, uint8_t button,
                     const char *action) {
    int16_t len = (int16_t)strlen(action);
    if (len == 0) return;

    // Pixel-separated text: stride=2 (sz=2 dimensions), pixel=1 (airy pixel-art)
    const uint8_t stride = 2;
    const uint8_t pxSz   = 1;
    const int16_t charW  = 6 * stride;   // 12 px per char cell
    const int16_t charH  = 8 * stride;   // 16 px per char cell
    const int16_t padX   = 4;
    const int16_t padY   = 4;

    if (rotation == 1) {
        // ── Landscape: horizontal pill on TOP edge ──────────────────────
        int16_t pillW = len * charW + padX * 2;
        int16_t pillH = charH + padY * 2;
        int16_t rad   = pillH / 2;
        int16_t btnX  = (button == 0) ? BOOT_BTN_X_L : PWR_BTN_X_L;
        int16_t px    = btnX - pillW / 2;
        int16_t py    = 4;

        gfx->fillRoundRect(px, py, pillW, pillH, rad, 0x0000);   // black pill bg — keeps label readable over any UI
        drawTextPx(gfx, px + padX, py + padY, action, HUD_PILL_TX, stride, pxSz);
    } else {
        // ── Portrait (rot=0 or 0xFF): vertical pill on RIGHT edge ───────
        int16_t pillW = charH + padX * 2;
        int16_t pillH = len * charW + padY * 2;
        int16_t rad   = pillW / 2;
        int16_t btnY  = (button == 0) ? BOOT_BTN_Y_P : PWR_BTN_Y_P;
        int16_t px    = LCD_WIDTH - pillW - 4;
        int16_t py    = btnY - pillH / 2;

        gfx->fillRoundRect(px, py, pillW, pillH, rad, 0x0000);   // black pill bg — keeps label readable over any UI
        drawTextRot(gfx, px + padX, py + padY, action, HUD_PILL_TX, stride, pxSz);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Battery icon — 3-bar monochrome
// ═════════════════════════════════════════════════════════════════════════════

static int batteryBars() {
    if (!power.isBatteryConnect()) return -1;  // no battery
    int pct = (int)power.getBatteryPercent();
    if (pct > 70)  return 3;
    if (pct > 40)  return 2;
    if (pct > 15)  return 1;
    return 0;
}

// Draw battery icon at anchor (ax, ay).  Total footprint: 18×9 px.
//   ┌──────────┐─
//   │ ▮▮  ▮▮  ▮▮ │█   (nub on right)
//   └──────────┘─
static void drawBatteryAt(Arduino_GFX *gfx, int16_t ax, int16_t ay) {
    int bars = batteryBars();
    bool charging = power.isBatteryConnect() && power.isCharging();
    uint16_t col = HUD_COL_BAT;

    // Outline (14×9)
    gfx->drawRect(ax, ay, 14, 9, col);
    // Nub (2×5 on right side, centred vertically)
    gfx->fillRect(ax + 14, ay + 2, 2, 5, col);

    // 3 bars inside: each 3×5 px, at x offsets 2, 6, 10
    for (int i = 0; i < 3; i++) {
        if (bars > i) {
            gfx->fillRect(ax + 2 + i * 4, ay + 2, 3, 5, col);
        }
    }

    // Charging indicator: small "+" to the right of the icon
    if (charging) {
        int16_t cx = ax + 18, cy = ay + 2;
        gfx->drawFastHLine(cx, cy + 2, 5, col);  // horizontal bar
        gfx->drawFastVLine(cx + 2, cy, 5, col);   // vertical bar
    }
}

// ── Portrait HUD (368×448) ───────────────────────────────────────────────────

void draw_battery_p(Arduino_SH8601 *gfx) {
    drawBatteryAt(gfx, LCD_WIDTH - CORNER_R - 18, 10);
}

void draw_watermark_p(Arduino_SH8601 *gfx) {
    const char *wm = "www.app-pixels.com";
    int16_t tw = (int16_t)(strlen(wm) * 12);
    gfx->setTextSize(2);
    gfx->setTextColor(HUD_COL_WMK);
    gfx->setCursor((LCD_WIDTH - tw) / 2, LCD_HEIGHT - 20);
    gfx->print(wm);
}

// ── Landscape HUD (448×368) ──────────────────────────────────────────────────

#define L_W  448
#define L_H  368

void draw_battery_l(Arduino_Canvas *canvas) {
    drawBatteryAt(canvas, L_W - CORNER_R - 18, 10);
}

void draw_watermark_l(Arduino_Canvas *canvas) {
    const char *wm = "www.app-pixels.com";
    int16_t tw = (int16_t)(strlen(wm) * 12);
    canvas->setTextSize(2);
    canvas->setTextColor(HUD_COL_WMK);
    canvas->setCursor((L_W - tw) / 2, L_H - 20);
    canvas->print(wm);
}

// ── Generic HUD (any Arduino_GFX, explicit dims) ─────────────────────────────

void draw_battery_g(Arduino_GFX *gfx, int16_t w, int16_t /*h*/) {
    drawBatteryAt(gfx, w - CORNER_R - 18, 10);
}

void draw_watermark_g(Arduino_GFX *gfx, int16_t w, int16_t h) {
    const char *wm = "www.app-pixels.com";
    int16_t tw = (int16_t)(strlen(wm) * 12);
    gfx->setTextSize(2);
    gfx->setTextColor(HUD_COL_WMK);
    gfx->setCursor((w - tw) / 2, h - 20);
    gfx->print(wm);
}

// ═════════════════════════════════════════════════════════════════════════════
// Antialiased primitives
// Brute-force per-pixel — fine for radii up to ~130 (≈68 k checks). Used for
// bubble level bubbles + outer rings, and 8-ball outer ring.
// ═════════════════════════════════════════════════════════════════════════════

uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
    uint16_t fr  = (fg >> 11) & 0x1F;
    uint16_t fg5 = (fg >>  5) & 0x3F;
    uint16_t fb  =  fg        & 0x1F;
    uint16_t br  = (bg >> 11) & 0x1F;
    uint16_t bg5 = (bg >>  5) & 0x3F;
    uint16_t bb  =  bg        & 0x1F;
    // a is 0..255; (a + 1) >> 8 trick keeps it tight without /255.
    uint16_t r = (fr * a + br * (255 - a) + 127) / 255;
    uint16_t g = (fg5 * a + bg5 * (255 - a) + 127) / 255;
    uint16_t b = (fb * a + bb * (255 - a) + 127) / 255;
    return (r << 11) | (g << 5) | b;
}

// Solid fill where dist <= r-0.5, blended where r-0.5 < dist <= r+0.5.
void fillCircleAA(Arduino_GFX *gfx, int16_t cx, int16_t cy, int16_t r,
                  uint16_t col, uint16_t bg) {
    if (r < 1) { gfx->drawPixel(cx, cy, col); return; }
    // Per-row band: solid horizontal line for the inner core, two edge pixels
    // (left + right) per row plus top/bottom caps handled implicitly because
    // the |dy| > r-1 rows have no inner core and only blended edges.
    for (int16_t dy = -r - 1; dy <= r + 1; dy++) {
        // Half-width of the inner solid core for this row: dx_inner = sqrt(rinner² - dy²)
        // Half-width of the outer edge:                    dx_outer = sqrt(router² - dy²)
        float fdy = (float)dy;
        float ri  = (float)r - 0.5f;
        float ro  = (float)r + 0.5f;
        float ri2 = ri * ri - fdy * fdy;
        float ro2 = ro * ro - fdy * fdy;
        if (ro2 < 0) continue;                         // row outside circle
        int16_t dxOuter = (int16_t)sqrtf(ro2);
        int16_t y = cy + dy;
        if (ri2 > 0) {
            int16_t dxInner = (int16_t)sqrtf(ri2);
            // Inner core (solid)
            gfx->drawFastHLine(cx - dxInner, y, dxInner * 2 + 1, col);
            // Edge pixels left + right
            for (int16_t dx = dxInner + 1; dx <= dxOuter; dx++) {
                float d = sqrtf((float)dx * dx + fdy * fdy);
                float t = ro - d;                       // 0..1 across the 1-px band
                if (t > 1.0f) t = 1.0f;
                if (t < 0.0f) continue;
                uint8_t a = (uint8_t)(t * 255.0f);
                gfx->drawPixel(cx + dx, y, blend565(col, bg, a));
                gfx->drawPixel(cx - dx, y, blend565(col, bg, a));
            }
        } else {
            // Cap rows: only blended pixels, including dx=0
            for (int16_t dx = 0; dx <= dxOuter; dx++) {
                float d = sqrtf((float)dx * dx + fdy * fdy);
                float t = ro - d;
                if (t > 1.0f) t = 1.0f;
                if (t < 0.0f) continue;
                uint8_t a = (uint8_t)(t * 255.0f);
                uint16_t c = blend565(col, bg, a);
                gfx->drawPixel(cx + dx, y, c);
                if (dx != 0) gfx->drawPixel(cx - dx, y, c);
            }
        }
    }
}

// 1-px outline at radius r, antialiased over background `bg`.
void drawCircleAA(Arduino_GFX *gfx, int16_t cx, int16_t cy, int16_t r,
                  uint16_t col, uint16_t bg) {
    if (r < 1) { gfx->drawPixel(cx, cy, col); return; }
    float fr = (float)r;
    for (int16_t dy = -r - 1; dy <= r + 1; dy++) {
        float fdy = (float)dy;
        float band2 = (fr + 0.5f) * (fr + 0.5f) - fdy * fdy;
        if (band2 < 0) continue;
        int16_t dxMax = (int16_t)sqrtf(band2);
        int16_t y = cy + dy;
        for (int16_t dx = 0; dx <= dxMax; dx++) {
            float d = sqrtf((float)dx * dx + fdy * fdy);
            float diff = d - fr;
            if (diff < -0.5f || diff > 0.5f) continue;
            uint8_t a = (uint8_t)((1.0f - fabsf(diff) * 2.0f) * 255.0f);
            uint16_t c = blend565(col, bg, a);
            gfx->drawPixel(cx + dx, y, c);
            if (dx != 0) gfx->drawPixel(cx - dx, y, c);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// MIC pill — vertical, red, bottom-left. Reserved zone documented in header.
// ═════════════════════════════════════════════════════════════════════════════

void draw_mic_pill(Arduino_GFX *gfx, int16_t /*w*/, int16_t h) {
    // Plain vertical "MIC" text (no pill). Red. Moved up to clear the
    // rounded-corner safe zone — the original pill was half-eaten by the
    // bottom-left corner curve.
    const uint16_t txCol = 0xF800;
    drawTextRotCCW(gfx, 8, h - 38, "MIC", txCol, 2, 2);
}
