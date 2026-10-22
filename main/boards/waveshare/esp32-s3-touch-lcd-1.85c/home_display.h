#ifndef HOME_DISPLAY_H
#define HOME_DISPLAY_H

#include "display/lcd_display.h"

// Home 变体的 LCD 显示实现（小智家庭助手）。
// 复用 SpiLcdDisplay 的全部能力（主题/表情/状态栏/字幕），
// 追加：触摸接入 + 息屏/亮屏与唤醒（Task 8）、
//       信息屏 + 音量/亮度按钮 + 空闲三态状态机（Task 9）。
class HomeLcdDisplay : public SpiLcdDisplay {
public:
    HomeLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    void SetupUI() override;

    // 重写以接入唤醒逻辑：先跑父类同名，再按需亮屏（带防递归守护）。
    void SetStatus(const char* status) override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;

    // 息屏/亮屏的统一入口。基类 SetPowerSaveMode 只改表情/字幕，
    // 这里额外驱动背光，并抑制其内部 SetEmotion("sleepy")/SetChatMessage
    // 反悔亮屏（防递归）。
    void SetPowerSaveMode(bool on) override;

private:
    // 全局按压捕获（息屏唤醒）。LVGL 事件在 LVGL 任务内派发，递归锁已持有。
    static void screen_event_cb(lv_event_t* e);

    void WakeScreen();  // 亮屏：RestoreBrightness()（NVS 记忆亮度 + esp_timer 渐变）
    void SleepScreen();  // 息屏：SetBrightness(0)

    // 仅抑制“自身电源切换”引发的亮屏，避免 SetPowerSaveMode(true) 内部
    // 的 SetEmotion("sleepy")/SetChatMessage("system","") 把屏幕又点亮。
    bool suppress_wake_ = false;
};

#endif  // HOME_DISPLAY_H
