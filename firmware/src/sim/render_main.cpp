// ORmeter host-side render harness.
//
// Answers layout questions WITHOUT touching the device: no flash, no BLE, and
// no contention with the daemon that holds the GATT link. Renders LVGL into a
// memory buffer at the panel's real geometry, writes a BMP per case, and prints
// the text metrics behind the layout decision.
//
//   pio run -e sim
//   .pio/build/sim/program
//
// Fonts are OUR OWN: Inter and JetBrains Mono, both SIL OFL 1.1, generated into
// firmware/src/fonts/ with lv_font_conv --no-compress. They are compiled into
// this harness too, so the numbers are about the same glyphs the device renders
// -- measuring a stand-in font would answer a question about that font instead.
//
// Why this exists: the caption on the bottom row was clipped by the panel edge,
// because the font was sized against a widest-case CONSTANT rather than the
// string actually sent, and nothing constrained the label to the space left
// over. Two things follow, and this harness measures both:
//   * is the caption too wide for the space it gets, at each candidate size?
//   * does dropping the compiled-in "Resets " verb remove the problem outright?

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lvgl.h"

// Our generated fonts. The .c files are C, so declare them with C linkage.
extern "C" {
extern const lv_font_t font_inter_14;
extern const lv_font_t font_inter_16;
extern const lv_font_t font_inter_18;
extern const lv_font_t font_inter_20;
extern const lv_font_t font_inter_24;
extern const lv_font_t font_inter_28;
extern const lv_font_t font_jbmono_20;
}

// The ONE supported panel. Hardcoded deliberately: no board matrix, no
// resolution-agnostic layout to maintain.
static const int PANEL_W = 368;
static const int PANEL_H = 448;

static const char *OUT_DIR = "sim-out";

// The strings under test. The first group is what the OLD layout produced (a
// compiled-in "Resets " verb plus the payload's tail); the second group is what
// OUR layout would produce, with the verb dropped. The comparison between them
// is the point.
static const char *CAPTIONS[] = {
    "Under pace - Resets May 30",  // the old widest-case constant
    "Resets $24.02",               // old style, with cents
    "Resets $21",                  // old style, what the daemon sends now
    "Resets $$$$$$$$",             // old style, deliberately too wide
    "$24.02",                      // ours: verb dropped, with cents
    "$21",                         // ours: verb dropped
    "$$$$$$$$",                    // ours: verb dropped, still worst case
};
static const int CAPTION_COUNT = (int)(sizeof(CAPTIONS) / sizeof(CAPTIONS[0]));

// Fixed furniture on the bottom row: a pill, the percentage, and a bar. Only the
// caption tail is free text. Both texts are ours now -- "Period" was the old
// compiled-in pill label, and "Wallet" is what we are changing it to.
static const char *PILL_TEXT = "Wallet";
static const char *PCT_TEXT = "88%";
static const int BAR_W = 96;
static const int ROW_GAP = 8;
static const int ROW_PAD = 6;

static std::vector<uint16_t> g_fb;
static lv_obj_t *g_root = nullptr;    // holds the row under test; cleaned per case
static lv_obj_t *g_measure = nullptr; // off-screen label used for measurement

struct FontCase {
    const char *name;
    const lv_font_t *font;
};

static const FontCase FONTS[] = {
    {"14", &font_inter_14},
    {"16", &font_inter_16},
    {"18", &font_inter_18},
    {"20", &font_inter_20},
    {"24", &font_inter_24},
    {"28", &font_inter_28},
};
static const int FONT_COUNT = (int)(sizeof(FONTS) / sizeof(FONTS[0]));

// --- framebuffer -----------------------------------------------------------

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const uint32_t w = (uint32_t)lv_area_get_width(area);
    const uint32_t h = (uint32_t)lv_area_get_height(area);
    const uint16_t *src = reinterpret_cast<const uint16_t *>(px_map);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const int px = area->x1 + (int)x;
            const int py = area->y1 + (int)y;
            if (px >= 0 && px < PANEL_W && py >= 0 && py < PANEL_H) {
                g_fb[(size_t)py * PANEL_W + px] = src[y * w + x];
            }
        }
    }
    lv_display_flush_ready(disp);
}

