// ORmeter meter firmware.
//
// The ONE supported device: Waveshare ESP32-S3-Touch-AMOLED-1.8, 368x448.
//
// Display layout ("money first"): the WALLET BALANCE is the hero at 72px and the
// percentages are demoted to small print. Every number names what it measures --
// a bare percentage never says what it is a percentage OF, which is what made an
// earlier version unreadable.
//
//   ORmeter                        OpenRouter tracking
//   WALLET
//   $20
//   79% left . resets in 11d
//   [============== wallet bar ==============]
//
//   MONTHLY LIMIT                                    11% used
//   $5.25 of $50
//   [==== spend bar ====]
//
// Payload contract, fixed by the host daemon:
//
//   {"s":11,"sr":15629,"tp":79,"rd":"$20","limit":50.0,"used":5.25,"ok":true}
//
//   s       spend percentage          tp     wallet percentage
//   sr      minutes until reset       rd     the balance, drawn VERBATIM
//   limit   the spend cap, dollars    used   dollars spent against it
//   ok      false means "no data" -- never render it as a zero reading
//
// `limit`/`used` are optional: an older daemon omits them and the row falls back
// to a bare percentage rather than showing a wrong figure.
//
// PAIRING: hold BOOT for 3 seconds and release; the screen counts down while you
// hold. No on-screen button, and no PWR-button handling -- the PWR line hangs off
// the IO expander on the same fragile I2C bus as the touch controller, and a ~10s
// hold on this board's AXP2101 powers the PMU down.
//
// WHY THE DEVICE ALSO PRESENTS A HID KEYBOARD -- not optional:
// OS Bluetooth device lists only surface a BLE-only peripheral that explicitly
// advertises the standard HID service UUID (0x1812). Without it the device is
// discovered internally but HIDDEN from the user-facing nearby-devices list, so
// it can never be paired, never gets a bond, and every connection is refused
// because it cannot be encrypted. A raw scanner still sees it perfectly, which
// makes the whole thing look like a host bug.
//
// Load-bearing details, every one learned on hardware:
//  1. INIT ORDER: probe + display BEFORE the PMU (XPowersLib's begin()
//     re-initialises the shared Wire bus and breaks the probe).
//  2. The revision probe reads an I2C register, with address AND size cast to
//     uint8_t (casting the address to int fails with ESP_ERR_INVALID_STATE).
//  3. The Flags AD type in the advertising packet is MANDATORY.
//  4. The advertised name must fit ~8 characters or NimBLE drops it silently.
//  5. Round the flush area on LV_EVENT_INVALIDATE_AREA, not in the flush.
//  6. Set the colour format explicitly (LVGL is a library).
//  7. The first-frame render burst must run AFTER ble_init().
//  8. FLASH THE APP ONLY (0x10000). Flashing the merged factory image at 0x0
//     spans the NVS partition at 0x9000 and erases the BLE bond on every flash,
//     after which the OS cannot encrypt and drops every connection. See
//     docs/flashing.md.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <lvgl.h>

extern "C" {
extern const lv_font_t font_inter_14;
extern const lv_font_t font_inter_16;
extern const lv_font_t font_inter_20;
extern const lv_font_t font_inter_32;
extern const lv_font_t font_inter_72;
}

#define PANEL_SELFTEST 0

// Debug: force the first-run prompt even on a device that already has a bond, so
// the onboarding screen can be checked without clearing the pairing. Left at 0.
#define FORCE_WELCOME 0

// --- geometry / pins -------------------------------------------------------
#define LCD_WIDTH 368
#define LCD_HEIGHT 448

#define LCD_CS 12
#define LCD_SCLK 11
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7

#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 21
#define BTN_BOOT_GPIO 0

#define AXP2101_ADDR 0x34
#define XCA9554_ADDR 0x20

#define IOX_PIN_TP_RST 0
#define IOX_PIN_LCD_RST 1
#define IOX_PIN_PA_EN 2
#define IOX_PIN_PWR_BTN 4

#define FT3168_ADDR 0x38
#define CST8XX_ADDR 0x15

#define PAIR_HOLD_MS 3000
#define PAIR_WINDOW_MS 300000

