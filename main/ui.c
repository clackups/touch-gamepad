/*
 * LVGL user interface implementation.
 *
 * The screen mirrors the gesture model: the upper half shows four tap zones in
 * a 2x2 grid, the lower half is a slide surface with a live joystick marker,
 * and a status line reports the transport and connection state. A modal overlay
 * renders the configuration menu.
 *
 * Every public function locks the LVGL port before touching widgets so it is
 * safe to call from the application task.
 *
 * ASCII only. See AGENTS.md.
 */
#include "ui.h"

#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

#include "display.h"

static const char *TAG = "ui";

#define UI_TAP_ZONE_COUNT 4
#define UI_AXIS_INDEX_MASK 0x3

typedef struct {
    lv_color_t background;
    lv_color_t foreground;
    lv_color_t accent;
} ui_theme_colors_t;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_zones[UI_TAP_ZONE_COUNT] = { NULL };
static lv_obj_t *s_zone_labels[UI_TAP_ZONE_COUNT] = { NULL };
static lv_obj_t *s_slide_area = NULL;
static lv_obj_t *s_slide_marker = NULL;
static lv_obj_t *s_menu_overlay = NULL;
static lv_obj_t *s_menu_list = NULL;

static ui_theme_colors_t s_colors;

static ui_theme_colors_t ui_theme_lookup(touch_gamepad_theme_t theme)
{
    ui_theme_colors_t colors;
    colors.background = lv_color_black();
    if (theme == TOUCH_GAMEPAD_THEME_GREEN_ON_BLACK) {
        colors.foreground = lv_color_hex(0x33FF33);
        colors.accent = lv_color_hex(0x116611);
    } else {
        colors.foreground = lv_color_hex(0x3399FF);
        colors.accent = lv_color_hex(0x113366);
    }
    return colors;
}

static void ui_apply_theme_locked(void)
{
    /*
     * lv_obj_remove_style_all() clears the default opaque background, and the
     * LVGL default for LV_STYLE_BG_OPA is transparent (unlike border/text opa).
     * A transparent screen background is never painted, so on the RGB panel's
     * alternating frame buffers old content is never erased and new output is
     * drawn on top of it. Force every filled surface opaque so each refresh
     * clears the frame buffer.
     */
    lv_obj_set_style_bg_color(s_screen, s_colors.background, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    for (int i = 0; i < UI_TAP_ZONE_COUNT; ++i) {
        lv_obj_set_style_bg_color(s_zones[i], s_colors.background, 0);
        lv_obj_set_style_bg_opa(s_zones[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_zones[i], s_colors.foreground, 0);
        lv_obj_set_style_text_color(s_zone_labels[i], s_colors.foreground, 0);
    }

    lv_obj_set_style_bg_color(s_slide_area, s_colors.background, 0);
    lv_obj_set_style_bg_opa(s_slide_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_slide_area, s_colors.foreground, 0);
    lv_obj_set_style_bg_color(s_slide_marker, s_colors.accent, 0);
    lv_obj_set_style_bg_opa(s_slide_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_slide_marker, s_colors.foreground, 0);
    lv_obj_set_style_text_color(s_status_label, s_colors.foreground, 0);

    if (s_menu_overlay != NULL) {
        lv_obj_set_style_bg_color(s_menu_overlay, s_colors.background, 0);
        lv_obj_set_style_bg_opa(s_menu_overlay, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_menu_overlay, s_colors.foreground, 0);
    }
}

esp_err_t ui_init(const touch_gamepad_config_t *config,
                  const touch_gamepad_board_preset_t *preset)
{
    if (!display_lock(-1)) {
        return ESP_ERR_TIMEOUT;
    }

    s_colors = ui_theme_lookup(config->theme);

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, preset->screen_width, preset->screen_height);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t width = preset->screen_width;
    const int32_t half = preset->screen_height / 2;
    const int32_t cell_w = width / 2;
    const int32_t cell_h = half / 2;

    for (int i = 0; i < UI_TAP_ZONE_COUNT; ++i) {
        const int32_t row = i / 2;
        const int32_t col = i % 2;
        s_zones[i] = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_zones[i]);
        lv_obj_set_size(s_zones[i], cell_w - 4, cell_h - 4);
        lv_obj_set_pos(s_zones[i], col * cell_w + 2, row * cell_h + 2);
        lv_obj_set_style_border_width(s_zones[i], 2, 0);
        lv_obj_set_style_radius(s_zones[i], 8, 0);
        lv_obj_clear_flag(s_zones[i], LV_OBJ_FLAG_SCROLLABLE);

        s_zone_labels[i] = lv_label_create(s_zones[i]);
        lv_label_set_text_fmt(s_zone_labels[i], "Zone %d", i + 1);
        lv_obj_center(s_zone_labels[i]);
    }

    s_slide_area = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_slide_area);
    lv_obj_set_size(s_slide_area, width - 4, half - 4);
    lv_obj_set_pos(s_slide_area, 2, half + 2);
    lv_obj_set_style_border_width(s_slide_area, 2, 0);
    lv_obj_set_style_radius(s_slide_area, 8, 0);
    lv_obj_clear_flag(s_slide_area, LV_OBJ_FLAG_SCROLLABLE);

    s_slide_marker = lv_obj_create(s_slide_area);
    lv_obj_remove_style_all(s_slide_marker);
    lv_obj_set_size(s_slide_marker, 40, 40);
    lv_obj_set_style_radius(s_slide_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_slide_marker, 2, 0);
    lv_obj_center(s_slide_marker);

    s_status_label = lv_label_create(s_slide_area);
    lv_label_set_text(s_status_label, "Slide area");
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    ui_apply_theme_locked();
    lv_screen_load(s_screen);

    display_unlock();
    ESP_LOGI(TAG, "UI ready");
    return ESP_OK;
}

