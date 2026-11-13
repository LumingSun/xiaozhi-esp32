#ifndef HOME_DISPLAY_H
#define HOME_DISPLAY_H

#include "display/lcd_display.h"

#include <atomic>
#include <esp_timer.h>
#include <lvgl.h>
#include <string>
#include <vector>

class HomeLcdDisplay : public SpiLcdDisplay {
public:
    HomeLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy)
        : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                        swap_xy) {}

    ~HomeLcdDisplay() override;

    void SetupUI() override;

    // 重写以接入唤醒逻辑：先跑父类同名，再按需亮屏（带防递归守护）。
    void SetStatus(const char* status) override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;

    // 息屏/亮屏的统一入口。基类 SetPowerSaveMode 只改表情/字幕，
    // 这里额外驱动背光，并抑制其内部 SetEmotion("sleepy")/SetChatMessage
    // 反悔亮屏（防递归）。
    void SetPowerSaveMode(bool on) override;

    // 主题换字体时同步信息屏的局部样式。
    // 信息屏标签用 lv_obj_set_style_text_font 设的是局部样式，不在 SetTheme 的重绑范围内；
    // 不重设就会一直用建屏时（资产应用前）的 basic 字体 -> 缺"雨/湿/北京"等字形。
    bool SetTextFont(std::shared_ptr<LvglFont> text_font) override;

    // 供屏幕按钮调用（触摸入口）
    void EnterInfo();       // Active -> Info
    void EnterScreenOff();  // Info -> ScreenOff

private:
    // 空闲三态：Active(表情+字幕) -> Info(信息屏) -> ScreenOff(息屏)。
    enum class ScreenState { Active, Info, ScreenOff };

    // 事件/计时器回调。LVGL 事件与 lv_timer 均运行在 LVGL 任务内（递归锁已持有）；
    // esp_timer 回调运行在 esp_timer 任务（需自取 DisplayLockGuard）。
    static void screen_event_cb(lv_event_t* e);       // 全局按压（唤醒/交互）
    static void step_button_event_cb(lv_event_t* e);  // 音量/亮度 ± 按钮
    static void state_timer_cb(lv_timer_t* timer);    // 空闲状态机 tick
    static void fade_timer_cb(void* arg);             // 数值标签 1s 后淡出
    static void weather_task_entry(void* arg);        // 天气获取任务

    // 状态机动作（LVGL 任务内）
    void OnUserActivity(const char* why = "?");   // 任意交互：重置空闲计时；ScreenOff 则唤醒回 Active
    void WakeAndActivate(const char* why = "?");  // 亮屏 + 回 Active + 隐藏信息屏
    void OnStateTick();      // 每秒检查空闲超时

    // 息屏/亮屏（背光，跨任务安全）
    void WakeScreen();
    void SleepScreen();

    // 信息屏
    void BuildInfoScreen();  // 创建 info_container_ 及各标签/按钮（SetupUI 内一次）
    void UpdateInfoClock();
    void UpdateInfoConnection();
    void UpdateInfoWeather();
    void ShowValueLabel(lv_obj_t* label, const char* text);  // 显示数值并排 1s 淡出

    // 按钮动作（经 Application::Schedule 回主任务执行，避免在 LVGL 任务碰编解码器）
    void ChangeVolume(int delta);
    void ChangeBrightness(int delta);

    // 天气（独立低优先级任务；阻塞仅限本任务，不碰主循环/音频）
    bool FetchWeather();
    void ApplyWeather(const std::string& temp, const std::string& desc, const std::string& humid,
                      const std::string& region);
    static const char* WeatherCodeToZh(const char* code);
    static std::string CityDisplayName(const std::string& raw);  // Beijing -> 北京

    // 圆屏安全区：把超宽聊天气泡收进圆内（基类按整屏 85% 算宽，会溢出圆底）
    void ClampChatBubbles();
    void ApplyInfoFontStylesLocked();  // 已持 DisplayLockGuard 时调用
    void OnInfoButtonClicked();        // 对话屏「信息屏」按钮
    void OnSleepButtonClicked();       // 信息屏「息屏」按钮

    static void info_button_cb(lv_event_t* e);
    static void sleep_button_cb(lv_event_t* e);

    static int64_t NowMs();

    // 唤醒防递归：仅抑制“自身电源切换”引发的亮屏。
    bool suppress_wake_ = false;

    // 空闲状态机（state_/last_activity_ms_ 跨 LVGL 任务与主任务访问，须原子；
    // int64_t 在 32 位 Xtensa 上非原子读写，故用 std::atomic）
    std::atomic<ScreenState> state_{ScreenState::Active};
    std::atomic<int64_t> last_activity_ms_{0};
    lv_timer_t* state_timer_ = nullptr;
    esp_timer_handle_t value_fade_timer_ = nullptr;

    // 字体对象的所有权持有者。
    // LVGL 样式只存裸 lv_font_t*，而 Assets::Apply() 会调用 SetTextFont() 换主题字体，
    // 期间 LvglDisplay::SetTextFont 在重绑完主题样式后会释放旧字体对象。信息屏标签用的是
    // 局部样式（不在重绑范围），旧对象一释放指针就悬空，布局刷新时
    // lv_font_get_glyph_width 调到空函数指针 → 整机重启。
    // 这两个 shared_ptr 让旧字体活到本对象析构，彻底消除悬空。
    std::shared_ptr<LvglFont> info_text_font_;
    std::shared_ptr<LvglFont> info_icon_font_;
    // 换字体时的安全网：保留最近几个旧字体对象，避免"漏刷某个局部样式"立刻悬空崩溃。
    // 真正正确的做法仍是 ApplyInfoFontStylesLocked() 把每个局部样式都刷一遍。
    std::vector<std::shared_ptr<LvglFont>> held_fonts_;

    // 信息屏 UI（info_container_ 覆盖在基础 UI 之上，Active/ScreenOff 时隐藏）
    lv_obj_t* info_container_ = nullptr;
    lv_obj_t* main_info_box_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* weather_label_ = nullptr;
    lv_obj_t* humid_label_ = nullptr;
    lv_obj_t* conn_label_ = nullptr;
    lv_obj_t* controls_bar_ = nullptr;
    lv_obj_t* vol_minus_btn_ = nullptr;
    lv_obj_t* vol_plus_btn_ = nullptr;
    lv_obj_t* bright_minus_btn_ = nullptr;
    lv_obj_t* bright_plus_btn_ = nullptr;
    lv_obj_t* vol_value_label_ = nullptr;
    lv_obj_t* bright_value_label_ = nullptr;
    lv_obj_t* sleep_btn_ = nullptr;
    lv_obj_t* sleep_btn_label_ = nullptr;

    // 对话屏右上角「信息屏」按钮（overlay 在基础 UI 之上）
    lv_obj_t* info_btn_ = nullptr;
    lv_obj_t* info_btn_label_ = nullptr;

    // 天气（成员仅在主任务访问：ApplyWeather 写、UpdateInfoWeather 读）
    void* weather_task_ = nullptr;
    std::string weather_temp_ = "--";
    std::string weather_desc_ = "--";
    std::string weather_humid_ = "--";
    std::string weather_region_;
};

#endif  // HOME_DISPLAY_H
