/*
 * LVGL user interface implementation.
 *
 * The screen mirrors the gesture model: the upper half shows four tap zones in
 * a 2x2 grid that light up while pressed, the lower half is an analog joystick
 * that draws a central point and a vector to the current touch point, and a
 * status line reports the transport and connection state. A modal overlay
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
static lv_obj_t *s_slide_center = NULL;
static lv_obj_t *s_slide_vector = NULL;
static lv_point_precise_t s_vector_points[2];
static lv_obj_t *s_menu_overlay = NULL;
static lv_obj_t *s_menu_title = NULL;
static lv_obj_t *s_menu_hint = NULL;
static lv_obj_t *s_menu_list = NULL;
static lv_obj_t *s_menu_rows[TOUCH_GAMEPAD_MENU_MAX_ROWS] = { NULL };
static uint8_t s_menu_row_count = 0;

/* Height in pixels of a single menu row. Roughly twice the previous single-line
 * item height so each row is a comfortable one-finger touch target. */
#define UI_MENU_ROW_HEIGHT 48

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
    lv_obj_set_style_line_color(s_slide_vector, s_colors.foreground, 0);
    lv_obj_set_style_bg_color(s_slide_center, s_colors.foreground, 0);
    lv_obj_set_style_bg_opa(s_slide_center, LV_OPA_COVER, 0);
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

    /*
     * The lower half acts as an analog joystick. Draw the vector from the fixed
     * central point to the touch point first so the center dot and the moving
     * marker render on top of it. The vector points array is stored statically
     * because lv_line keeps a reference to it rather than copying.
     */
    s_slide_vector = lv_line_create(s_slide_area);
    lv_obj_set_style_line_width(s_slide_vector, 4, 0);
    lv_obj_set_style_line_rounded(s_slide_vector, true, 0);
    lv_obj_add_flag(s_slide_vector, LV_OBJ_FLAG_HIDDEN);

    s_slide_center = lv_obj_create(s_slide_area);
    lv_obj_remove_style_all(s_slide_center);
    lv_obj_set_size(s_slide_center, 14, 14);
    lv_obj_set_style_radius(s_slide_center, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(s_slide_center);

    s_slide_marker = lv_obj_create(s_slide_area);
    lv_obj_remove_style_all(s_slide_marker);
    lv_obj_set_size(s_slide_marker, 40, 40);
    lv_obj_set_style_radius(s_slide_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_slide_marker, 2, 0);
    lv_obj_center(s_slide_marker);

    s_status_label = lv_label_create(s_slide_area);
    lv_label_set_text(s_status_label, "Joystick");
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

void ui_set_zone_active(uint8_t zone, bool active)
{
    if (zone >= UI_TAP_ZONE_COUNT) {
        return;
    }
    if (!display_lock(-1)) {
        return;
    }
    lv_obj_set_style_bg_color(s_zones[zone], active ? s_colors.accent : s_colors.background, 0);
    lv_label_set_text_fmt(s_zone_labels[zone], "Zone %d%s", zone + 1, active ? "\nPRESS" : "");
    display_unlock();
}

void ui_show_joystick(int16_t delta_x, int16_t delta_y)
{
    if (!display_lock(-1)) {
        return;
    }

    const int32_t content_w = lv_obj_get_content_width(s_slide_area);
    const int32_t content_h = lv_obj_get_content_height(s_slide_area);
    const int32_t marker_half = 20; /* half the 40px marker */
    const int32_t max_x = (content_w / 2) - marker_half;
    const int32_t max_y = (content_h / 2) - marker_half;

    int32_t offset_x = delta_x;
    int32_t offset_y = delta_y;
    if (offset_x > max_x) {
        offset_x = max_x;
    } else if (offset_x < -max_x) {
        offset_x = -max_x;
    }
    if (offset_y > max_y) {
        offset_y = max_y;
    } else if (offset_y < -max_y) {
        offset_y = -max_y;
    }

    lv_obj_align(s_slide_marker, LV_ALIGN_CENTER, offset_x, offset_y);

    const int32_t center_x = content_w / 2;
    const int32_t center_y = content_h / 2;
    s_vector_points[0].x = center_x;
    s_vector_points[0].y = center_y;
    s_vector_points[1].x = center_x + offset_x;
    s_vector_points[1].y = center_y + offset_y;
    lv_line_set_points(s_slide_vector, s_vector_points, 2);
    lv_obj_clear_flag(s_slide_vector, LV_OBJ_FLAG_HIDDEN);

    display_unlock();
}

void ui_hide_joystick(void)
{
    if (!display_lock(-1)) {
        return;
    }
    lv_obj_align(s_slide_marker, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_slide_vector, LV_OBJ_FLAG_HIDDEN);
    display_unlock();
}

static void ui_menu_create_overlay_locked(const touch_gamepad_board_preset_t *preset)
{
    s_menu_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu_overlay);
    lv_obj_set_size(s_menu_overlay, preset->screen_width - 20, preset->screen_height - 20);
    lv_obj_center(s_menu_overlay);
    lv_obj_set_style_border_width(s_menu_overlay, 3, 0);
    lv_obj_set_style_radius(s_menu_overlay, 12, 0);
    lv_obj_set_style_pad_all(s_menu_overlay, 10, 0);
    lv_obj_set_flex_flow(s_menu_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_menu_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_menu_title = lv_label_create(s_menu_overlay);
    lv_label_set_text(s_menu_title, "Configuration");

    s_menu_hint = lv_label_create(s_menu_overlay);
    lv_label_set_text(s_menu_hint,
                      "Tap a row. Tap left/right to change a value.\n"
                      "Slide up/down to scroll.");

    s_menu_list = lv_obj_create(s_menu_overlay);
    lv_obj_remove_style_all(s_menu_list);
    lv_obj_set_width(s_menu_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_menu_list, 1);
    lv_obj_set_flex_flow(s_menu_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_menu_list, 4, 0);
    lv_obj_set_scroll_dir(s_menu_list, LV_DIR_VER);
}

void ui_show_menu(const touch_gamepad_menu_state_t *menu,
                  const touch_gamepad_config_t *config,
                  const touch_gamepad_board_preset_t *preset)
{
    touch_gamepad_menu_view_t view;

    if (!display_lock(-1)) {
        return;
    }

    touch_gamepad_menu_build_view(menu, config, preset, &view);

    if (s_menu_overlay == NULL) {
        ui_menu_create_overlay_locked(preset);
    }

    lv_obj_set_style_bg_color(s_menu_overlay, s_colors.background, 0);
    lv_obj_set_style_bg_opa(s_menu_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_menu_overlay, s_colors.foreground, 0);
    lv_label_set_text(s_menu_title, view.title);
    lv_obj_set_style_text_color(s_menu_title, s_colors.foreground, 0);
    lv_obj_set_style_text_color(s_menu_hint, s_colors.foreground, 0);

    lv_obj_clean(s_menu_list);
    s_menu_row_count = 0;

    for (uint8_t i = 0; i < view.count && i < TOUCH_GAMEPAD_MENU_MAX_ROWS; ++i) {
        const touch_gamepad_menu_row_t *row = &view.rows[i];
        const bool selected = (i == view.selected);
        char text[80];

        if (row->kind == TOUCH_GAMEPAD_MENU_ROW_CHOICE) {
            snprintf(text, sizeof(text), "%s:  < %s >", row->label, row->value);
        } else if (row->value[0] != '\0') {
            snprintf(text, sizeof(text), "%s:  %s", row->label, row->value);
        } else {
            snprintf(text, sizeof(text), "%s", row->label);
        }

        lv_obj_t *item = lv_obj_create(s_menu_list);
        lv_obj_remove_style_all(item);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, UI_MENU_ROW_HEIGHT);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_bg_color(item, selected ? s_colors.accent : s_colors.background, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, s_colors.foreground, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

        s_menu_rows[i] = item;
        s_menu_row_count = (uint8_t)(i + 1U);
    }

    if ((view.selected < s_menu_row_count) && (s_menu_rows[view.selected] != NULL)) {
        lv_obj_scroll_to_view(s_menu_rows[view.selected], LV_ANIM_OFF);
    }

    lv_obj_clear_flag(s_menu_overlay, LV_OBJ_FLAG_HIDDEN);
    display_unlock();
}

bool ui_menu_hit_test(int16_t x, int16_t y, uint8_t *out_row, int8_t *out_direction)
{
    bool hit = false;

    if ((s_menu_overlay == NULL) || (s_menu_row_count == 0U)) {
        return false;
    }
    if (!display_lock(-1)) {
        return false;
    }

    lv_area_t list_area;
    lv_obj_get_coords(s_menu_list, &list_area);

    for (uint8_t i = 0; i < s_menu_row_count; ++i) {
        lv_area_t area;

        if (s_menu_rows[i] == NULL) {
            continue;
        }
        lv_obj_get_coords(s_menu_rows[i], &area);

        /* Only accept taps that land inside the visible list viewport so rows
         * scrolled out of view are not selected. */
        if ((y < list_area.y1) || (y > list_area.y2)) {
            continue;
        }
        if ((x >= area.x1) && (x <= area.x2) && (y >= area.y1) && (y <= area.y2)) {
            const int32_t mid = (area.x1 + area.x2) / 2;
            *out_row = i;
            *out_direction = (x < mid) ? (int8_t)-1 : (int8_t)1;
            hit = true;
            break;
        }
    }

    display_unlock();
    return hit;
}

void ui_menu_scroll(int16_t delta_y)
{
    if (s_menu_list == NULL) {
        return;
    }
    if (!display_lock(-1)) {
        return;
    }
    lv_obj_scroll_by(s_menu_list, 0, delta_y, LV_ANIM_OFF);
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