void ui_set_theme(touch_gamepad_theme_t theme)
{
    if (!display_lock(-1)) {
        return;
    }
    s_colors = ui_theme_lookup(theme);
    ui_apply_theme_locked();
    display_unlock();
}

void ui_set_status(touch_gamepad_transport_t transport, bool connected)
{
    if (!display_lock(-1)) {
        return;
    }
    lv_label_set_text_fmt(s_status_label, "%s %s",
                          transport == TOUCH_GAMEPAD_TRANSPORT_USB ? "USB" : "BLE",
                          connected ? "connected" : "waiting");
    display_unlock();
}

void ui_flash_tap(uint8_t zone, uint8_t finger_count)
{
    if (zone >= UI_TAP_ZONE_COUNT) {
        return;
    }
    if (!display_lock(-1)) {
        return;
    }
    lv_obj_set_style_bg_color(s_zones[zone], s_colors.accent, 0);
    lv_label_set_text_fmt(s_zone_labels[zone], "Zone %d\n%d-finger", zone + 1, finger_count);
    display_unlock();
}

void ui_show_slide(uint8_t finger_count, int16_t delta_x, int16_t delta_y)
{
    if (!display_lock(-1)) {
        return;
    }
    int32_t offset_x = delta_x / 2;
    int32_t offset_y = delta_y / 2;
    if (offset_x > 100) {
        offset_x = 100;
    } else if (offset_x < -100) {
        offset_x = -100;
    }
    if (offset_y > 60) {
        offset_y = 60;
    } else if (offset_y < -60) {
        offset_y = -60;
    }
    lv_obj_align(s_slide_marker, LV_ALIGN_CENTER, offset_x, offset_y);
    lv_label_set_text_fmt(s_status_label, "%d-finger slide", finger_count);
    display_unlock();
}

static void ui_menu_value(const touch_gamepad_menu_item_t item,
                          const touch_gamepad_config_t *config,
                          const touch_gamepad_board_preset_t *preset,
                          char *out,
                          size_t out_len)
{
    switch (item) {
    case TOUCH_GAMEPAD_MENU_ITEM_TRANSPORT:
        snprintf(out, out_len, "%s%s",
                 config->transport_mode == TOUCH_GAMEPAD_TRANSPORT_USB ? "USB" : "BLE",
                 preset->supports_usb ? "" : " (fixed)");
        break;
    case TOUCH_GAMEPAD_MENU_ITEM_REPAIR:
        snprintf(out, out_len, "%s",
                 config->transport_mode == TOUCH_GAMEPAD_TRANSPORT_BLE ? "tap to re-pair" : "BLE only");
        break;
    case TOUCH_GAMEPAD_MENU_ITEM_MAPPING:
        snprintf(out, out_len, "8 taps / 2 slides");
        break;
    case TOUCH_GAMEPAD_MENU_ITEM_THEME:
        snprintf(out, out_len, "%s",
                 config->theme == TOUCH_GAMEPAD_THEME_GREEN_ON_BLACK ? "green" : "blue");
        break;
    default:
        out[0] = '\0';
        break;
    }
}