// --- BLE contract ----------------------------------------------------------
// Custom GATT namespace, spelled "ORmeterBLE" in hex. Must match the host
// daemon's SERVICE_UUID / RX_CHAR_UUID / REQ_CHAR_UUID: the daemon WRITES the
// JSON payload to RX, and this firmware NOTIFIES a refresh request on REQ.
#define SERVICE_UUID "4f526d65-7465-7242-4c45-000000000001"
#define RX_CHAR_UUID "4f526d65-7465-7242-4c45-000000000002"
#define REQ_CHAR_UUID "4f526d65-7465-7242-4c45-000000000004"

#define DEVICE_NAME "ORmeter"     // must fit ~8 chars next to the 128-bit UUID
#define APPEARANCE_KEYBOARD 0x03C0

// --- palette ---------------------------------------------------------------
#define COL_BG 0x000000
#define COL_TEXT 0xF2F2F2
#define COL_DIM 0x9A9A9A
#define COL_TRACK 0x262626
#define COL_ACCENT 0x4FA3FF
#define COL_WARN 0xE8A33D
#define COL_HOT 0xE5544B

// --- layout ----------------------------------------------------------------
#define MARGIN 24
#define CONTENT_W (LCD_WIDTH - 2 * MARGIN)
#define BAR_H 16          // thick enough to read at a glance across a desk

// Battery indicator geometry, and the width reserved for its percentage text.
// Reserving a fixed width keeps the icon's position stable as the reading
// changes ("9%" vs "100%") without needing a flex layout.
#define BATT_W 26
#define BATT_H 13
#define BATT_PAD 2

// Warn when the wallet is nearly gone or the limit is nearly consumed.
#define ALARM_WALLET_PCT 5      // wallet remaining at or below this
#define ALARM_SPEND_PCT 95      // spend consumed at or above this
#define ALARM_BORDER_W 6

// --- HID report descriptor -------------------------------------------------
// Standard 6-key-rollover boot-protocol keyboard from the USB HID Usage Tables:
// modifier byte, reserved byte, six key codes in; a 5-bit LED report out. The
// LED output report is required for macOS to present it as a full keyboard.
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
    0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x91, 0x01, 0xC0
};

// --- state -----------------------------------------------------------------
enum BoardRev { REV_SH8601_FT3168, REV_CO5300_CST8XX };

static BoardRev g_rev = REV_SH8601_FT3168;
static Arduino_DataBus *g_bus = nullptr;
static Arduino_OLED *g_gfx = nullptr;
static XPowersPMU g_pmu;
static lv_display_t *g_disp = nullptr;
static NimBLEServer *g_server = nullptr;
static NimBLEHIDDevice *g_hid = nullptr;

static lv_obj_t *lbl_wallet_cap = nullptr;   // "WALLET"
static lv_obj_t *lbl_balance = nullptr;      // the hero
static lv_obj_t *lbl_wallet_sub = nullptr;   // "79% left . resets in 11d"
static lv_obj_t *bar_wallet = nullptr;
static lv_obj_t *lbl_limit_cap = nullptr;    // "MONTHLY LIMIT"
static lv_obj_t *lbl_limit_amt = nullptr;    // "$5.25 of $50"
static lv_obj_t *lbl_spend_pct = nullptr;    // "11% used"
static lv_obj_t *bar_spend = nullptr;
static lv_obj_t *lbl_banner = nullptr;       // hold-to-pair + pairing mode
static lv_obj_t *batt_body = nullptr;        // battery outline
static lv_obj_t *batt_fill = nullptr;        // battery level fill
static lv_obj_t *batt_nub = nullptr;         // battery terminal
static lv_obj_t *lbl_batt = nullptr;         // battery percentage

static volatile bool g_have_data = false;
static bool g_pairing = false;

// Onboarding state. g_welcome is owned by the UI (setup/loop); the two volatile
// flags are raised from BLE callbacks and consumed in loop(), because calling
// LVGL from the BLE task is not safe.
static bool g_welcome = false;                    // showing the first-run prompt
static volatile bool g_bond_established = false;  // raised by onAuthenticationComplete
static volatile bool g_data_arrived = false;      // raised by the RX write callback
static uint32_t g_pairing_until = 0;
static NimBLECharacteristic *g_req_char = nullptr;

// --- touch (kept initialised for future on-screen controls) ----------------
static volatile bool g_touch_ready = false;
static bool g_touch_pressed = false;
static uint16_t g_touch_x = 0;
static uint16_t g_touch_y = 0;
static uint8_t g_touch_addr = FT3168_ADDR;

