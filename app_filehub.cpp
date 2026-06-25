/*
 * app_filehub.cpp — wireless file hub over WiFi (WebDAV + HTTP upload/download)
 *
 * Portrait 368×448, canvas-based file explorer.
 *
 * Controls:
 *   Touch tap   – enter folder / go up (first row is "..")
 *   Touch drag  – scroll file list
 *   BOOT short  – toggle Info view (connection details + QR code)
 *   PWR  short  – toggle AP / STA mode at runtime
 *
 * Config from SD /setup/setup.txt:
 *   SSID / PASSWORD (+ optional SSID2/PASSWORD2, SSID3/PASSWORD3)
 *   AP_SSID            (optional; default "Filehub-XXXX" using MAC)
 *   AP_PASS            (optional; default "filehub123"; min 8 chars for WPA2)
 *   WEBDAV_USER        (optional; default "filehub")
 *   WEBDAV_PASS        (optional; empty = no auth)
 *   HTTP_UPLOAD_DIR    (optional; default "/filehub" — shared with downloads)
 *   HTTP_DOWNLOAD_DIR  (optional; default "/filehub" — shared with uploads)
 *   HTTP_PORT          (optional; default 80)
 */

#include "app_filehub.h"
#include "app_common.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <FS.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include "TouchDrvFT6X36.hpp"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "qrcode.h"

// USB MSC is only available in TinyUSB mode (USBMode=default + CDCOnBoot=cdc).
// Under Hardware CDC+JTAG mode this block is skipped so the file still builds.
#if !ARDUINO_USB_MODE && SOC_USB_OTG_SUPPORTED
  #define FH_HAS_USB_MSC 1
  #include <USB.h>
  #include <USBMSC.h>
#else
  #define FH_HAS_USB_MSC 0
#endif

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;
extern TouchDrvInterface *touch;

// ── Constants ────────────────────────────────────────────────────────────────
#define BOOT_BTN        0
#define PWR_POLL_MS     50
#define SWIPE_THRESH    20
#define TAP_MAX_MS      300
#define TAP_MAX_DIST    20
// UI geometry (portrait 368×448, follows UI_GUIDE.md)
#define ROW_H           28        // per-row height in the file list (textSize 2)
#define LIST_TOP        96        // starts below path bar; past BOOT pill (y≈79-111)
#define LIST_BOTTOM    326        // ends above PWR pill (y≈329-361)
#define PATH_BAR_Y      68
#define PATH_BAR_H      22
#define MAX_ENTRIES     512
#define NAME_COL_MAX    24        // max filename chars shown at textSize 2
#define QR_MAX_SIDE     49        // version-8 max = 49 modules

enum FhState { FH_BOOT, FH_NO_SD, FH_READY };
enum FhView  { VIEW_FILES, VIEW_INFO, VIEW_TEXT };
enum FhMode  { MODE_WIFI, MODE_USB };

// Text viewer constants
#define TEXT_MAX_BYTES  (256 * 1024)   // cap file read to keep PSRAM sane
#define TEXT_MAX_LINES  20000          // cap wrapped-line table
#define TEXT_COLS       29             // chars per line at textSize 2 (~12 px ea)
#define TEXT_TOP        46             // body starts below filename bar
#define TEXT_BOTTOM     320            // leaves room for bottom hint
#define TEXT_LINE_H     20             // 16 px char + 4 spacing

// ── Config ───────────────────────────────────────────────────────────────────
static char s_ssid[3][64] = {};
static char s_pass[3][64] = {};
static char s_apSsid[33]  = {};
static char s_apPass[33]  = "filehub123";
static char s_davUser[33] = "filehub";
static char s_davPass[33] = {};
static char s_upDir[64]   = "/filehub";
static char s_dnDir[64]   = "/filehub";
static uint16_t s_port    = 80;

// ── State ────────────────────────────────────────────────────────────────────
static Arduino_Canvas *canvas = nullptr;
static FhState   s_state      = FH_BOOT;
static bool      s_apMode     = false;
static char      s_ipStr[20]  = "0.0.0.0";
static char      s_ssidShown[40] = "";
static httpd_handle_t s_httpd = nullptr;
static volatile uint64_t s_bytesIn  = 0;   // bytes uploaded to device
static volatile uint64_t s_bytesOut = 0;   // bytes downloaded from device
static volatile uint32_t s_activity = 0;   // millis of last HTTP activity

// Info-view state
static FhView    s_view       = VIEW_FILES;
static uint8_t   s_qrMods[QR_MAX_SIDE * QR_MAX_SIDE] = {0};
static int       s_qrSize     = 0;
static char      s_qrSource[48] = {};

// USB-MSC state
static volatile FhMode s_mode = MODE_WIFI;
static volatile bool   s_usbPending = false;   // set from USB event, handled in loop
static volatile bool   s_exitUsbPending = false;
#if FH_HAS_USB_MSC
static USBMSC s_msc;
#endif

// Text viewer state
static char     *s_textBuf        = nullptr;  // PSRAM-allocated file bytes (NUL-terminated)
static uint32_t  s_textLen        = 0;        // bytes actually loaded
static bool      s_textTruncated  = false;
static char      s_textName[64]   = {};
static uint32_t *s_textLineOff    = nullptr;  // offsets into s_textBuf, PSRAM
static int       s_textLineCount  = 0;
static int       s_textScroll     = 0;

// UI
static char      s_path[256]  = "/";      // current relative path inside SD
static struct FhEntry {
    char name[96];
    uint32_t size;
    bool isDir;
} *s_entries = nullptr;
static int       s_entryCount = 0;
static int       s_scroll     = 0;
static bool      s_bootWas    = false;
static uint32_t  s_lastPwr    = 0;
static bool      s_touchWas   = false;
static int16_t   s_touchStartY= 0;
static int16_t   s_touchLastY = 0;
static uint32_t  s_touchDownMs= 0;
static bool      s_touchDrag  = false;
static uint32_t  s_lastDraw   = 0;
static uint32_t  s_lastStatus = 0;

// ── Config parsing helpers ───────────────────────────────────────────────────
// Anchored at line start so "SSID" doesn't match inside "AP_SSID". Skips any
// leading whitespace, then requires an exact key followed by a non-word char.
static bool extractVal(const char *line, const char *key, char *out, size_t cap) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    size_t kl = strlen(key);
    if (strncmp(p, key, kl) != 0) return false;
    char after = p[kl];
    if (isalnum((unsigned char)after) || after == '_') return false;
    p += kl;
    while (*p == ' ' || *p == '=') p++;
    if (*p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && *p != '\n' && *p != '\r' && n < cap - 1)
        out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
}

static void extractInt(const char *line, const char *key, uint16_t *out) {
    char buf[16];
    if (extractVal(line, key, buf, sizeof(buf))) {
        int v = atoi(buf);
        if (v > 0 && v < 65536) *out = (uint16_t)v;
    }
}

static void loadConfig() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_apSsid, sizeof(s_apSsid), "Filehub-%02X%02X", mac[4], mac[5]);

    File f = SD_MMC.open("/setup/setup.txt");
    if (!f) return;
    char line[200];
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        extractVal(line, "SSID",             s_ssid[0], 64);
        extractVal(line, "PASSWORD",         s_pass[0], 64);
        extractVal(line, "SSID2",            s_ssid[1], 64);
        extractVal(line, "PASSWORD2",        s_pass[1], 64);
        extractVal(line, "SSID3",            s_ssid[2], 64);
        extractVal(line, "PASSWORD3",        s_pass[2], 64);
        extractVal(line, "AP_SSID",          s_apSsid, 33);
        extractVal(line, "AP_PASS",          s_apPass, 33);
        extractVal(line, "WEBDAV_USER",      s_davUser, 33);
        extractVal(line, "WEBDAV_PASS",      s_davPass, 33);
        extractVal(line, "HTTP_UPLOAD_DIR",  s_upDir, 64);
        extractVal(line, "HTTP_DOWNLOAD_DIR",s_dnDir, 64);
        extractInt(line, "HTTP_PORT",        &s_port);
    }
    f.close();
}