void ui_show_menu(const touch_gamepad_menu_state_t *menu,
                  const touch_gamepad_config_t *config,
                  const touch_gamepad_board_preset_t *preset)
{
    if (!display_lock(-1)) {
        return;
    }

    if (s_menu_overlay == NULL) {
        s_menu_overlay = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_menu_overlay);
        lv_obj_set_size(s_menu_overlay, preset->screen_width - 40, preset->screen_height - 40);
        lv_obj_center(s_menu_overlay);
        lv_obj_set_style_border_width(s_menu_overlay, 3, 0);
        lv_obj_set_style_radius(s_menu_overlay, 12, 0);
        lv_obj_set_style_pad_all(s_menu_overlay, 12, 0);
        lv_obj_set_flex_flow(s_menu_overlay, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *title = lv_label_create(s_menu_overlay);
        lv_label_set_text(title, "Configuration");
        lv_obj_set_style_text_color(title, s_colors.foreground, 0);

        s_menu_list = lv_obj_create(s_menu_overlay);
        lv_obj_remove_style_all(s_menu_list);
        lv_obj_set_width(s_menu_list, LV_PCT(100));
        lv_obj_set_flex_grow(s_menu_list, 1);
        lv_obj_set_flex_flow(s_menu_list, LV_FLEX_FLOW_COLUMN);
    }

    lv_obj_set_style_bg_color(s_menu_overlay, s_colors.background, 0);
    lv_obj_set_style_bg_opa(s_menu_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_menu_overlay, s_colors.foreground, 0);
    lv_obj_clean(s_menu_list);

    for (int i = 0; i < TOUCH_GAMEPAD_MENU_ITEM_COUNT; ++i) {
        char value[32];
        ui_menu_value((touch_gamepad_menu_item_t)i, config, preset, value, sizeof(value));

        lv_obj_t *row = lv_label_create(s_menu_list);
        const bool selected = (i == (int)menu->current_item);
        lv_label_set_text_fmt(row, "%s %s: %s",
                              selected ? ">" : " ",
                              touch_gamepad_menu_item_name((touch_gamepad_menu_item_t)i),
                              value);
        lv_obj_set_style_text_color(row, selected ? s_colors.accent : s_colors.foreground, 0);
    }

    lv_obj_clear_flag(s_menu_overlay, LV_OBJ_FLAG_HIDDEN);
    display_unlock();
}

void ui_show_mapping(const touch_gamepad_config_t *config,
                     const touch_gamepad_board_preset_t *preset)
{
    static const char *const axis_names[] = { "X", "Y", "Z", "Rz" };

    if (!display_lock(-1)) {
        return;
    }

    if (s_menu_overlay == NULL || s_menu_list == NULL) {
        display_unlock();
        return;
    }

    lv_obj_clean(s_menu_list);

    lv_obj_t *hint = lv_label_create(s_menu_list);
    lv_label_set_text(hint,
                      "Tap a zone to cycle its button.\n"
                      "Slide to cycle joystick axes.\n"
                      "Unlock sequence exits.");
    lv_obj_set_style_text_color(hint, s_colors.foreground, 0);

    for (int i = 0; i < TOUCH_GAMEPAD_TAP_BINDING_COUNT; ++i) {
        const int zone = i / 2;
        const int fingers = (i % 2) + 1;
        lv_obj_t *row = lv_label_create(s_menu_list);
        lv_label_set_text_fmt(row, "Zone %d %d-finger: %s",
                              zone + 1, fingers,
                              touch_gamepad_button_label(config->tap_buttons[i]));
        lv_obj_set_style_text_color(row, s_colors.foreground, 0);
    }

    lv_obj_t *slide1 = lv_label_create(s_menu_list);
    lv_label_set_text_fmt(slide1, "1-finger slide: %s/%s",
                          axis_names[config->one_finger_slide.axis_x & UI_AXIS_INDEX_MASK],
                          axis_names[config->one_finger_slide.axis_y & UI_AXIS_INDEX_MASK]);
    lv_obj_set_style_text_color(slide1, s_colors.foreground, 0);

    lv_obj_t *slide2 = lv_label_create(s_menu_list);
    lv_label_set_text_fmt(slide2, "2-finger slide: %s/%s",
                          axis_names[config->two_finger_slide.axis_x & UI_AXIS_INDEX_MASK],
                          axis_names[config->two_finger_slide.axis_y & UI_AXIS_INDEX_MASK]);
    lv_obj_set_style_text_color(slide2, s_colors.foreground, 0);

    (void)preset;
    lv_obj_clear_flag(s_menu_overlay, LV_OBJ_FLAG_HIDDEN);
    display_unlock();
}

void ui_hide_menu(void)
{
    if (s_menu_overlay == NULL) {
        return;
    }
    if (!display_lock(-1)) {
        return;
    }
    lv_obj_add_flag(s_menu_overlay, LV_OBJ_FLAG_HIDDEN);
    display_unlock();
}