static void IRAM_ATTR touch_isr() { g_touch_ready = true; }

static void touch_init() {
    g_touch_addr = (g_rev == REV_CO5300_CST8XX) ? CST8XX_ADDR : FT3168_ADDR;
    if (g_rev != REV_CO5300_CST8XX) {
        Wire.beginTransmission(g_touch_addr);   // FT3168: 0xA5=0x00 active scan
        Wire.write(0xA5);
        Wire.write(0x00);
        Wire.endTransmission();
    }
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);
    Serial.printf("Touch init: addr 0x%02X, INT on GPIO %d\n", g_touch_addr, TP_INT);
}

static void touch_poll() {
    if (!g_touch_ready) return;
    g_touch_ready = false;
    Wire.beginTransmission(g_touch_addr);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) { g_touch_pressed = false; return; }
    if (Wire.requestFrom((uint8_t)g_touch_addr, (uint8_t)5) != 5) {
        g_touch_pressed = false; return;
    }
    const uint8_t fingers = Wire.read() & 0x0F;
    const uint8_t xh = Wire.read();
    const uint8_t xl = Wire.read();
    const uint8_t yh = Wire.read();
    const uint8_t yl = Wire.read();
    if (fingers == 0 || fingers > 5) { g_touch_pressed = false; return; }
    g_touch_x = (uint16_t)(((uint16_t)(xh & 0x0F) << 8) | xl);
    g_touch_y = (uint16_t)(((uint16_t)(yh & 0x0F) << 8) | yl);
    g_touch_pressed = true;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    if (g_touch_pressed) {
        data->point.x = g_touch_x;
        data->point.y = g_touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// --- IO expander -----------------------------------------------------------
static void iox_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(XCA9554_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static void iox_init() {
    iox_write(0x03, (uint8_t)(1u << IOX_PIN_PWR_BTN));
    uint8_t out = (uint8_t)(1u << IOX_PIN_PA_EN);   // PA_EN must stay HIGH
    iox_write(0x01, out);
    delay(10);
    out |= (uint8_t)((1u << IOX_PIN_TP_RST) | (1u << IOX_PIN_LCD_RST));
    iox_write(0x01, out);
    Serial.println("XCA9554 init OK");
}

// --- I2C + revision probe --------------------------------------------------
static bool i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *out) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)addr, (uint8_t)1) != 1) return false;
    *out = (uint8_t)Wire.read();
    return true;
}

static bool probe_touch(uint8_t addr, uint8_t id_reg, uint8_t *id_out) {
    for (int attempt = 0; attempt < 5; attempt++) {
        uint8_t id = 0;
        if (i2c_read_reg(addr, id_reg, &id)) { *id_out = id; return true; }
        delay(60);
    }
    return false;
}

static BoardRev detect_panel_revision() {
    delay(150);
    uint8_t id = 0;
    if (probe_touch(FT3168_ADDR, 0xA0, &id)) {
        Serial.printf("Board revision: SH8601 + FT3168 (id 0x%02X)\n", id);
        return REV_SH8601_FT3168;
    }
    if (probe_touch(CST8XX_ADDR, 0xA7, &id)) {
        Serial.printf("Board revision: CO5300 + CST8xx (id 0x%02X)\n", id);
        return REV_CO5300_CST8XX;
    }
    Serial.println("Board revision: PROBE FAILED -- falling back to SH8601");
    return REV_SH8601_FT3168;
}

// --- display ---------------------------------------------------------------
static void display_init() {
    g_bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    if (g_rev == REV_CO5300_CST8XX) {
        g_gfx = new Arduino_CO5300(g_bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);
    } else {
        g_gfx = new Arduino_SH8601(g_bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);
    }
    const bool ok = g_gfx->begin();
    Serial.printf("Display init: %s, %dx%d (begin=%s)\n",
                  g_rev == REV_CO5300_CST8XX ? "CO5300" : "SH8601", LCD_WIDTH, LCD_HEIGHT,
                  ok ? "ok" : "FAILED");
    g_gfx->fillScreen(0x0000);
    g_gfx->setBrightness(200);
}

