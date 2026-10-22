#include "home_display.h"

#include "board.h"

#include <esp_log.h>
#include <cstring>

#define TAG "HomeLcdDisplay"

void HomeLcdDisplay::SetupUI() {
    // 先跑父类 SetupUI()，完整创建主题/表情/状态栏/字幕等既有对象
    SpiLcdDisplay::SetupUI();

    // 全局按压捕获：息屏时点击屏幕立即唤醒。挂在 lv_screen_active() 上，
    // LVGL 输入事件（PRESSED/RELEASED…）默认沿父链冒泡，故子对象（Task 9
    // 的音量/亮度按钮）上的点击也会冒泡到这里，一并视为唤醒/交互。
    lv_obj_add_event_cb(lv_screen_active(), screen_event_cb, LV_EVENT_PRESSED, this);

    ESP_LOGI(TAG, "home variant UI initialized (touch wake armed)");
}

void HomeLcdDisplay::screen_event_cb(lv_event_t* e) {
    auto* self = static_cast<HomeLcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        // 事件回调运行在 LVGL 任务内（递归锁已持有）；RestoreBrightness 只操作
        // 背光 PWM，不触碰 LVGL 对象，故此处无需再取 DisplayLockGuard。
        self->WakeScreen();
    }
}

void HomeLcdDisplay::WakeScreen() {
    // RestoreBrightness()：从 NVS 取回记忆亮度，经 esp_timer 渐变过渡，
    // 跨任务安全；已在亮屏状态时重复调用亦无害（幂等）。
    Board::GetInstance().GetBacklight()->RestoreBrightness();
}

void HomeLcdDisplay::SleepScreen() {
    Board::GetInstance().GetBacklight()->SetBrightness(0);
}

void HomeLcdDisplay::SetStatus(const char* status) {
    SpiLcdDisplay::SetStatus(status);
    if (!suppress_wake_ && status != nullptr && status[0] != '\0') {
        WakeScreen();
    }
}

bool HomeLcdDisplay::SetEmotion(const char* emotion) {
    bool ret = SpiLcdDisplay::SetEmotion(emotion);
    // "sleepy" 表示正在入睡（SetPowerSaveMode(true) 内部会设置），不亮屏。
    if (!suppress_wake_ && emotion != nullptr && strcmp(emotion, "sleepy") != 0) {
        WakeScreen();
    }
    return ret;
}

void HomeLcdDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);
    // 空 content（如 SetPowerSaveMode 的 SetChatMessage("system","")）不亮屏。
    if (!suppress_wake_ && content != nullptr && content[0] != '\0') {
        WakeScreen();
    }
}

void HomeLcdDisplay::SetPowerSaveMode(bool on) {
    // 基类内部会调 SetEmotion("sleepy")/SetChatMessage("system","")，这些虚调用
    // 会回到本类的重写；suppress_wake_ 阻止它们把屏幕又点亮（防递归）。
    suppress_wake_ = true;
    SpiLcdDisplay::SetPowerSaveMode(on);
    suppress_wake_ = false;

    if (on) {
        SleepScreen();
    } else {
        WakeScreen();
    }
}