// 24-bit uncompressed BMP, hand-rolled so the harness needs no image library
// (and therefore nothing extra for CI to install).
static bool write_bmp(const char *path, const std::vector<uint16_t> &fb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return false;
    }
    const int row_bytes = (w * 3 + 3) & ~3;
    const int data_size = row_bytes * h;
    const int file_size = 54 + data_size;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(&hdr[2], &file_size, 4);
    const int offset = 54;
    memcpy(&hdr[10], &offset, 4);
    const int dib = 40;
    memcpy(&hdr[14], &dib, 4);
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    const uint16_t planes = 1;
    const uint16_t bpp = 24;
    memcpy(&hdr[26], &planes, 2);
    memcpy(&hdr[28], &bpp, 2);
    memcpy(&hdr[34], &data_size, 4);
    fwrite(hdr, 1, 54, f);

    std::vector<uint8_t> row((size_t)row_bytes, 0);
    for (int y = h - 1; y >= 0; y--) {  // BMP rows run bottom-up
        for (int x = 0; x < w; x++) {
            const uint16_t c = fb[(size_t)y * w + x];
            const uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
            const uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
            const uint8_t b5 = (uint8_t)(c & 0x1F);
            row[(size_t)x * 3 + 0] = (uint8_t)((b5 * 255) / 31);
            row[(size_t)x * 3 + 1] = (uint8_t)((g6 * 255) / 63);
            row[(size_t)x * 3 + 2] = (uint8_t)((r5 * 255) / 31);
        }
        fwrite(row.data(), 1, (size_t)row_bytes, f);
    }
    fclose(f);
    return true;
}

// --- measurement -----------------------------------------------------------

// Measured with a real label parked off-screen, not via a text-width API. A
// label's width is exactly what the row layout sees, and it avoids depending on
// which name a given LVGL version exposes (lv_text_get_width is absent here).
static int32_t text_w(const char *s, const lv_font_t *f) {
    if (!g_measure) {
        g_measure = lv_label_create(lv_screen_active());
        lv_obj_set_pos(g_measure, -2000, -2000);
        lv_label_set_long_mode(g_measure, LV_LABEL_LONG_CLIP);
    }
    lv_obj_set_style_text_font(g_measure, f, 0);
    lv_label_set_text(g_measure, s);
    lv_obj_update_layout(g_measure);
    return lv_obj_get_width(g_measure);
}

static void print_width_table(void) {
    printf("panel: %d x %d px   (row padding %d px/side, gap %d px, bar %d px)\n",
           PANEL_W, PANEL_H, ROW_PAD, ROW_GAP, BAR_W);
    printf("fonts: Inter + JetBrains Mono (SIL OFL 1.1), generated with lv_font_conv\n\n");

    printf("=== text width by font size (px) ===\n");
    printf("%-30s", "string");
    for (int i = 0; i < FONT_COUNT; i++) printf("%6s", FONTS[i].name);
    printf("\n");
    for (int c = 0; c < CAPTION_COUNT; c++) {
        printf("%-30s", CAPTIONS[c]);
        for (int i = 0; i < FONT_COUNT; i++) printf("%6d", (int)text_w(CAPTIONS[c], FONTS[i].font));
        printf("\n");
    }

    const int inner = PANEL_W - 2 * ROW_PAD;
    printf("\n=== width the caption gets (inner %d px) ===\n", inner);
    printf("%-8s%7s%7s%7s%7s%11s\n", "font", "pill", "pct", "bar", "gaps", "caption");
    for (int i = 0; i < FONT_COUNT; i++) {
        const lv_font_t *f = FONTS[i].font;
        const int pill = (int)text_w(PILL_TEXT, f);
        const int pct = (int)text_w(PCT_TEXT, f);
        const int gaps = 3 * ROW_GAP;
        printf("%-8s%7d%7d%7d%7d%11d\n", FONTS[i].name, pill, pct, BAR_W, gaps,
               inner - pill - pct - BAR_W - gaps);
    }

    printf("\n=== FIT CHECK ===\n");
    for (int i = 0; i < FONT_COUNT; i++) {
        const lv_font_t *f = FONTS[i].font;
        const int avail = inner - (int)text_w(PILL_TEXT, f) - (int)text_w(PCT_TEXT, f) - BAR_W - 3 * ROW_GAP;
        printf("  font %-3s avail=%3d px\n", FONTS[i].name, avail);
        for (int c = 0; c < CAPTION_COUNT; c++) {
            const int w = (int)text_w(CAPTIONS[c], f);
            printf("      %-30s w=%3d  %s\n", CAPTIONS[c], w, w <= avail ? "fits" : "OVERFLOWS");
        }
    }
}

