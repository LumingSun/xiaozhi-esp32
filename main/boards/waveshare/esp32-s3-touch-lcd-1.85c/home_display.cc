#include "home_display.h"

#include "lvgl_theme.h"

#include <esp_log.h>

#define TAG "HomeLcdDisplay"

void HomeLcdDisplay::SetupUI() {
    // 先跑父类 SetupUI()，完整创建主题/表情/状态栏等既有对象
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme == nullptr) {
        ESP_LOGE(TAG, "current_theme_ is null, skip home UI setup");
        return;
    }
    auto text_font = lvgl_theme->text_font();
    if (text_font == nullptr) {
        ESP_LOGE(TAG, "text_font is null, skip home UI setup");
        return;
    }

    // Task 7 占位：隐藏 label，仅验证 HomeLcdDisplay::SetupUI() 链路。
    // 自定义页面一律 lv_obj_create(lv_screen_active()) 独立建，
    // 不塞进 bottom_bar_/top_bar_（会被 SetHideSubtitle()/UpdateStatusBar() 干扰）。
    // 真实家庭助手 UI 由 Task 8/9 填充。
    placeholder_label_ = lv_label_create(lv_screen_active());
    lv_label_set_text(placeholder_label_, "");
    lv_obj_set_style_text_font(placeholder_label_, text_font->font(), 0);
    lv_obj_set_style_text_color(placeholder_label_, lvgl_theme->text_color(), 0);
    lv_obj_add_flag(placeholder_label_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "home variant placeholder UI initialized");
}
