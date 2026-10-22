#ifndef HOME_DISPLAY_H
#define HOME_DISPLAY_H

#include "display/lcd_display.h"

// Home 变体的 LCD 显示实现（Task 7 骨架）。
// 复用 SpiLcdDisplay 的全部能力（主题/表情/状态栏/字幕），仅重写 SetupUI()
// 追加家庭助手自定义 UI；真实内容由 Task 8/9 填充。
class HomeLcdDisplay : public SpiLcdDisplay {
public:
    HomeLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    void SetupUI() override;

private:
    lv_obj_t* placeholder_label_ = nullptr;  // Task 8/9 之前的隐藏占位，仅验证链路
};

#endif  // HOME_DISPLAY_H
