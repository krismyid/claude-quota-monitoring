/*
 * Claude Code Quota Monitor — Wemos D1 Mini + 16x2 LCD (PCF8574 I2C backpack)
 *
 * Wiring:
 *   LCD SDA → D2 (GPIO4)
 *   LCD SCL → D1 (GPIO5)
 *   LCD VCC → 5V
 *   LCD GND → GND
 *
 * Libraries (install via Library Manager):
 *   LiquidCrystal_I2C, ArduinoJson (v6/v7), NTPClient
 *   ESP8266WiFi, ESP8266WebServer, WiFiUdp (all bundled with ESP8266 board package)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ArduinoJson.h>
#include <time.h>

// ── Config ────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
#define LCD_ADDR   0x27   // try 0x3F if display stays blank
#define TZ_OFFSET  25200  // UTC+7 Asia/Jakarta

// ── Hardware ──────────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
ESP8266WebServer  server(80);
WiFiUDP           ntpUDP;
WiFiUDP           dataUDP;
NTPClient         ntp(ntpUDP, "pool.ntp.org", TZ_OFFSET, 60000);
#define DATA_PORT 4210

// ── Custom characters (CGRAM slots 0-2) ───────────────────────────────────────
byte BLOCK_FULL[8]  = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
byte BLOCK_EMPTY[8] = {0x15,0x0A,0x15,0x0A,0x15,0x0A,0x15,0x0A}; // light shade
byte MIDDLE_DOT[8]  = {0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x00}; // · stale dot

#define CH_FULL  0
#define CH_EMPTY 1
#define CH_DOT   2

// ── Price state (fetched from Indodax) ────────────────────────────────────────
long  usdc_idr         = 0;
unsigned long price_ms = 0;
#define PRICE_INTERVAL 60000UL

// ── Quota state (populated by POST /update) ───────────────────────────────────
float five_pct         = 0;
int   five_reset_min   = 0;
float weekly_pct       = 0;
int   weekly_reset_min = 0;
unsigned long last_push_ms = 0;
bool  has_data         = false;

// ── Screen rotation ───────────────────────────────────────────────────────────
uint8_t cur_screen       = 0; // 0=datetime, 1=usage, 2=reset, 3=price
unsigned long screen_ms  = 0;
#define SCREEN_INTERVAL 4000UL

// ── Bar expand animation (screen 1 entry) ─────────────────────────────────────
bool  anim_active    = false;
float anim_five      = 0;
float anim_weekly    = 0;
unsigned long anim_ms = 0;
#define ANIM_STEP_MS 125UL

// ── Spinner / stale indicator ─────────────────────────────────────────────────
const char SPIN[] = {'|','/','-','\\'};
uint8_t spin_idx     = 0;
unsigned long spin_ms = 0;
#define SPIN_MS   250UL
#define STALE_MS 60000UL

// ── Backlight blink (alert: either pct >= 80%) ────────────────────────────────
bool  bl_on       = true;
unsigned long bl_ms = 0;
#define BLINK_MS 500UL

// ── Helpers ───────────────────────────────────────────────────────────────────

const char* dayName(int wday) {
    static const char* days[] = {
        "Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"
    };
    return days[wday % 7];
}

int pctToBlocks(float pct) {
    int b = (int)(pct / 100.0f * 8 + 0.5f);
    if (b < 0) b = 0;
    if (b > 8) b = 8;
    return b;
}

void drawBar(int filled) {
    for (int i = 0; i < 8; i++)
        lcd.write(i < filled ? CH_FULL : CH_EMPTY);
}

// Returns 7-char right-padded reset string, e.g. "4h20m  " or "now    "
String fmtReset(int min) {
    char buf[8];
    if (min <= 0) { strcpy(buf, "now    "); return buf; }
    int h = min / 60, m = min % 60;
    if (h > 0) snprintf(buf, 8, "%dh%02dm  ", h, m);
    else       snprintf(buf, 8, "%dm      ", m);
    buf[7] = '\0';
    return String(buf);
}

// ── Screen renderers (write cols 0-14; col 15 is indicator, written in loop) ──

void renderDateTime() {
    time_t t = ntp.getEpochTime();
    struct tm* ti = gmtime(&t);

    // Row 0: "YYYY-MM-DD     " (15 chars)
    char r0[16];
    snprintf(r0, 16, "%04d-%02d-%02d     ",
             ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday);
    r0[15] = '\0';

    // Row 1: "HH:MM DayName  " (15 chars; longest day "Wednesday"=9 → 5+1+9=15)
    char r1[16];
    snprintf(r1, 16, "%02d:%02d %-9s",
             ti->tm_hour, ti->tm_min, dayName(ti->tm_wday));
    r1[15] = '\0';

    lcd.setCursor(0, 0); lcd.print(r0);
    lcd.setCursor(0, 1); lcd.print(r1);
}

void renderUsage(float five_draw, float weekly_draw) {
    // Row 0: "5H:████░░░░ 12%" → 3 + 8 + 4 = 15 chars
    // Row 1: "7D:████░░░░  5%" → same layout
    char pct_buf[5];

    lcd.setCursor(0, 0);
    lcd.print("5H:");
    drawBar(pctToBlocks(five_draw));
    snprintf(pct_buf, 5, "%3d%%", (int)five_draw);
    lcd.print(pct_buf);

    lcd.setCursor(0, 1);
    lcd.print("7D:");
    drawBar(pctToBlocks(weekly_draw));
    snprintf(pct_buf, 5, "%3d%%", (int)weekly_draw);
    lcd.print(pct_buf);
}

void renderReset() {
    // Row 0: "5H rst:4h20m   " (7 + 7 + 1 pad = 15 chars)
    // Row 1: "7D rst:6h50m   "
    char r0[16], r1[16];
    snprintf(r0, 16, "5H rst:%-8s", fmtReset(five_reset_min).c_str());
    snprintf(r1, 16, "7D rst:%-8s", fmtReset(weekly_reset_min).c_str());
    r0[15] = '\0'; r1[15] = '\0';
    lcd.setCursor(0, 0); lcd.print(r0);
    lcd.setCursor(0, 1); lcd.print(r1);
}

void renderPrice() {
    // Row 0: "USDC/IDR       " (15 chars)
    // Row 1: "Rp 16.250      " (15 chars) or "Fetching...    " if no data yet
    lcd.setCursor(0, 0); lcd.print("USDC/IDR       ");
    lcd.setCursor(0, 1);
    if (usdc_idr == 0) {
        lcd.print("Fetching...    ");
    } else {
        // format with dot thousands: e.g. 16250 → "16.250"
        char num[12];
        long v = usdc_idr;
        if (v >= 1000000) snprintf(num, 12, "%ld.%03ld.%03ld", v/1000000, (v%1000000)/1000, v%1000);
        else if (v >= 1000) snprintf(num, 12, "%ld.%03ld", v/1000, v%1000);
        else snprintf(num, 12, "%ld", v);
        char r1[16];
        snprintf(r1, 16, "Rp %-12s", num);
        r1[15] = '\0';
        lcd.print(r1);
    }
}

void fetchPrice() {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://indodax.com/api/ticker/usdcidr");
    http.setTimeout(8000);
    if (http.GET() != HTTP_CODE_OK) { http.end(); return; }
    JsonDocument doc;
    if (deserializeJson(doc, http.getString())) { http.end(); return; }
    http.end();
    const char* last = doc["ticker"]["last"];
    if (last) usdc_idr = atol(last);
}

// ── HTTP handler ──────────────────────────────────────────────────────────────

void handleUpdate() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "no body");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "text/plain", "bad json");
        return;
    }
    five_pct         = doc["five_hour_pct"]       | 0.0f;
    five_reset_min   = doc["five_hour_resets_min"] | 0;
    weekly_pct       = doc["weekly_pct"]           | 0.0f;
    weekly_reset_min = doc["weekly_resets_min"]    | 0;
    last_push_ms     = millis();
    has_data         = true;
    server.send(200, "text/plain", "ok");
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Wire.begin(4, 5); // SDA=D2(GPIO4), SCL=D1(GPIO5)
    lcd.init();
    lcd.begin(16, 2);
    lcd.backlight();
    lcd.createChar(CH_FULL,  BLOCK_FULL);
    lcd.createChar(CH_EMPTY, BLOCK_EMPTY);
    lcd.createChar(CH_DOT,   MIDDLE_DOT);

    lcd.setCursor(0, 0); lcd.print("Connecting WiFi ");
    lcd.setCursor(0, 1); lcd.print(WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) delay(300);

    if (WiFi.status() != WL_CONNECTED) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("WiFi failed!    ");
        lcd.setCursor(0, 1); lcd.print("Check creds     ");
        while (true) delay(1000);
    }

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("IP:             ");
    String ip = WiFi.localIP().toString();
    lcd.setCursor(0, 1);
    lcd.print(ip);
    Serial.println("IP: " + ip);
    delay(3000);

    ntp.begin();
    ntp.update();

    server.on("/update", HTTP_POST, handleUpdate);
    server.begin();
    dataUDP.begin(DATA_PORT);

    screen_ms = millis();
    spin_ms   = millis();
}

// ── Loop ─────────────────────────────────────────────────────────────────────

void loop() {
    server.handleClient();
    ntp.update();

    // UDP quota packets
    int pktSize = dataUDP.parsePacket();
    if (pktSize > 0) {
        char buf[256];
        int len = dataUDP.read(buf, sizeof(buf) - 1);
        buf[len] = '\0';
        JsonDocument doc;
        if (!deserializeJson(doc, buf)) {
            five_pct         = doc["five_hour_pct"]        | 0.0f;
            five_reset_min   = doc["five_hour_resets_min"] | 0;
            weekly_pct       = doc["weekly_pct"]           | 0.0f;
            weekly_reset_min = doc["weekly_resets_min"]    | 0;
            last_push_ms     = millis();
            has_data         = true;
        }
    }

    unsigned long now = millis();

    // price fetch every minute
    if (now - price_ms >= PRICE_INTERVAL || price_ms == 0) {
        price_ms = now;
        fetchPrice();
    }

    // spinner tick
    if (now - spin_ms >= SPIN_MS) {
        spin_idx = (spin_idx + 1) % 4;
        spin_ms  = now;
    }

    // screen rotation
    if (now - screen_ms >= SCREEN_INTERVAL) {
        cur_screen = (cur_screen + 1) % 4;
        screen_ms  = now;
        if (cur_screen == 1) { // usage screen: start bar animation
            anim_active = true;
            anim_five   = 0;
            anim_weekly = 0;
            anim_ms     = now;
        }
    }

    // bar animation step
    if (anim_active && cur_screen == 1 && now - anim_ms >= ANIM_STEP_MS) {
        anim_ms     = now;
        float step  = 100.0f / 8; // one block width
        anim_five   = min(anim_five   + step, five_pct);
        anim_weekly = min(anim_weekly + step, weekly_pct);
        if (anim_five >= five_pct && anim_weekly >= weekly_pct)
            anim_active = false;
    }

    // backlight blink on alert
    bool alert = has_data && (five_pct >= 80.0f || weekly_pct >= 80.0f);
    if (alert) {
        if (now - bl_ms >= BLINK_MS) {
            bl_on = !bl_on;
            bl_ms = now;
            if (bl_on) lcd.backlight(); else lcd.noBacklight();
        }
    } else if (!bl_on) {
        lcd.backlight();
        bl_on = true;
    }

    // render active screen (cols 0-14)
    switch (cur_screen) {
        case 0: renderDateTime(); break;
        case 1: renderUsage(anim_active ? anim_five   : five_pct,
                            anim_active ? anim_weekly : weekly_pct); break;
        case 2: renderReset(); break;
        case 3: renderPrice(); break;
    }

    // indicator at col 15 on both rows
    bool stale = !has_data || (now - last_push_ms > STALE_MS);
    lcd.setCursor(15, 0);
    if (stale) lcd.write(CH_DOT); else lcd.print(SPIN[spin_idx]);
    lcd.setCursor(15, 1);
    if (stale) lcd.write(CH_DOT); else lcd.print(SPIN[spin_idx]);

    delay(50);
}