// --- LVGL plumbing ---------------------------------------------------------
static void lv_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const uint32_t w = lv_area_get_width(area);
    const uint32_t h = lv_area_get_height(area);
    g_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t *e) {
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    area->x1 &= ~1; area->y1 &= ~1;
    area->x2 |= 1;  area->y2 |= 1;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// Small dim caps label with a little letter-spacing, as a section marker.
static lv_obj_t *make_caps(lv_obj_t *parent, const char *text) {
    lv_obj_t *l = make_label(parent, &font_inter_20, COL_DIM, text);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

static void style_bar(lv_obj_t *bar, int32_t w) {
    lv_obj_set_size(bar, w, BAR_H);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
}

static uint32_t severity_color(int pct) {
    if (pct >= 90) return COL_HOT;
    if (pct >= 75) return COL_WARN;
    return COL_ACCENT;
}

// "15629" minutes -> "10d" / "4h 21m": the quota reset can be days away, so a
// raw minute count is useless on screen.
static void format_reset(int minutes, char *out, size_t n) {
    if (minutes <= 0) { snprintf(out, n, "due now"); return; }
    if (minutes >= 1440)      snprintf(out, n, "%dd", minutes / 1440);
    else if (minutes >= 60)   snprintf(out, n, "%dh %dm", minutes / 60, minutes % 60);
    else                      snprintf(out, n, "%dm", minutes);
}

// --- pairing ---------------------------------------------------------------
static void banner_show(const char *text, bool large) {
    lv_obj_set_style_text_font(lbl_banner, large ? &font_inter_32 : &font_inter_20, 0);
    lv_obj_clear_flag(lbl_banner, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl_banner, text);
}

static void banner_hide() { lv_obj_add_flag(lbl_banner, LV_OBJ_FLAG_HIDDEN); }

// The dashboard's own widgets, hidden while the first-run prompt is up so the
// customer is not shown an empty balance and two blank bars.
static lv_obj_t *const DASHBOARD_WIDGETS[] = {
    lbl_wallet_cap, lbl_balance, lbl_wallet_sub, bar_wallet,
    lbl_limit_cap, lbl_limit_amt, lbl_spend_pct, bar_spend,
};
static void dashboard_show(bool visible) {
    for (lv_obj_t *o : DASHBOARD_WIDGETS) {
        if (o == nullptr) continue;
        if (visible) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void enter_pairing_mode() {
    const int before = NimBLEDevice::getNumBonds();
    const int cleared = NimBLEDevice::deleteAllBonds();
    Serial.printf("PAIR: bonds before=%d after=%d (deleteAllBonds=%d)\n",
                  before, NimBLEDevice::getNumBonds(), cleared);

    g_pairing = true;
    g_pairing_until = millis() + PAIR_WINDOW_MS;
    banner_show("PAIRING MODE\nlook for ORmeter", false);

    NimBLEDevice::stopAdvertising();
    delay(20);
    const bool adv = NimBLEDevice::startAdvertising();
    Serial.printf("PAIR: startAdvertising=%d isAdvertising=%d\n",
                  adv ? 1 : 0, NimBLEDevice::getAdvertising()->isAdvertising() ? 1 : 0);
}

static void exit_pairing_mode(const char *why) {
    if (!g_pairing) return;
    g_pairing = false;
    banner_hide();
    Serial.printf("PAIR: leaving pairing mode (%s)\n", why);
    NimBLEDevice::startAdvertising();
}

// Hold BOOT for PAIR_HOLD_MS and release.
static void boot_btn_tick() {
    static bool was_pressed = false;
    static uint32_t press_started = 0;

    const bool pressed = (digitalRead(BTN_BOOT_GPIO) == LOW);

    if (pressed && !was_pressed) {
        press_started = millis();
    } else if (pressed && was_pressed) {
        const uint32_t held = millis() - press_started;
        if (held >= 400 && !g_pairing) {
            const int secs = (int)((PAIR_HOLD_MS - held + 999) / 1000);
            char b[40];
            if (secs > 0) snprintf(b, sizeof(b), "hold to pair: %d", secs);
            else          snprintf(b, sizeof(b), "release to pair");
            banner_show(b, false);
        }
    } else if (!pressed && was_pressed) {
        const uint32_t held = millis() - press_started;
        if (held >= PAIR_HOLD_MS) {
            enter_pairing_mode();
        } else if (!g_pairing) {
            banner_hide();
            Serial.printf("BTN: BOOT short press (%lums)\n", (unsigned long)held);
        }
    }
    was_pressed = pressed;
}

// --- battery indicator -----------------------------------------------------
// Drawn from primitives rather than an icon glyph, so it does not depend on any
// symbol font being compiled in.
static uint32_t battery_color(int pct) {
    if (pct <= 15) return COL_HOT;
    if (pct <= 35) return COL_WARN;
    return COL_ACCENT;
}

static void battery_build(lv_obj_t *parent) {
    batt_body = lv_obj_create(parent);
    lv_obj_set_size(batt_body, BATT_W, BATT_H);
    lv_obj_set_style_radius(batt_body, 3, 0);
    lv_obj_set_style_border_width(batt_body, 2, 0);
    lv_obj_set_style_border_color(batt_body, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(batt_body, 0, 0);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE);

    batt_fill = lv_obj_create(batt_body);
    lv_obj_set_height(batt_fill, BATT_H - 2 * BATT_PAD - 4);
    lv_obj_set_width(batt_fill, 0);
    lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(batt_fill, 1, 0);
    lv_obj_set_style_border_width(batt_fill, 0, 0);
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    // Terminal nub, offset so it sits against the body's right edge.
    batt_nub = lv_obj_create(parent);
    lv_obj_set_size(batt_nub, 3, 6);
    lv_obj_set_style_radius(batt_nub, 1, 0);
    lv_obj_set_style_bg_color(batt_nub, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_border_width(batt_nub, 0, 0);
    lv_obj_clear_flag(batt_nub, LV_OBJ_FLAG_SCROLLABLE);

    // Deliberately no explicit width. Giving a label a width smaller than its
    // text makes LVGL wrap it, which split "100%" across two lines. Auto-sized
    // it never wraps; battery_layout() below then positions the icon from the
    // width the text actually needs, so the two stay tight at "9%" and "100%".
    lbl_batt = make_label(parent, &font_inter_14, COL_TEXT, "--%");
}

// Pack the icon against the percentage using its REAL width. Called after every
// text change, and once at build time.
static void battery_layout() {
    lv_obj_update_layout(lbl_batt);          // force the new width to take
    const int32_t tw = lv_obj_get_width(lbl_batt);
    lv_obj_align(batt_nub, LV_ALIGN_TOP_RIGHT, -(MARGIN + tw + 2), 24);
    lv_obj_align(batt_body, LV_ALIGN_TOP_RIGHT, -(MARGIN + tw + 6), 20);
}

// PMU reads are cheap but pointless at 10Hz, so throttle them. Hides the whole
// indicator when no cell is attached -- this board runs fine on USB alone, and a
// permanent "0%" would read as a fault rather than an absence.
static void battery_tick() {
    static uint32_t last = 0;
    if (millis() - last < 10000) return;
    last = millis();

    if (!g_pmu.isBatteryConnect()) {
        lv_obj_add_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(batt_nub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batt_nub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);

    int pct = (int)g_pmu.getBatteryPercent();
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(lbl_batt, buf);
    battery_layout();

    const int inner_max = BATT_W - 2 * BATT_PAD - 4;
    lv_obj_set_width(batt_fill, (inner_max * pct) / 100);
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(battery_color(pct)), 0);
}

// --- UI (money first) ------------------------------------------------------
static void ui_build() {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // ---- masthead: brand left, tagline centred, battery right
    lv_obj_t *lbl_brand = make_label(scr, &font_inter_20, COL_TEXT, "ORmeter");
    lv_obj_set_style_text_letter_space(lbl_brand, 1, 0);
    lv_obj_align(lbl_brand, LV_ALIGN_TOP_LEFT, MARGIN, 18);

    // The tagline was dropped from this row: three items do not fit 368px, and
    // it met both the brand and the battery. The masthead is now brand + battery
    // only.
    //
    // Right-hand group, laid out from the right edge inwards and packed tight:
    // the percentage reserves a fixed width so the icon never shifts between
    // "9%" and "100%", then the terminal nub, then the body.
    lbl_batt = nullptr;  // created by battery_build()
    battery_build(scr);
    lv_obj_align(lbl_batt, LV_ALIGN_TOP_RIGHT, -MARGIN, 20);
    battery_layout();
    lv_obj_add_flag(batt_body, LV_OBJ_FLAG_HIDDEN);   // until the PMU is read
    lv_obj_add_flag(batt_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(batt_nub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_batt, LV_OBJ_FLAG_HIDDEN);

    // ---- WALLET: the hero
    lbl_wallet_cap = make_caps(scr, "WALLET");
    lv_obj_align(lbl_wallet_cap, LV_ALIGN_TOP_LEFT, MARGIN, 58);

    lbl_balance = make_label(scr, &font_inter_72, COL_TEXT, "--");
    lv_obj_align(lbl_balance, LV_ALIGN_TOP_LEFT, MARGIN, 84);

    lbl_wallet_sub = make_label(scr, &font_inter_20, COL_DIM, "--% left");
    lv_obj_align(lbl_wallet_sub, LV_ALIGN_TOP_LEFT, MARGIN, 176);

    bar_wallet = lv_bar_create(scr);
    style_bar(bar_wallet, CONTENT_W);
    lv_obj_align(bar_wallet, LV_ALIGN_TOP_LEFT, MARGIN, 210);

    // ---- MONTHLY LIMIT: what the bottom percentage is a percentage OF
    lbl_limit_cap = make_caps(scr, "MONTHLY LIMIT");
    lv_obj_align(lbl_limit_cap, LV_ALIGN_BOTTOM_LEFT, MARGIN, -116);

    lbl_limit_amt = make_label(scr, &font_inter_20, COL_DIM, "-- of --");
    lv_obj_align(lbl_limit_amt, LV_ALIGN_BOTTOM_LEFT, MARGIN, -74);

    // Same size as the dollar line it sits opposite, so the two read as a single
    // row: "MONTHLY LIMIT / $5.45 of $50 ... 11% used". White rather than accent
    // so the figure reads as a plain number, not a status warning.
    lbl_spend_pct = make_label(scr, &font_inter_20, COL_TEXT, "--% used");
    lv_obj_align(lbl_spend_pct, LV_ALIGN_BOTTOM_RIGHT, -MARGIN, -74);

    bar_spend = lv_bar_create(scr);
    style_bar(bar_spend, CONTENT_W);
    lv_obj_align(bar_spend, LV_ALIGN_BOTTOM_LEFT, MARGIN, -36);

    // ---- banner: hold-to-pair countdown and pairing mode
    lbl_banner = make_label(scr, &font_inter_20, COL_ACCENT, "");
    lv_label_set_long_mode(lbl_banner, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_banner, CONTENT_W);
    lv_obj_set_style_text_align(lbl_banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_banner, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(lbl_banner, LV_OBJ_FLAG_HIDDEN);
}

// --- payload ---------------------------------------------------------------
static bool apply_payload(const char *json, size_t len) {
    static JsonDocument doc;
    doc.clear();
    const DeserializationError err = deserializeJson(doc, json, len);
    if (err) {
        Serial.printf("payload: JSON parse failed: %s\n", err.c_str());
        return false;
    }

    if (doc["ok"].is<bool>() && doc["ok"].as<bool>() == false) {
        g_have_data = false;
        Serial.println("payload: ok=false -> idle");
        return true;
    }

    const int s = doc["s"] | 0;
    const int tp = doc["tp"] | 0;
    const int sr = doc["sr"] | 0;
    const char *rd = doc["rd"] | "";
    const bool have_amounts = doc.containsKey("limit") && doc.containsKey("used");
    const float limit = doc["limit"] | 0.0f;
    const float used = doc["used"] | 0.0f;

    char buf[64];

    // Balance is the hero, drawn verbatim from the payload.
    lv_label_set_text(lbl_balance, rd);

    char resets[24];
    format_reset(sr, resets, sizeof(resets));
    snprintf(buf, sizeof(buf), "%d%% left  .  resets in %s", tp, resets);
    lv_label_set_text(lbl_wallet_sub, buf);

    lv_bar_set_value(bar_wallet, tp, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_wallet, lv_color_hex(severity_color(100 - tp)), LV_PART_INDICATOR);

    snprintf(buf, sizeof(buf), "%d%% used", s);
    lv_label_set_text(lbl_spend_pct, buf);

    // Name the denominator when we have it; otherwise stay honest and show only
    // the percentage rather than inventing a figure.
    if (have_amounts) {
        snprintf(buf, sizeof(buf), "$%.2f of $%.0f", (double)used, (double)limit);
        lv_label_set_text(lbl_limit_amt, buf);
    } else {
        lv_label_set_text(lbl_limit_amt, "of the monthly limit");
    }

    lv_bar_set_value(bar_spend, s, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_spend, lv_color_hex(severity_color(s)), LV_PART_INDICATOR);

    // Loud, unmissable, and impossible to confuse with a data reading: a red
    // frame around the whole panel when either figure is nearly exhausted.
    const bool alarm = (tp <= ALARM_WALLET_PCT) || (s >= ALARM_SPEND_PCT);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_border_color(scr, lv_color_hex(COL_HOT), 0);
    lv_obj_set_style_border_opa(scr, alarm ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scr, alarm ? ALARM_BORDER_W : 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    Serial.printf("alarm: wallet %d%% remaining, spend %d%% used -> border %s\n",
                  tp, s, alarm ? "ON" : "off");

    g_have_data = true;
    Serial.printf("payload: s=%d tp=%d sr=%d rd=\"%s\" limit=%.2f used=%.2f\n",
                  s, tp, sr, rd, (double)limit, (double)used);
    return true;
}

static void request_refresh() {
    if (g_req_char == nullptr) return;
    const uint8_t req = 0x01;
    g_req_char->setValue(&req, 1);
    g_req_char->notify();
    Serial.println("BLE: refresh requested");
}

// --- BLE -------------------------------------------------------------------
// Advertising restarts are deferred to loop(); issuing startAdvertising() from
// inside onConnect was observed to destabilise a fresh connection.
static volatile bool g_need_advertise = false;
static uint32_t g_advertise_at = 0;
static void need_advertise() {
    g_advertise_at = millis() + 300;
    g_need_advertise = true;
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
        (void)info;
        const std::string v = c->getValue();
        Serial.printf("BLE: rx %u bytes\n", (unsigned)v.size());
        g_data_arrived = true;
        apply_payload(v.data(), v.size());
    }
};

class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &info, uint16_t subValue) override {
        (void)c; (void)info;
        Serial.printf("BLE: req_char onSubscribe subValue=%u has_data=%d\n",
                      subValue, g_have_data ? 1 : 0);
        if (subValue == 1 && !g_have_data) request_refresh();
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
        Serial.printf("BLE: connected from %s (active=%d, bonded=%d, encrypted=%d)\n",
                      info.getAddress().toString().c_str(), s->getConnectedCount(),
                      info.isBonded() ? 1 : 0, info.isEncrypted() ? 1 : 0);
        need_advertise();
    }
    void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
        Serial.printf("BLE: disconnected from %s (reason=%d, remaining=%u)\n",
                      info.getAddress().toString().c_str(), reason, s->getConnectedCount());
        need_advertise();
    }
    void onAuthenticationComplete(NimBLEConnInfo &info) override {
        Serial.printf("BLE: auth complete bonded=%d encrypted=%d\n",
                      info.isBonded() ? 1 : 0, info.isEncrypted() ? 1 : 0);
        // No LVGL here: this runs on the BLE task. loop() does the UI work.
        if (info.isBonded()) g_bond_established = true;
    }
};

static void ble_init() {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks());

    // HID keyboard service -- what makes the device visible/pairable. See header.
    g_hid = new NimBLEHIDDevice(g_server);
    g_hid->setReportMap((uint8_t *)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    g_hid->setManufacturer("ORmeter");
    // PnP identity: Bluetooth SIG signature, an explicitly reserved vendor id
    // (0xFFFF means "testing"), and our own product id. The previous 0x02E5 was
    // Apple's USB vendor id -- borrowed, like the GATT UUIDs, from the reference
    // firmware. Windows resolves HID by class, so a third-party vendor id buys
    // nothing and is not ours to claim.
    g_hid->setPnp(0x01, 0xFFFF, 0x0001, 0x0100);
    g_hid->setHidInfo(0x21, 0x02);
    g_hid->setBatteryLevel(100);
    g_hid->getInputReport(1);

    NimBLEService *svc = g_server->createService(SERVICE_UUID);

    NimBLECharacteristic *rx = svc->createCharacteristic(
        RX_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    static RxCallbacks rxCb;
    rx->setCallbacks(&rxCb);

    g_req_char = svc->createCharacteristic(
        REQ_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    static ReqCallbacks reqCb;
    g_req_char->setCallbacks(&reqCb);

    svc->start();
    g_server->start();

    // Advertising: the fields that make the device VISIBLE go in the primary
    // packet (flags mandatory; 0x1812 is what OS device lists require); the
    // custom 128-bit UUID goes in the scan response, which has its own budget.
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->reset();
    adv->setAppearance(APPEARANCE_KEYBOARD);
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));
    adv->setName(DEVICE_NAME);

    NimBLEAdvertisementData scanResp;
    scanResp.addServiceUUID(SERVICE_UUID);
    adv->setScanResponseData(scanResp);
    adv->enableScanResponse(true);

    adv->setMinInterval(160);   // 0.625ms units; 0 would mean the ~1.28s default
    adv->setMaxInterval(240);

    NimBLEDevice::startAdvertising();

    Serial.printf("BLE: init complete, MAC=%s, adv=%u bytes, isAdvertising=%d, bonds=%d\n",
                  NimBLEDevice::getAddress().toString().c_str(),
                  (unsigned)adv->getAdvertisementData().getPayload().size(),
                  adv->isAdvertising() ? 1 : 0,
                  NimBLEDevice::getNumBonds());
}

// --- setup / loop ----------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("=== ORmeter meter firmware ===");

    Wire.begin(IIC_SDA, IIC_SCL);
    iox_init();

    // Probe + display FIRST: the PMU re-initialises the shared Wire bus.
    g_rev = detect_panel_revision();
    display_init();

    if (g_pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        g_pmu.enableBattDetection();
        g_pmu.enableBattVoltageMeasure();
        Serial.println("AXP2101 init OK");
    } else {
        Serial.println("AXP2101 init FAILED (continuing)");
    }

#if PANEL_SELFTEST
    Serial.println("PANEL SELFTEST: red");   g_gfx->fillScreen(0xF800); delay(1200);
    Serial.println("PANEL SELFTEST: green"); g_gfx->fillScreen(0x07E0); delay(1200);
    Serial.println("PANEL SELFTEST: blue");  g_gfx->fillScreen(0x001F); delay(1200);
    Serial.println("PANEL SELFTEST: done");
#endif

    touch_init();
    pinMode(BTN_BOOT_GPIO, INPUT_PULLUP);

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    g_disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    static uint8_t draw_buf[LCD_WIDTH * 40 * 2];
    lv_display_set_flush_cb(g_disp, lv_flush_cb);
    lv_display_set_buffers(g_disp, draw_buf, nullptr, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(g_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    ui_build();
    lv_obj_invalidate(lv_screen_active());

    ble_init();

    // Prime the first frame. MUST come after ble_init().
    for (int i = 0; i < 30; i++) {
        lv_timer_handler();
        delay(10);
    }

    // First-run state. A device with no bonds has never been paired, so prompt
    // for it instead of showing an empty dashboard. Self-clearing: once a bond
    // exists this branch never runs again, and no flag has to be stored.
    if (FORCE_WELCOME || NimBLEDevice::getNumBonds() == 0) {
        g_welcome = true;
        dashboard_show(false);
        banner_show("Pair me over Bluetooth\n\nAdd a device called ORmeter", true);
        Serial.println("First run: no bonds, showing the pair-me prompt");
    }

    Serial.printf("Dashboard ready (%dx%d), waiting for data on BLE...\n", LCD_WIDTH, LCD_HEIGHT);
}

void loop() {
    lv_timer_handler();
    touch_poll();
    boot_btn_tick();
    battery_tick();

    if (g_need_advertise && (int32_t)(millis() - g_advertise_at) >= 0) {
        g_need_advertise = false;
        NimBLEDevice::startAdvertising();
    }

    if (g_bond_established) {
        g_bond_established = false;
        exit_pairing_mode("bond established");
        if (g_welcome) {
            g_welcome = false;
            dashboard_show(true);
        }
        banner_show("Paired\n\nwaiting for data", false);
        Serial.println("Onboarding: paired, waiting for the first payload");
    }

    if (g_data_arrived) {
        g_data_arrived = false;
        if (g_welcome) {
            g_welcome = false;
            dashboard_show(true);
            banner_hide();
        }
    }

    if (g_pairing && (int32_t)(millis() - g_pairing_until) >= 0) {
        exit_pairing_mode("window expired");
    }

    delay(5);
}