// --- rendering -------------------------------------------------------------

static void build_row(lv_obj_t *parent, const lv_font_t *f, const char *caption) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, PANEL_W, 44);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x101010), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_left(row, ROW_PAD, 0);
    lv_obj_set_style_pad_right(row, ROW_PAD, 0);
    lv_obj_set_style_pad_column(row, ROW_GAP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *pill = lv_label_create(row);
    lv_label_set_text(pill, PILL_TEXT);
    lv_obj_set_style_text_font(pill, f, 0);
    lv_obj_set_style_text_color(pill, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *pct = lv_label_create(row);
    lv_label_set_text(pct, PCT_TEXT);
    lv_obj_set_style_text_font(pct, f, 0);
    lv_obj_set_style_text_color(pct, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *bar = lv_bar_create(row);
    lv_obj_set_size(bar, BAR_W, 8);
    lv_bar_set_value(bar, 88, LV_ANIM_OFF);

    lv_obj_t *cap = lv_label_create(row);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, f, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(0xFFFFFF), 0);
    // The device's old layout let the caption run past the panel edge. Ours must
    // not: clip it to whatever is left rather than overflowing the screen.
    lv_label_set_long_mode(cap, LV_LABEL_LONG_CLIP);

    lv_obj_update_layout(parent);
}

static void render_case(const lv_font_t *f, const char *caption, const char *path) {
    lv_obj_clean(g_root);  // safe: g_measure lives on the screen, not in g_root
    build_row(g_root, f, caption);
    lv_refr_now(NULL);
    write_bmp(path, g_fb, PANEL_W, PANEL_H);
}

int main(void) {
    mkdir(OUT_DIR, 0755);

    lv_init();
    g_fb.assign((size_t)PANEL_W * PANEL_H, 0);

    lv_display_t *disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, g_fb.data(), nullptr,
                           (uint32_t)(PANEL_W * PANEL_H * 2),
                           LV_DISPLAY_RENDER_MODE_FULL);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    // Persistent container for the row under test, kept separate from the screen
    // so the off-screen measurement label survives the per-case cleanup.
    g_root = lv_obj_create(scr);
    lv_obj_set_size(g_root, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_root, 0, 0);
    lv_obj_set_style_radius(g_root, 0, 0);
    lv_obj_set_style_pad_all(g_root, 0, 0);

    print_width_table();

    printf("\n=== rendered rows ===\n");
    for (int i = 0; i < FONT_COUNT; i++) {
        for (int c = 0; c < CAPTION_COUNT; c++) {
            char path[256];
            snprintf(path, sizeof(path), "%s/row-f%s-c%d.bmp", OUT_DIR, FONTS[i].name, c);
            render_case(FONTS[i].font, CAPTIONS[c], path);
        }
        printf("  font %-3s -> %d BMPs\n", FONTS[i].name, CAPTION_COUNT);
    }

    printf("\nwrote %d BMPs to ./%s/\n", FONT_COUNT * CAPTION_COUNT, OUT_DIR);
    return 0;
}