// Forward declarations
static void startHttpd();
static void drawAll();
static void generateQr();

// ── WiFi ─────────────────────────────────────────────────────────────────────
static bool wifiStartSTA() {
    WifiCred list[3] = {
        { s_ssid[0], s_pass[0] },
        { s_ssid[1], s_pass[1] },
        { s_ssid[2], s_pass[2] },
    };
    int idx = wifi_try_connect(list, 3);
    if (idx < 0) return false;
    strncpy(s_ssidShown, s_ssid[idx], sizeof(s_ssidShown) - 1);
    s_ssidShown[sizeof(s_ssidShown) - 1] = '\0';
    IPAddress ip = WiFi.localIP();
    snprintf(s_ipStr, sizeof(s_ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    s_apMode = false;
    return true;
}

static void wifiStartAP() {
    WiFi.disconnect(true, true);
    delay(50);
    WiFi.mode(WIFI_AP);
    // WPA2 requires password >= 8 chars; fall back to open if shorter.
    const char *pw = (strlen(s_apPass) >= 8) ? s_apPass : nullptr;
    WiFi.softAP(s_apSsid, pw);
    delay(300);
    IPAddress ip = WiFi.softAPIP();
    snprintf(s_ipStr, sizeof(s_ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    strncpy(s_ssidShown, s_apSsid, sizeof(s_ssidShown) - 1);
    s_ssidShown[sizeof(s_ssidShown) - 1] = '\0';
    s_apMode = true;
}

// Toggle between AP and STA modes at runtime (PWR button).
static void wifiToggleMode() {
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = nullptr; }
    if (s_apMode) {
        // AP -> try STA; if it fails, stay in AP.
        if (!wifiStartSTA()) wifiStartAP();
    } else {
        wifiStartAP();
    }
    startHttpd();
}

// ── Path utilities ───────────────────────────────────────────────────────────
// SD_MMC paths are rooted at "/", not at the mount point. Do NOT prepend "/sdcard".
static void joinSd(char *out, size_t cap, const char *rel) {
    if (!rel || !*rel) { if (cap > 1) { out[0] = '/'; out[1] = '\0'; } return; }
    if (rel[0] == '/') { strncpy(out, rel, cap - 1); out[cap - 1] = '\0'; }
    else               { snprintf(out, cap, "/%s", rel); }
}

// In-place URL decode. Safe for null-terminated strings.
static void urlDecode(char *s) {
    char *dst = s, *src = s;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char h[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(h, nullptr, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Reject .. segments and backslashes. Keep leading slash, collapse doubles.
static bool sanitizeRelPath(char *p) {
    if (!p) return false;
    if (strstr(p, "..")) return false;
    for (char *c = p; *c; c++) if (*c == '\\') return false;
    return true;
}

static void humanSize(uint32_t b, char *buf, size_t cap) {
    if (b < 1024)                snprintf(buf, cap, "%u B",   (unsigned)b);
    else if (b < 1024UL * 1024)  snprintf(buf, cap, "%.1f KB", b / 1024.0);
    else if (b < 1024UL * 1024 * 1024) snprintf(buf, cap, "%.1f MB", b / (1024.0 * 1024));
    else                         snprintf(buf, cap, "%.2f GB", b / (1024.0 * 1024 * 1024));
}

static void ensureDir(const char *rel) {
    char full[256];
    joinSd(full, sizeof(full), rel);
    if (!SD_MMC.exists(full)) SD_MMC.mkdir(full);
}

// ── Base64 decode (for Basic auth) ───────────────────────────────────────────
static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64decode(const char *in, char *out, int cap) {
    int o = 0, bits = 0, buf = 0;
    for (const char *p = in; *p && *p != '='; p++) {
        int v = b64val(*p);
        if (v < 0) continue;
        buf = (buf << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (o < cap - 1) out[o++] = (buf >> bits) & 0xFF; }
    }
    out[o] = '\0';
    return o;
}

// ── Auth ─────────────────────────────────────────────────────────────────────
static bool checkAuth(httpd_req_t *req) {
    if (!s_davPass[0]) return true;  // auth disabled
    size_t hlen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hlen == 0 || hlen > 200) goto deny;
    {
        char hdr[220];
        if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) goto deny;
        const char *b = strstr(hdr, "Basic ");
        if (!b) goto deny;
        b += 6;
        char dec[96];
        b64decode(b, dec, sizeof(dec));
        char expect[80];
        snprintf(expect, sizeof(expect), "%s:%s", s_davUser, s_davPass);
        if (strcmp(dec, expect) == 0) return true;
    }
deny:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"filehub\"");
    httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
    return false;
}

// ── HTTP: landing page ───────────────────────────────────────────────────────
static const char *LANDING_HTML_HEAD =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Filehub</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#111;color:#eee}"
    "h1{font-size:1.2em;margin:.3em 0}h2{font-size:1em;margin:1.2em 0 .4em;color:#8cf}"
    "a{color:#8cf;text-decoration:none}a:hover{text-decoration:underline}"
    "input[type=file]{width:100%;padding:10px;background:#222;border:1px dashed #555;color:#eee;border-radius:6px}"
    "button{padding:10px 18px;background:#4a8;border:0;border-radius:6px;color:#fff;font-size:1em;margin-top:8px}"
    "button:disabled{opacity:.5}"
    "ul{list-style:none;padding:0}li{padding:6px 0;border-bottom:1px solid #333;display:flex;justify-content:space-between}"
    ".sz{color:#888;font-size:.85em}"
    "#log{margin-top:8px;font-family:monospace;font-size:.9em;color:#aaa}"
    "</style></head><body>"
    "<h1>Filehub</h1>"
    "<h2>Upload</h2>"
    "<input type=file id=f multiple>"
    "<button id=b onclick=up()>Upload</button>"
    "<div id=log></div>"
    "<script>"
    "async function up(){const fi=document.getElementById('f');const lg=document.getElementById('log');const b=document.getElementById('b');"
    "if(!fi.files.length){lg.textContent='Pick a file first.';return}b.disabled=true;"
    "for(const f of fi.files){lg.textContent='Uploading '+f.name+' ('+(f.size/1024|0)+' KB)...';"
    "try{const r=await fetch('/upload?name='+encodeURIComponent(f.name),{method:'PUT',body:f});"
    "lg.textContent=(r.ok?'OK: ':'FAIL: ')+f.name;}catch(e){lg.textContent='ERROR '+e;break}}"
    "b.disabled=false;fi.value='';setTimeout(()=>location.reload(),600)}"
    "</script>"
    "<h2>Download</h2>";

static esp_err_t handleRoot(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send_chunk(req, LANDING_HTML_HEAD, HTTPD_RESP_USE_STRLEN);

    char dirFull[128];
    joinSd(dirFull, sizeof(dirFull), s_dnDir);
    File dir = SD_MMC.open(dirFull);
    if (!dir || !dir.isDirectory()) {
        httpd_resp_send_chunk(req, "<p>(no download folder)</p>", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_chunk(req, "<ul>", HTTPD_RESP_USE_STRLEN);
        File f;
        int count = 0;
        while ((f = dir.openNextFile()) && count++ < 500) {
            if (f.isDirectory()) { f.close(); continue; }
            const char *nm = f.name();
            const char *base = strrchr(nm, '/');
            base = base ? base + 1 : nm;
            char sz[24]; humanSize((uint32_t)f.size(), sz, sizeof(sz));
            char row[300];
            // Minimal HTML-escape of filename (& < >)
            char esc[128]; size_t ei = 0;
            for (const char *c = base; *c && ei < sizeof(esc) - 7; c++) {
                if      (*c == '&') { memcpy(esc + ei, "&amp;", 5); ei += 5; }
                else if (*c == '<') { memcpy(esc + ei, "&lt;",  4); ei += 4; }
                else if (*c == '>') { memcpy(esc + ei, "&gt;",  4); ei += 4; }
                else                { esc[ei++] = *c; }
            }
            esc[ei] = '\0';
            snprintf(row, sizeof(row),
                "<li><a href='/download?name=%s'>%s</a><span class=sz>%s</span></li>",
                esc, esc, sz);
            httpd_resp_send_chunk(req, row, HTTPD_RESP_USE_STRLEN);
            f.close();
        }
        dir.close();
        httpd_resp_send_chunk(req, "</ul>", HTTPD_RESP_USE_STRLEN);
    }

    const char *foot =
        "<p style='color:#666;font-size:.8em;margin-top:2em'>WebDAV: /webdav/</p></body></html>";
    httpd_resp_send_chunk(req, foot, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nullptr, 0);
    s_activity = millis();
    return ESP_OK;
}

// ── HTTP: upload (PUT /upload?name=...) ──────────────────────────────────────
static esp_err_t handleUpload(httpd_req_t *req) {
    char name[128] = {};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 400) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name"); return ESP_FAIL; }
    char qry[420];
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query"); return ESP_FAIL; }
    if (httpd_query_key_value(qry, "name", name, sizeof(name)) != ESP_OK) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no name"); return ESP_FAIL; }
    urlDecode(name);
    // basename only
    for (char *c = name; *c; c++) if (*c == '/' || *c == '\\') *c = '_';
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_FAIL; }

    char full[256];
    snprintf(full, sizeof(full), "%s%s/%s",
             s_upDir[0] == '/' ? "" : "/", s_upDir, name);
    // Ensure parent exists
    ensureDir(s_upDir);

    File f = SD_MMC.open(full, FILE_WRITE);
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed"); return ESP_FAIL; }

    char buf[2048];
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) { f.close(); SD_MMC.remove(full); httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv fail"); return ESP_FAIL; }
        if (f.write((const uint8_t *)buf, r) != (size_t)r) {
            f.close(); SD_MMC.remove(full);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd write");
            return ESP_FAIL;
        }
        remaining -= r;
        s_bytesIn += r;
    }
    f.close();
    s_activity = millis();
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// ── HTTP: download one file (GET /download?name=...) ────────────────────────
static esp_err_t handleDownload(httpd_req_t *req) {
    char name[128] = {};
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name"); return ESP_FAIL; }
    char qry[420];
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad"); return ESP_FAIL; }
    if (httpd_query_key_value(qry, "name", name, sizeof(name)) != ESP_OK) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no name"); return ESP_FAIL; }
    urlDecode(name);
    for (char *c = name; *c; c++) if (*c == '/' || *c == '\\') { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name"); return ESP_FAIL; }

    char full[256];
    snprintf(full, sizeof(full), "%s%s/%s",
             s_dnDir[0] == '/' ? "" : "/", s_dnDir, name);
    File f = SD_MMC.open(full);
    if (!f || f.isDirectory()) { if (f) f.close(); httpd_resp_send_404(req); return ESP_FAIL; }

    httpd_resp_set_type(req, "application/octet-stream");
    char dispo[200];
    snprintf(dispo, sizeof(dispo), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", dispo);

    char buf[2048];
    while (f.available()) {
        int r = f.read((uint8_t *)buf, sizeof(buf));
        if (r <= 0) break;
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { f.close(); return ESP_FAIL; }
        s_bytesOut += r;
    }
    f.close();
    httpd_resp_send_chunk(req, nullptr, 0);
    s_activity = millis();
    return ESP_OK;
}

// ── WebDAV ───────────────────────────────────────────────────────────────────
// Strip "/webdav" prefix from req->uri → relative SD path.
static void davRel(const char *uri, char *out, size_t cap) {
    const char *p = uri;
    if (strncmp(p, "/webdav", 7) == 0) p += 7;
    if (!*p) { strncpy(out, "/", cap); out[cap-1] = '\0'; return; }
    strncpy(out, p, cap - 1);
    out[cap - 1] = '\0';
    // strip query
    char *q = strchr(out, '?'); if (q) *q = '\0';
    urlDecode(out);
}

static void xmlEscape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *c = in; *c && o < cap - 7; c++) {
        if      (*c == '&') { memcpy(out + o, "&amp;",  5); o += 5; }
        else if (*c == '<') { memcpy(out + o, "&lt;",   4); o += 4; }
        else if (*c == '>') { memcpy(out + o, "&gt;",   4); o += 4; }
        else if (*c == '"') { memcpy(out + o, "&quot;", 6); o += 6; }
        else                { out[o++] = *c; }
    }
    out[o] = '\0';
}

static void davEmitResponse(httpd_req_t *req, const char *rel, bool isDir, uint32_t size) {
    char enc[400];
    // Percent-encode spaces & a few specials for the href
    size_t o = 0;
    const char *src = rel;
    o += snprintf(enc + o, sizeof(enc) - o, "/webdav");
    for (const char *c = src; *c && o < sizeof(enc) - 4; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch > 127 || ch == ' ' || ch == '"' || ch == '#' || ch == '?' || ch == '&' || ch == '%' || ch == '+')
            o += snprintf(enc + o, sizeof(enc) - o, "%%%02X", ch);
        else
            enc[o++] = *c;
    }
    enc[o] = '\0';

    char body[800];
    if (isDir) {
        snprintf(body, sizeof(body),
            "<d:response><d:href>%s/</d:href><d:propstat><d:prop>"
            "<d:resourcetype><d:collection/></d:resourcetype>"
            "<d:getcontentlength>0</d:getcontentlength>"
            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>",
            enc);
    } else {
        snprintf(body, sizeof(body),
            "<d:response><d:href>%s</d:href><d:propstat><d:prop>"
            "<d:resourcetype/>"
            "<d:getcontentlength>%u</d:getcontentlength>"
            "<d:getcontenttype>application/octet-stream</d:getcontenttype>"
            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>",
            enc, (unsigned)size);
    }
    httpd_resp_send_chunk(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t davOptions(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "DAV", "1, 2");
    httpd_resp_set_hdr(req, "Allow", "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, MOVE, LOCK, UNLOCK");
    httpd_resp_set_hdr(req, "MS-Author-Via", "DAV");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t davPropfind(httpd_req_t *req, const char *rel) {
    // Drain body (some clients send <propfind/> XML; we ignore it)
    if (req->content_len > 0) {
        char tmp[256]; int rem = req->content_len;
        while (rem > 0) {
            int r = httpd_req_recv(req, tmp, rem > (int)sizeof(tmp) ? (int)sizeof(tmp) : rem);
            if (r <= 0) break;
            rem -= r;
        }
    }

    char full[300]; joinSd(full, sizeof(full), rel);
    File root = SD_MMC.open(full);
    if (!root) { httpd_resp_send_404(req); return ESP_FAIL; }

    int depth = 1;
    char dh[8];
    if (httpd_req_get_hdr_value_str(req, "Depth", dh, sizeof(dh)) == ESP_OK) {
        if (strcmp(dh, "0") == 0) depth = 0;
    }

    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_send_chunk(req,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<d:multistatus xmlns:d=\"DAV:\">", HTTPD_RESP_USE_STRLEN);

    davEmitResponse(req, rel, root.isDirectory(), (uint32_t)root.size());

    if (depth > 0 && root.isDirectory()) {
        File c;
        int count = 0;
        while ((c = root.openNextFile()) && count++ < MAX_ENTRIES) {
            const char *nm = c.name();
            const char *base = strrchr(nm, '/');
            base = base ? base + 1 : nm;
            char childRel[300];
            if (strcmp(rel, "/") == 0) snprintf(childRel, sizeof(childRel), "/%s", base);
            else                        snprintf(childRel, sizeof(childRel), "%s/%s", rel, base);
            davEmitResponse(req, childRel, c.isDirectory(), (uint32_t)c.size());
            c.close();
        }
    }
    root.close();

    httpd_resp_send_chunk(req, "</d:multistatus>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, nullptr, 0);
    s_activity = millis();
    return ESP_OK;
}

static esp_err_t davGet(httpd_req_t *req, const char *rel) {
    char full[300]; joinSd(full, sizeof(full), rel);
    File f = SD_MMC.open(full);
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
    if (f.isDirectory()) { f.close(); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "is directory"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/octet-stream");
    char buf[2048];
    while (f.available()) {
        int r = f.read((uint8_t *)buf, sizeof(buf));
        if (r <= 0) break;
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { f.close(); return ESP_FAIL; }
        s_bytesOut += r;
    }
    f.close();
    httpd_resp_send_chunk(req, nullptr, 0);
    s_activity = millis();
    return ESP_OK;
}

static esp_err_t davPut(httpd_req_t *req, const char *rel) {
    char full[300]; joinSd(full, sizeof(full), rel);
    File f = SD_MMC.open(full, FILE_WRITE);
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open fail"); return ESP_FAIL; }
    char buf[2048];
    int remaining = req->content_len;
    while (remaining > 0) {
        int want = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) { f.close(); SD_MMC.remove(full); httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv"); return ESP_FAIL; }
        if (f.write((const uint8_t *)buf, r) != (size_t)r) {
            f.close(); SD_MMC.remove(full);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd write");
            return ESP_FAIL;
        }
        remaining -= r;
        s_bytesIn += r;
    }
    f.close();
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_send(req, nullptr, 0);
    s_activity = millis();
    return ESP_OK;
}

static esp_err_t davDelete(httpd_req_t *req, const char *rel) {
    if (!rel[0] || strcmp(rel, "/") == 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "refuse root"); return ESP_FAIL; }
    char full[300]; joinSd(full, sizeof(full), rel);
    File f = SD_MMC.open(full);
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
    bool dir = f.isDirectory();
    f.close();
    bool ok = dir ? SD_MMC.rmdir(full) : SD_MMC.remove(full);
    if (!ok) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete fail"); return ESP_FAIL; }
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    s_activity = millis();
    return ESP_OK;
}

static esp_err_t davMkcol(httpd_req_t *req, const char *rel) {
    char full[300]; joinSd(full, sizeof(full), rel);
    if (SD_MMC.exists(full)) { httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "exists"); return ESP_FAIL; }
    if (!SD_MMC.mkdir(full)) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir fail"); return ESP_FAIL; }
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// Windows Explorer requires LOCK before writing. We return a fake lock token
// so the client thinks it holds an exclusive lock. We never reject UNLOCK.
static esp_err_t davLock(httpd_req_t *req, const char *rel) {
    // Drain the XML body (lock-info)
    if (req->content_len > 0) {
        char tmp[256]; int rem = req->content_len;
        while (rem > 0) { int r = httpd_req_recv(req, tmp, rem > (int)sizeof(tmp) ? (int)sizeof(tmp) : rem); if (r <= 0) break; rem -= r; }
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_set_hdr(req, "Lock-Token", "<opaquelocktoken:filehub-0001>");
    const char *body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<d:prop xmlns:d=\"DAV:\"><d:lockdiscovery><d:activelock>"
        "<d:locktype><d:write/></d:locktype>"
        "<d:lockscope><d:exclusive/></d:lockscope>"
        "<d:depth>infinity</d:depth>"
        "<d:owner><d:href>filehub</d:href></d:owner>"
        "<d:timeout>Second-3600</d:timeout>"
        "<d:locktoken><d:href>opaquelocktoken:filehub-0001</d:href></d:locktoken>"
        "</d:activelock></d:lockdiscovery></d:prop>";
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t davUnlock(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t davMove(httpd_req_t *req, const char *rel) {
    char dest[300] = {};
    if (httpd_req_get_hdr_value_str(req, "Destination", dest, sizeof(dest)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing Destination");
        return ESP_FAIL;
    }
    // Destination is a full URL like http://host/webdav/path — extract the /webdav/... part
    char *wp = strstr(dest, "/webdav");
    if (!wp) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad Destination"); return ESP_FAIL; }
    char destRel[300];
    davRel(wp, destRel, sizeof(destRel));
    size_t dl = strlen(destRel);
    if (dl > 1 && destRel[dl - 1] == '/') destRel[dl - 1] = '\0';

    char srcFull[300], dstFull[300];
    joinSd(srcFull, sizeof(srcFull), rel);
    joinSd(dstFull, sizeof(dstFull), destRel);

    if (!SD_MMC.rename(srcFull, dstFull)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename fail");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, "201 Created");
    httpd_resp_send(req, nullptr, 0);
    s_activity = millis();
    return ESP_OK;
}

static esp_err_t davProppatch(httpd_req_t *req) {
    // Drain body, return success — we don't persist custom properties.
    if (req->content_len > 0) {
        char tmp[256]; int rem = req->content_len;
        while (rem > 0) { int r = httpd_req_recv(req, tmp, rem > (int)sizeof(tmp) ? (int)sizeof(tmp) : rem); if (r <= 0) break; rem -= r; }
    }
    httpd_resp_set_status(req, "207 Multi-Status");
    httpd_resp_set_type(req, "application/xml; charset=utf-8");
    httpd_resp_send(req,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<d:multistatus xmlns:d=\"DAV:\"><d:response>"
        "<d:href>/</d:href><d:propstat><d:prop/>"
        "<d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
        "</d:response></d:multistatus>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t davDispatch(httpd_req_t *req) {
    if (!checkAuth(req)) return ESP_OK;

    char rel[300];
    davRel(req->uri, rel, sizeof(rel));
    // Trim trailing slash (except root)
    size_t rl = strlen(rel);
    if (rl > 1 && rel[rl - 1] == '/') rel[rl - 1] = '\0';
    if (!sanitizeRelPath(rel)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_FAIL; }

    switch (req->method) {
        case HTTP_OPTIONS:      return davOptions(req);
        case HTTP_GET:          return davGet(req, rel);
        case HTTP_HEAD:         return davGet(req, rel);
        case HTTP_PUT:          return davPut(req, rel);
        case HTTP_DELETE:       return davDelete(req, rel);
        case HTTP_PROPFIND:     return davPropfind(req, rel);
        case HTTP_MKCOL:        return davMkcol(req, rel);
        case HTTP_MOVE:         return davMove(req, rel);
        case HTTP_LOCK:         return davLock(req, rel);
        case HTTP_UNLOCK:       return davUnlock(req);
        case HTTP_PROPPATCH:    return davProppatch(req);
        default:
            httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "unsupported");
            return ESP_FAIL;
    }
}

// ── HTTP server setup ────────────────────────────────────────────────────────
static void startHttpd() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = s_port;
    cfg.ctrl_port         = 32768 + s_port;
    cfg.max_uri_handlers  = 28;
    cfg.max_open_sockets  = 4;
    cfg.stack_size        = 12288;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) { USBSerial.println("[filehub] httpd_start failed"); return; }

    httpd_uri_t root   = { .uri = "/",         .method = HTTP_GET,  .handler = handleRoot,     .user_ctx = nullptr };
    httpd_uri_t up     = { .uri = "/upload",   .method = HTTP_PUT,  .handler = handleUpload,   .user_ctx = nullptr };
    httpd_uri_t upPost = { .uri = "/upload",   .method = HTTP_POST, .handler = handleUpload,   .user_ctx = nullptr };
    httpd_uri_t dn     = { .uri = "/download", .method = HTTP_GET,  .handler = handleDownload, .user_ctx = nullptr };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &up);
    httpd_register_uri_handler(s_httpd, &upPost);
    httpd_register_uri_handler(s_httpd, &dn);

    // WebDAV: register each method explicitly (HTTP_ANY is not valid in uri handler method field)
    const httpd_method_t davMethods[] = {
        HTTP_OPTIONS, HTTP_GET, HTTP_HEAD, HTTP_PUT, HTTP_DELETE,
        (httpd_method_t)HTTP_PROPFIND, (httpd_method_t)HTTP_PROPPATCH,
        (httpd_method_t)HTTP_MKCOL, (httpd_method_t)HTTP_MOVE,
        (httpd_method_t)HTTP_LOCK, (httpd_method_t)HTTP_UNLOCK,
    };
    for (size_t i = 0; i < sizeof(davMethods) / sizeof(davMethods[0]); i++) {
        httpd_uri_t u1 = { .uri = "/webdav",   .method = davMethods[i], .handler = davDispatch, .user_ctx = nullptr };
        httpd_uri_t u2 = { .uri = "/webdav/*", .method = davMethods[i], .handler = davDispatch, .user_ctx = nullptr };
        httpd_register_uri_handler(s_httpd, &u1);
        httpd_register_uri_handler(s_httpd, &u2);
    }
}

// ── UI ───────────────────────────────────────────────────────────────────────
static int cmpEntry(const void *a, const void *b) {
    const FhEntry *x = (const FhEntry *)a, *y = (const FhEntry *)b;
    if (x->isDir != y->isDir) return x->isDir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void refreshListing() {
    if (!s_entries) s_entries = (FhEntry *)ps_malloc(sizeof(FhEntry) * MAX_ENTRIES);
    if (!s_entries) return;
    s_entryCount = 0;

    char full[300]; joinSd(full, sizeof(full), s_path);
    File d = SD_MMC.open(full);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }

    File c;
    while ((c = d.openNextFile()) && s_entryCount < MAX_ENTRIES) {
        const char *nm = c.name();
        const char *base = strrchr(nm, '/'); base = base ? base + 1 : nm;
        FhEntry &e = s_entries[s_entryCount++];
        strncpy(e.name, base, sizeof(e.name) - 1); e.name[sizeof(e.name) - 1] = '\0';
        e.size  = (uint32_t)c.size();
        e.isDir = c.isDirectory();
        c.close();
    }
    d.close();
    qsort(s_entries, s_entryCount, sizeof(FhEntry), cmpEntry);
    s_scroll = 0;
}

static void pathUp() {
    if (strcmp(s_path, "/") == 0) return;
    char *slash = strrchr(s_path, '/');
    if (!slash) return;
    if (slash == s_path) { s_path[1] = '\0'; }   // -> "/"
    else                 { *slash    = '\0'; }
    refreshListing();
}

static void pathEnter(const char *sub) {
    char np[256];
    if (strcmp(s_path, "/") == 0) snprintf(np, sizeof(np), "/%s", sub);
    else                          snprintf(np, sizeof(np), "%s/%s", s_path, sub);
    if (!sanitizeRelPath(np)) return;
    strncpy(s_path, np, sizeof(s_path) - 1); s_path[sizeof(s_path) - 1] = '\0';
    refreshListing();
}

// ── QR code generation (for Info view) ───────────────────────────────────────
static void qrDisplayCb(esp_qrcode_handle_t qr) {
    int sz = esp_qrcode_get_size(qr);
    if (sz <= 0 || sz > QR_MAX_SIDE) { s_qrSize = 0; return; }
    s_qrSize = sz;
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++)
            s_qrMods[y * QR_MAX_SIDE + x] = esp_qrcode_get_module(qr, x, y) ? 1 : 0;
}

static void generateQr() {
    char url[48];
    if (s_port == 80) snprintf(url, sizeof(url), "http://%s", s_ipStr);
    else              snprintf(url, sizeof(url), "http://%s:%u", s_ipStr, (unsigned)s_port);
    if (strcmp(url, s_qrSource) == 0 && s_qrSize > 0) return;  // up-to-date
    strncpy(s_qrSource, url, sizeof(s_qrSource) - 1);
    s_qrSource[sizeof(s_qrSource) - 1] = '\0';
    s_qrSize = 0;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qrDisplayCb;
    cfg.max_qrcode_version = 6;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
    esp_qrcode_generate(&cfg, url);
}

// ── Shared header: centered title ────────────────────────────────────────────
static void drawTitle() {
    const char *title = "Filehub";
    int tw = (int)strlen(title) * 18;  // textSize 3 = 18 px per char
    canvas->setTextSize(3);
    canvas->setTextColor(0x07FF);
    canvas->setCursor((LCD_WIDTH - tw) / 2, 10);
    canvas->print(title);
}

// ── Main file-explorer view (BOOT toggles to Info view) ──────────────────────
//
// Layout:
//   y= 10  Title "Filehub"  (centered, textSize 3)
//   y= 40  Mode + SSID      (textSize 2, colored)
//   y= 68- 90  Path bar     (panel bg)
//   y= 96-326  File list    (textSize 2, ROW_H=28, ~8 rows visible)
//   y=329-361  PWR pill zone
//   y=370  Counters "up .. dn .."
//   y=428  Watermark

static void drawFiles() {
    canvas->fillScreen(0x0000);

    drawTitle();

    // ── Mode + SSID (single line) ───────────────────────────────────────────
    canvas->setTextSize(2);
    canvas->setTextColor(s_apMode ? 0xFD20 : 0x07E0);   // orange for AP, green for STA
    canvas->setCursor(12, 40);
    canvas->print(s_apMode ? "Hotspot: " : "WiFi: ");
    canvas->setTextColor(0xDEFB);
    char ssidTrim[22];
    strncpy(ssidTrim, s_ssidShown, sizeof(ssidTrim) - 1);
    ssidTrim[sizeof(ssidTrim) - 1] = '\0';
    canvas->print(ssidTrim);

    // ── Path bar ────────────────────────────────────────────────────────────
    canvas->fillRect(0, PATH_BAR_Y, LCD_WIDTH, PATH_BAR_H, HUD_PILL_BG);
    canvas->drawFastHLine(0, PATH_BAR_Y,              LCD_WIDTH, HUD_PILL_BD);
    canvas->drawFastHLine(0, PATH_BAR_Y + PATH_BAR_H, LCD_WIDTH, HUD_PILL_BD);
    canvas->setTextSize(2);
    canvas->setTextColor(0xDEFB);
    canvas->setCursor(8, PATH_BAR_Y + 4);
    // Truncate long paths to fit (x width 0..330 → ~26 chars at textSize 2).
    char pathTrim[32];
    size_t pl = strlen(s_path);
    if (pl > 26) {
        strcpy(pathTrim, "...");
        strcat(pathTrim, s_path + pl - 23);
    } else {
        strncpy(pathTrim, s_path, sizeof(pathTrim) - 1);
        pathTrim[sizeof(pathTrim) - 1] = '\0';
    }
    canvas->print(pathTrim);

    // ── File list ───────────────────────────────────────────────────────────
    int listH = LIST_BOTTOM - LIST_TOP;
    int rowsVisible = listH / ROW_H;
    int atRoot = (strcmp(s_path, "/") == 0);
    int totalRows = s_entryCount + (atRoot ? 0 : 1);

    if (totalRows > rowsVisible && s_scroll > totalRows - rowsVisible)
        s_scroll = totalRows - rowsVisible;
    if (s_scroll < 0) s_scroll = 0;

    for (int row = 0; row < rowsVisible; row++) {
        int idx = s_scroll + row;
        if (idx >= totalRows) break;
        int16_t y = LIST_TOP + row * ROW_H;

        // Alternating row background for readability
        canvas->fillRect(0, y, LCD_WIDTH, ROW_H - 2, (row & 1) ? 0x0861 : 0x0000);

        const char *name;
        char szBuf[16] = "";
        bool isDir;
        if (!atRoot && idx == 0) { name = ".."; isDir = true; }
        else {
            const FhEntry &e = s_entries[idx - (atRoot ? 0 : 1)];
            name = e.name;
            isDir = e.isDir;
            if (!e.isDir) humanSize(e.size, szBuf, sizeof(szBuf));
        }

        // Folder glyph: "/" prefix in accent color, file shows inline.
        canvas->setTextSize(2);
        canvas->setCursor(10, y + 5);
        if (isDir) { canvas->setTextColor(0x07FF); canvas->print("/ "); }
        else       { canvas->setTextColor(0x2104); canvas->print("  "); }

        // Filename (truncate to NAME_COL_MAX chars so size column fits)
        char trimmed[NAME_COL_MAX + 1];
        strncpy(trimmed, name, NAME_COL_MAX);
        trimmed[NAME_COL_MAX] = '\0';
        canvas->setTextColor(isDir ? 0xDEFB : 0xFFFF);
        printUtf8(canvas, trimmed);

        // Size column (right-aligned near pill edge, textSize 1 for density)
        if (szBuf[0]) {
            canvas->setTextSize(1);
            canvas->setTextColor(0x8410);
            int16_t szW = (int16_t)strlen(szBuf) * 6;
            canvas->setCursor(LCD_WIDTH - 32 - szW, y + 10);
            canvas->print(szBuf);
        }
    }

    // ── Counters (footer) ───────────────────────────────────────────────────
    canvas->setTextSize(2);
    canvas->setTextColor(0x4208);
    char up[16], dn[16], counters[48];
    humanSize((uint32_t)(s_bytesIn  & 0xFFFFFFFF), up, sizeof(up));
    humanSize((uint32_t)(s_bytesOut & 0xFFFFFFFF), dn, sizeof(dn));
    snprintf(counters, sizeof(counters), "up %s  dn %s", up, dn);
    canvas->setCursor(12, 370);
    canvas->print(counters);

    // ── Pill labels (BOOT=info, PWR=toggle ap/sta) ──────────────────────────
    draw_pill_label(canvas, 0, 0, "info");
    draw_pill_label(canvas, 0, 1, s_apMode ? "sta" : "ap");

    // ── Battery + watermark (shared HUD) ────────────────────────────────────
    draw_battery_g  (canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);

    canvas->flush();
}

// ── Info view: connection details + QR code ─────────────────────────────────
static void drawQr(int cx, int cy, int pixelSize) {
    if (s_qrSize <= 0) return;
    int total = s_qrSize * pixelSize;
    int x0 = cx - total / 2;
    int y0 = cy - total / 2;
    // White background frame with quiet zone
    canvas->fillRect(x0 - pixelSize * 2, y0 - pixelSize * 2,
                     total + pixelSize * 4, total + pixelSize * 4, 0xFFFF);
    for (int yy = 0; yy < s_qrSize; yy++)
        for (int xx = 0; xx < s_qrSize; xx++)
            if (s_qrMods[yy * QR_MAX_SIDE + xx])
                canvas->fillRect(x0 + xx * pixelSize, y0 + yy * pixelSize,
                                 pixelSize, pixelSize, 0x0000);
}

static void drawInfo() {
    canvas->fillScreen(0x0000);
    drawTitle();

    int y = 44;
    canvas->setTextSize(2);

    // Mode + SSID
    canvas->setTextColor(s_apMode ? 0xFD20 : 0x07E0);
    canvas->setCursor(12, y);
    canvas->print(s_apMode ? "Hotspot: " : "WiFi: ");
    canvas->setTextColor(0xDEFB);
    canvas->print(s_ssidShown);
    y += 22;

    // Password (AP only)
    if (s_apMode) {
        canvas->setTextColor(0xFD20);
        canvas->setCursor(12, y);
        canvas->print("Pass: ");
        canvas->setTextColor(0xFFFF);
        canvas->print(strlen(s_apPass) >= 8 ? s_apPass : "(open)");
        y += 22;
    }

    // URL
    canvas->setTextColor(0xFFE0);
    canvas->setCursor(12, y);
    canvas->print("http://");
    canvas->print(s_ipStr);
    if (s_port != 80) { canvas->print(":"); canvas->print(s_port); }
    y += 22;

    // WebDAV
    canvas->setTextColor(0x07FF);
    canvas->setCursor(12, y);
    canvas->print("WebDAV: /webdav");
    if (s_davPass[0]) {
        canvas->setTextColor(0x8410);
        canvas->print(" u:");
        canvas->print(s_davUser);
    }
    y += 28;

    // QR code — size picks pixel so it fits the remaining room
    if (s_qrSize > 0) {
        int avail = 400 - y;                       // vertical room above watermark
        int maxSide = (avail < 240) ? avail : 240;
        int pixelSize = maxSide / (s_qrSize + 4);  // +4 quiet zone
        if (pixelSize < 2) pixelSize = 2;
        int cy = y + (s_qrSize * pixelSize) / 2 + pixelSize * 2;
        drawQr(LCD_WIDTH / 2, cy, pixelSize);
    }

    // Hint
    canvas->setTextSize(2);
    canvas->setTextColor(0x8410);
    const char *hint = "BOOT = back to files";
    int hw = (int)strlen(hint) * 12;
    canvas->setCursor((LCD_WIDTH - hw) / 2, 404);
    canvas->print(hint);

    // Pills unchanged (info stays "info" — pressing BOOT again returns)
    draw_pill_label(canvas, 0, 0, "back");
    draw_pill_label(canvas, 0, 1, s_apMode ? "sta" : "ap");
    draw_battery_g  (canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();
}

static void drawText();
static void drawAll() {
    if      (s_view == VIEW_INFO) drawInfo();
    else if (s_view == VIEW_TEXT) drawText();
    else                          drawFiles();
}

// ── Text viewer ──────────────────────────────────────────────────────────────
static bool isTextFile(const char *name) {
    static const char *exts[] = {
        ".txt", ".md", ".gcode", ".nc", ".csv", ".log", ".json", ".ini",
        ".cfg", ".conf", ".sh", ".py", ".js", ".html", ".htm", ".css",
        ".xml", ".yaml", ".yml", ".c", ".cpp", ".h", ".hpp", ".ino", nullptr
    };
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[12]; size_t n = strlen(dot);
    if (n >= sizeof(ext)) return false;
    for (size_t i = 0; i <= n; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    for (int i = 0; exts[i]; i++) if (!strcmp(ext, exts[i])) return true;
    return false;
}

static void freeText() {
    if (s_textBuf)     { heap_caps_free(s_textBuf);     s_textBuf = nullptr; }
    if (s_textLineOff) { heap_caps_free(s_textLineOff); s_textLineOff = nullptr; }
    s_textLen = 0; s_textLineCount = 0; s_textScroll = 0; s_textTruncated = false;
    s_textName[0] = '\0';
}

// Build wrapped-line start-offset table. Hard-break on '\n', soft-break at
// TEXT_COLS columns, preferring to break on a space when one is nearby.
static void buildTextLines() {
    if (!s_textBuf || !s_textLineOff) return;
    s_textLineCount = 0;
    uint32_t i = 0;
    while (i < s_textLen && s_textLineCount < TEXT_MAX_LINES) {
        s_textLineOff[s_textLineCount++] = i;
        uint32_t lineStart = i;
        uint32_t lastSpace = 0; bool haveSpace = false;
        while (i < s_textLen) {
            char c = s_textBuf[i];
            if (c == '\n') { i++; break; }
            if (c == ' ') { lastSpace = i; haveSpace = true; }
            uint32_t col = i - lineStart;
            if (col >= TEXT_COLS) {
                if (haveSpace && (i - lastSpace) < TEXT_COLS / 2) {
                    i = lastSpace + 1;   // soft-break at last space
                }
                break;
            }
            i++;
        }
    }
}

static bool openTextFile(const char *sdPath, const char *displayName) {
    freeText();
    File f = SD_MMC.open(sdPath, FILE_READ);
    if (!f || f.isDirectory()) { if (f) f.close(); return false; }

    uint32_t fsize = f.size();
    uint32_t toRead = fsize;
    if (toRead > TEXT_MAX_BYTES) { toRead = TEXT_MAX_BYTES; s_textTruncated = true; }

    s_textBuf = (char *)heap_caps_malloc(toRead + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_textBuf) { f.close(); return false; }

    uint32_t got = 0;
    while (got < toRead) {
        int n = f.read((uint8_t *)s_textBuf + got, toRead - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    f.close();
    s_textLen = got;
    s_textBuf[s_textLen] = '\0';

    // Normalize CR, convert tabs to 4 spaces in-place where cheap (skip: just
    // render tabs as space during draw). Strip lone '\r'.
    for (uint32_t k = 0; k < s_textLen; k++) if (s_textBuf[k] == '\r') s_textBuf[k] = ' ';

    s_textLineOff = (uint32_t *)heap_caps_malloc(TEXT_MAX_LINES * sizeof(uint32_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_textLineOff) { heap_caps_free(s_textBuf); s_textBuf = nullptr; return false; }

    buildTextLines();

    strncpy(s_textName, displayName, sizeof(s_textName) - 1);
    s_textName[sizeof(s_textName) - 1] = '\0';
    s_textScroll = 0;
    return true;
}

static void drawText() {
    canvas->fillScreen(0x0000);

    // Filename title bar
    canvas->setTextSize(2);
    canvas->setTextColor(0x07FF);
    char title[30];
    size_t nl = strlen(s_textName);
    if (nl > 26) { memcpy(title, s_textName, 23); title[23] = '.'; title[24] = '.'; title[25] = '.'; title[26] = '\0'; }
    else         { strcpy(title, s_textName); }
    int16_t tw = (int16_t)strlen(title) * 12;
    canvas->setCursor((LCD_WIDTH - tw) / 2, 14);
    canvas->print(title);

    // Body
    int linesPerPage = (TEXT_BOTTOM - TEXT_TOP) / TEXT_LINE_H;
    if (s_textScroll < 0) s_textScroll = 0;
    if (s_textScroll > s_textLineCount - 1) s_textScroll = s_textLineCount - 1;
    if (s_textScroll < 0) s_textScroll = 0;

    canvas->setTextSize(2);
    canvas->setTextColor(0xFFFF);
    for (int row = 0; row < linesPerPage; row++) {
        int idx = s_textScroll + row;
        if (idx >= s_textLineCount) break;
        uint32_t start = s_textLineOff[idx];
        uint32_t end   = (idx + 1 < s_textLineCount) ? s_textLineOff[idx + 1] : s_textLen;
        // Trim trailing newline from the line for rendering.
        while (end > start && (s_textBuf[end - 1] == '\n' || s_textBuf[end - 1] == ' ')) end--;
        uint32_t len = end - start;
        if (len > TEXT_COLS) len = TEXT_COLS;
        char buf[TEXT_COLS + 1];
        memcpy(buf, s_textBuf + start, len);
        for (uint32_t k = 0; k < len; k++) {
            unsigned char c = (unsigned char)buf[k];
            if (c == '\t') buf[k] = ' ';
            else if (c < 0x20 || c >= 0x7F) buf[k] = '.';
        }
        buf[len] = '\0';
        canvas->setCursor(8, TEXT_TOP + row * TEXT_LINE_H);
        canvas->print(buf);
    }

    // Bottom status: line range + truncation flag
    canvas->setTextSize(2);
    canvas->setTextColor(0x8410);
    char st[48];
    int endLine = s_textScroll + linesPerPage;
    if (endLine > s_textLineCount) endLine = s_textLineCount;
    snprintf(st, sizeof(st), "lines %d-%d / %d%s",
             s_textScroll + 1, endLine, s_textLineCount,
             s_textTruncated ? " (truncated)" : "");
    int sw = (int)strlen(st) * 12;
    canvas->setCursor((LCD_WIDTH - sw) / 2, 404);
    canvas->print(st);

    draw_pill_label(canvas, 0, 0, "back");
    draw_battery_g  (canvas, LCD_WIDTH, LCD_HEIGHT);
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();
}

static void drawCenteredBanner(const char *title, const char *sub, uint16_t titleCol) {
    canvas->fillScreen(0x0000);
    canvas->setTextSize(3);
    canvas->setTextColor(titleCol);
    int16_t tw = (int16_t)strlen(title) * 18;
    canvas->setCursor((LCD_WIDTH - tw) / 2, 180);
    canvas->print(title);
    if (sub) {
        canvas->setTextSize(2);
        canvas->setTextColor(0xDEFB);
        int16_t sw = (int16_t)strlen(sub) * 12;
        canvas->setCursor((LCD_WIDTH - sw) / 2, 230);
        canvas->print(sub);
    }
    draw_watermark_g(canvas, LCD_WIDTH, LCD_HEIGHT);
    canvas->flush();
}

static void drawNoSd()              { drawCenteredBanner("No SD card", "insert and reset", 0xFD20); }
static void drawBoot(const char *m) { drawCenteredBanner("Filehub",    m,                   0x07FF); }
static void drawUsb()               { drawCenteredBanner("USB mode",   "eject to resume WiFi", 0x07FF); }

// ── USB Mass Storage (PC sees the SD card as a removable drive) ─────────────
#if FH_HAS_USB_MSC
extern "C" {
#include "diskio.h"
}
// SD_MMC registers as FATFS drive 0 when it's the only SD card. Calling
// disk_read/disk_write directly with the host's full block count issues a
// single CMD18/CMD25 (multi-block) instead of N× CMD17/CMD24 — the SD
// per-command overhead is what makes SD_MMC.readRAW feel slow from MSC.
static constexpr uint8_t FH_PDRV = 0;

static int32_t mscOnWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)offset;
    uint32_t secSize = SD_MMC.sectorSize();
    if (!secSize) return -1;
    uint32_t count = bufsize / secSize;
    if (disk_write(FH_PDRV, buffer, lba, count) != RES_OK) return -1;
    s_bytesIn += bufsize;
    return bufsize;
}
static int32_t mscOnRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)offset;
    uint32_t secSize = SD_MMC.sectorSize();
    if (!secSize) return -1;
    uint32_t count = bufsize / secSize;
    if (disk_read(FH_PDRV, (uint8_t *)buffer, lba, count) != RES_OK) return -1;
    s_bytesOut += bufsize;
    return bufsize;
}
static bool mscOnStartStop(uint8_t, bool, bool) { return true; }

static void usbEventCallback(void *, esp_event_base_t base, int32_t id, void *) {
    if (base != ARDUINO_USB_EVENTS) return;
    // Tear down contention as fast as possible: PC starts issuing SCSI reads
    // within tens of ms of ARDUINO_USB_STARTED_EVENT. If HTTP and WiFi keep
    // running alongside those reads, MSC throughput drops ~10× and the initial
    // FAT scan that populates the host's file list can take 20-30 s.
    // Event callback runs on the Arduino USB event task (not ISR), so calling
    // httpd_stop / WiFi.mode here is safe.
    if (id == ARDUINO_USB_STARTED_EVENT) {
        if (s_mode == MODE_WIFI) {
            if (s_httpd) { httpd_stop(s_httpd); s_httpd = nullptr; }
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            s_mode = MODE_USB;
        }
        s_usbPending = true;  // loop redraws the UI
    }
    if (id == ARDUINO_USB_STOPPED_EVENT) s_exitUsbPending = true;
}

static void enterUsbMode() {
    // Called from loop after usbEventCallback flipped s_mode. Just update UI.
    drawUsb();
}

static void exitUsbMode() {
    // WiFi is down; bring it back up the same way boot does.
    if (!wifiStartSTA()) wifiStartAP();
    startHttpd();
    s_mode = MODE_WIFI;
    refreshListing();
    drawAll();
}

static void initUsbMsc() {
    s_msc.vendorID("Pixels");
    s_msc.productID("Filehub");
    s_msc.productRevision("1.0");
    s_msc.onRead(mscOnRead);
    s_msc.onWrite(mscOnWrite);
    s_msc.onStartStop(mscOnStartStop);
    s_msc.begin(SD_MMC.numSectors(), SD_MMC.sectorSize());
    // Advertise media present so the host doesn't cache "no media"
    // and wait out its retry interval (~30 s) before reading.
    s_msc.mediaPresent(true);
    USB.onEvent(usbEventCallback);
    USB.begin();
}
#else
static void initUsbMsc() {}
#endif

// ── Lifecycle ────────────────────────────────────────────────────────────────
void app_filehub_setup(Arduino_OLED *gfx_unused) {
    (void)gfx_unused;
    canvas = g_canvas;
    s_bytesIn = s_bytesOut = 0;
    s_scroll = 0;

    drawBoot("starting...");

    // Mount SD — keep mounted for the life of the app
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (!SD_MMC.begin("/sdcard", true)) {
        s_state = FH_NO_SD;
        drawNoSd();
        return;
    }

    loadConfig();
    ensureDir(s_upDir);
    ensureDir(s_dnDir);

    drawBoot("connecting wifi...");
    if (!wifiStartSTA()) {
        drawBoot("starting AP...");
        wifiStartAP();
    }

    drawBoot("starting server...");
    startHttpd();
    initUsbMsc();

    strcpy(s_path, "/");
    refreshListing();
    s_state = FH_READY;
    drawAll();
    s_lastDraw = millis();
}

void app_filehub_loop() {
    // Keep the screen awake: filehub is a background service app, no idle-off.
    common_activity();
    common_tick();

#if FH_HAS_USB_MSC
    // usbEventCallback already flipped s_mode and stopped HTTP/WiFi; here we
    // just catch up the UI on the Arduino loop task (safer for canvas draws).
    if (s_usbPending)     { s_usbPending = false;     if (s_mode == MODE_USB)  enterUsbMode(); }
    if (s_exitUsbPending) { s_exitUsbPending = false; if (s_mode == MODE_USB)  exitUsbMode(); }
#endif

    if (s_mode == MODE_USB) {
        // PC has exclusive use of the SD; freeze the UI but still poll BOOT
        // so the user can force-exit without yanking the cable.
        bool boot = (digitalRead(BOOT_BTN) == LOW);
        if (boot && !s_bootWas) {
#if FH_HAS_USB_MSC
            exitUsbMode();
#endif
        }
        s_bootWas = boot;
        delay(50);
        return;
    }

    if (s_state == FH_NO_SD) {
        delay(2000);
        // Retry SD
        if (SD_MMC.begin("/sdcard", true)) {
            s_state = FH_BOOT;
            loadConfig();
            ensureDir(s_upDir); ensureDir(s_dnDir);
            if (!wifiStartSTA()) wifiStartAP();
            startHttpd();
            strcpy(s_path, "/");
            refreshListing();
            s_state = FH_READY;
            drawAll();
        }
        return;
    }

    uint32_t now = millis();

    // BOOT short press = toggle Info view (or exit text viewer back to list)
    bool boot = (digitalRead(BOOT_BTN) == LOW);
    if (boot && !s_bootWas) {
        common_activity();
        if (s_view == VIEW_TEXT) {
            freeText();
            s_view = VIEW_FILES;
        } else {
            s_view = (s_view == VIEW_FILES) ? VIEW_INFO : VIEW_FILES;
            if (s_view == VIEW_INFO) generateQr();
        }
        drawAll();
        s_lastDraw = now;
    }
    s_bootWas = boot;

    // PWR short press = toggle AP / STA mode at runtime
    if (common_consume_pwr_short()) {
        common_activity();
        drawBoot(s_apMode ? "trying wifi..." : "starting AP...");
        wifiToggleMode();
        s_qrSource[0] = '\0';               // URL changed; force QR regen
        if (s_view == VIEW_INFO) generateQr();
        drawAll();
        s_lastDraw = now;
    }

    // Touch
    int16_t tx[1], ty[1];
    bool touching = touch->getPoint(tx, ty, 1);

    if (touching && !s_touchWas) {
        s_touchStartY = ty[0];
        s_touchLastY  = ty[0];
        s_touchDownMs = now;
        s_touchDrag   = false;
        common_activity();
    }

    if (touching && s_touchWas && s_view == VIEW_FILES) {
        int16_t dy = ty[0] - s_touchLastY;
        if (dy > SWIPE_THRESH) {
            s_scroll -= 1;
            if (s_scroll < 0) s_scroll = 0;
            s_touchLastY = ty[0];
            s_touchDrag = true;
            drawAll();
            s_lastDraw = now;
        } else if (dy < -SWIPE_THRESH) {
            s_scroll += 1;
            s_touchLastY = ty[0];
            s_touchDrag = true;
            drawAll();
            s_lastDraw = now;
        }
    }

    if (touching && s_touchWas && s_view == VIEW_TEXT) {
        // Scroll the text viewer in whole-line steps. Step size ~page/3 feels
        // responsive without skipping past short paragraphs.
        int16_t dy = ty[0] - s_touchLastY;
        int linesPerPage = (TEXT_BOTTOM - TEXT_TOP) / TEXT_LINE_H;
        int step = linesPerPage / 3; if (step < 1) step = 1;
        if (dy > SWIPE_THRESH) {
            s_textScroll -= step;
            if (s_textScroll < 0) s_textScroll = 0;
            s_touchLastY = ty[0];
            s_touchDrag = true;
            drawAll();
            s_lastDraw = now;
        } else if (dy < -SWIPE_THRESH) {
            int maxScroll = s_textLineCount - linesPerPage;
            if (maxScroll < 0) maxScroll = 0;
            if (s_textScroll < maxScroll) s_textScroll += step;
            if (s_textScroll > maxScroll) s_textScroll = maxScroll;
            s_touchLastY = ty[0];
            s_touchDrag = true;
            drawAll();
            s_lastDraw = now;
        }
    }

    if (!touching && s_touchWas && s_view == VIEW_FILES) {
        // Touch up — tap detection
        int16_t totDy = abs((int)ty[0] - (int)s_touchStartY);
        uint32_t dur = now - s_touchDownMs;
        if (!s_touchDrag && dur < TAP_MAX_MS && totDy < TAP_MAX_DIST) {
            int yAtTouch = s_touchStartY;
            if (yAtTouch >= LIST_TOP && yAtTouch < LIST_BOTTOM) {
                int row = (yAtTouch - LIST_TOP) / ROW_H;
                int idx = s_scroll + row;
                int atRoot = (strcmp(s_path, "/") == 0);
                int totalRows = s_entryCount + (atRoot ? 0 : 1);
                if (idx >= 0 && idx < totalRows) {
                    if (!atRoot && idx == 0) pathUp();
                    else {
                        const FhEntry &e = s_entries[idx - (atRoot ? 0 : 1)];
                        if (e.isDir) {
                            pathEnter(e.name);
                        } else if (isTextFile(e.name)) {
                            char full[512];
                            // Build absolute SD path from current s_path + e.name
                            if (strcmp(s_path, "/") == 0) snprintf(full, sizeof(full), "/%s", e.name);
                            else                          snprintf(full, sizeof(full), "%s/%s", s_path, e.name);
                            if (openTextFile(full, e.name)) s_view = VIEW_TEXT;
                        }
                    }
                    drawAll();
                    s_lastDraw = now;
                }
            }
        }
    }
    s_touchWas = touching;

    // Redraw status bar periodically if HTTP activity happened
    if (now - s_lastStatus > 500) {
        s_lastStatus = now;
        static uint64_t lastIn = 0, lastOut = 0;
        if (s_bytesIn != lastIn || s_bytesOut != lastOut) {
            lastIn = s_bytesIn; lastOut = s_bytesOut;
            drawAll();
            s_lastDraw = now;
        }
    }

    delay(10);
}
