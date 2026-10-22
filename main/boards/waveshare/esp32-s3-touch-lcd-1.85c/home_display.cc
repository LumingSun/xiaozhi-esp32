#include "home_display.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "device_state.h"
#include "lvgl_theme.h"

#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <material_symbols.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "HomeLcdDisplay"

// 城市配置 depends on home 开关；本文件在两种变体都会编译（GLOB），
// 原版变体未开 HOME 时该宏不存在，给一个默认值保证可编译（原版运行期不会用到）。
#ifndef CONFIG_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C_HOME_CITY
#define CONFIG_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C_HOME_CITY "Shanghai"
#endif

namespace {
// ---- 时长/步进常量（集中在文件头部，便于调整）----
constexpr int64_t kActiveToInfoMs = 60 * 1000;        // Active 无对话事件 60s -> Info
constexpr int64_t kInfoToScreenOffMs = 300 * 1000;    // Info 无交互 300s -> ScreenOff
constexpr int kStateTickMs = 1000;                    // 状态机 tick 周期
constexpr int kWeatherStartDelayMs = 15 * 1000;       // 启动后首次抓取前等待（等连网）
constexpr int kWeatherRefreshMs = 30 * 60 * 1000;     // 天气刷新周期 30min
constexpr int kWeatherRetryMs = 60 * 1000;            // 天气失败重试间隔
constexpr int kWeatherTimeoutMs = 10000;              // HTTP 超时
constexpr int kWeatherBufferSize = 2048;              // esp_http_client 读缓冲
constexpr size_t kWeatherMaxBody = 64 * 1024;         // body 累计上限（实测约 40KB）
constexpr int kVolumeStep = 10;                       // 音量步进
constexpr int kBrightnessStep = 10;                   // 亮度步进
constexpr int kValueFadeDelayMs = 1000;               // 数值显示 1s 后淡出
constexpr int kValueFadeDurMs = 300;                  // 淡出时长
constexpr int kWeatherTaskStack = 8192;               // 天气任务栈（字节）
constexpr int kWeatherTaskPrio = 3;                   // 天气任务优先级（低于 LVGL 的 4）

// weatherCode -> 中文（wttr.in 的 ?lang=zh 不本地化 weatherDesc，需自建映射）。
struct WeatherCodeEntry {
    const char* code;
    const char* zh;
};
const WeatherCodeEntry kWeatherCodeMap[] = {
    {"113", "晴"},   {"116", "多云"}, {"119", "阴"},   {"122", "阴"},   {"143", "雾"},
    {"176", "小雨"}, {"179", "小雪"}, {"182", "雨夹雪"},{"185", "小雨"}, {"200", "雷雨"},
    {"227", "吹雪"}, {"230", "大雪"}, {"248", "雾"},   {"260", "雾"},   {"263", "毛毛雨"},
    {"281", "小雨"}, {"284", "小雨"}, {"293", "小雨"}, {"296", "小雨"}, {"299", "中雨"},
    {"302", "大雨"}, {"305", "中雨"}, {"308", "大雨"}, {"311", "小雨"}, {"314", "雨夹雪"},
    {"317", "雨夹雪"},{"320", "冻雨"},{"323", "小雪"}, {"326", "中雪"}, {"329", "中雪"},
    {"332", "大雪"}, {"335", "大雪"}, {"338", "大雪"}, {"350", "冰珠"}, {"353", "小雨"},
    {"356", "中雨"}, {"359", "大雨"}, {"362", "雨夹雪"},{"365", "雨夹雪"},{"368", "小雪"},
    {"371", "大雪"}, {"374", "冰珠"}, {"377", "冰珠"}, {"386", "雷雨"}, {"389", "雷雨"},
    {"392", "雷雪"}, {"395", "大雪"},
};

const char* kWeekdaysCn[] = {"日", "一", "二", "三", "四", "五", "六"};

// 数值标签淡入/淡出过渡。文件作用域（须比 label 存活更久）；用样式过渡而非一次性
// lv_obj_fade_out 动画，连续点按可平滑改目标、不冲突。
lv_style_transition_dsc_t g_value_fade_trans;
const lv_style_prop_t kValueFadeProps[] = {LV_STYLE_OPA, LV_STYLE_PROP_INV};

// 创建一个 ± 步进按钮（34x34，居中的 +/- 文本）。不需要 this，故为自由函数。
lv_obj_t* MakeStepButton(lv_obj_t* parent, const char* symbol, const lv_font_t* font) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 34, 34);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_center(lbl);
    return btn;
}
}  // namespace

HomeLcdDisplay::~HomeLcdDisplay() {
    if (weather_task_ != nullptr) {
        vTaskDelete(static_cast<TaskHandle_t>(weather_task_));
        weather_task_ = nullptr;
    }
    if (value_fade_timer_ != nullptr) {
        esp_timer_stop(value_fade_timer_);
        esp_timer_delete(value_fade_timer_);
        value_fade_timer_ = nullptr;
    }
    if (state_timer_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_timer_del(state_timer_);
        state_timer_ = nullptr;
    }
}

void HomeLcdDisplay::SetupUI() {
    // 先跑父类 SetupUI()，完整创建主题/表情/状态栏/字幕等既有对象
    SpiLcdDisplay::SetupUI();

    // 全局按压捕获：息屏/信息屏下的任意点击都会沿父链冒泡到这里。
    lv_obj_add_event_cb(lv_screen_active(), screen_event_cb, LV_EVENT_PRESSED, this);

    BuildInfoScreen();

    // 空闲状态机 tick
    last_activity_ms_.store(NowMs());
    state_timer_ = lv_timer_create(state_timer_cb, kStateTickMs, this);

    // 数值标签 1s 后淡出计时器（esp_timer，ESP_TIMER_TASK 派发）
    esp_timer_create_args_t fade_args = {
        .callback = fade_timer_cb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "home_val_fade",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&fade_args, &value_fade_timer_));

    // 天气获取任务（独立低优先级任务，阻塞仅限本任务）
    if (weather_task_ == nullptr) {
        TaskHandle_t handle = nullptr;
        BaseType_t ok =
            xTaskCreate(weather_task_entry, "home_weather", kWeatherTaskStack, this,
                        kWeatherTaskPrio, &handle);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create weather task");
        } else {
            weather_task_ = static_cast<void*>(handle);
        }
    }

    ESP_LOGI(TAG, "home variant UI initialized (state machine + info screen + weather armed)");
}

// ---- 静态回调 ----

void HomeLcdDisplay::screen_event_cb(lv_event_t* e) {
    auto* self = static_cast<HomeLcdDisplay*>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->OnUserActivity();
    }
}

void HomeLcdDisplay::step_button_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto* self = static_cast<HomeLcdDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    lv_obj_t* target = lv_event_get_target_obj(e);
    int vol_delta = 0;
    int bright_delta = 0;
    if (target == self->vol_minus_btn_) {
        vol_delta = -kVolumeStep;
    } else if (target == self->vol_plus_btn_) {
        vol_delta = kVolumeStep;
    } else if (target == self->bright_minus_btn_) {
        bright_delta = -kBrightnessStep;
    } else if (target == self->bright_plus_btn_) {
        bright_delta = kBrightnessStep;
    } else {
        return;
    }
    // 按钮回调在 LVGL 任务内；把编解码器/背光/UI 工作排到主任务，保持单线程访问。
    Application::GetInstance().Schedule([self, vol_delta, bright_delta]() {
        if (vol_delta != 0) {
            self->ChangeVolume(vol_delta);
        }
        if (bright_delta != 0) {
            self->ChangeBrightness(bright_delta);
        }
    });
}

void HomeLcdDisplay::state_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<HomeLcdDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnStateTick();
    }
}

void HomeLcdDisplay::fade_timer_cb(void* arg) {
    auto* self = static_cast<HomeLcdDisplay*>(arg);
    if (self == nullptr) {
        return;
    }
    DisplayLockGuard lock(self);
    // 置 OPA 全透明，由样式过渡完成淡出。
    if (self->vol_value_label_ != nullptr) {
        lv_obj_set_style_opa(self->vol_value_label_, LV_OPA_TRANSP, 0);
    }
    if (self->bright_value_label_ != nullptr) {
        lv_obj_set_style_opa(self->bright_value_label_, LV_OPA_TRANSP, 0);
    }
}

void HomeLcdDisplay::weather_task_entry(void* arg) {
    auto* self = static_cast<HomeLcdDisplay*>(arg);
    vTaskDelay(pdMS_TO_TICKS(kWeatherStartDelayMs));
    while (true) {
        bool ok = self->FetchWeather();
        vTaskDelay(pdMS_TO_TICKS(ok ? kWeatherRefreshMs : kWeatherRetryMs));
    }
}

// ---- 状态机 ----

void HomeLcdDisplay::OnUserActivity() {
    last_activity_ms_.store(NowMs());
    ScreenState s = state_.load();
    if (s == ScreenState::ScreenOff) {
        WakeAndActivate();
    } else if (s == ScreenState::Info) {
        UpdateInfoClock();  // 用户正在看，顺手刷新时间
    }
    // Active：仅重置计时，保持在 Active。
}

void HomeLcdDisplay::WakeAndActivate() {
    bool was_off = state_.load() == ScreenState::ScreenOff;
    state_.store(ScreenState::Active);
    last_activity_ms_.store(NowMs());
    {
        DisplayLockGuard lock(this);
        if (info_container_ != nullptr) {
            lv_obj_add_flag(info_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (was_off) {
        WakeScreen();  // 仅息屏态才恢复背光，避免每次表情变化都重放渐变
    }
}

void HomeLcdDisplay::EnterInfo() {
    state_.store(ScreenState::Info);
    {
        DisplayLockGuard lock(this);
        if (info_container_ != nullptr) {
            lv_obj_remove_flag(info_container_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(info_container_);
        }
    }
    UpdateInfoClock();       // LVGL 任务：取自 time()，无共享状态
    UpdateInfoConnection();  // LVGL 任务：读 GetDeviceState()（32 位原子读，良性）
    // weather_* 只在主任务访问，排到主任务刷新，避免与 ApplyWeather 数据竞争。
    Application::GetInstance().Schedule([this]() {
        if (state_.load() == ScreenState::Info) {
            UpdateInfoWeather();
        }
    });
    ESP_LOGI(TAG, "state: Active -> Info");
}

void HomeLcdDisplay::EnterScreenOff() {
    state_.store(ScreenState::ScreenOff);
    {
        DisplayLockGuard lock(this);
        if (info_container_ != nullptr) {
            lv_obj_add_flag(info_container_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    SleepScreen();
    ESP_LOGI(TAG, "state: Info -> ScreenOff");
}

void HomeLcdDisplay::OnStateTick() {
    if (state_.load() == ScreenState::ScreenOff) {
        return;
    }
    int64_t idle = NowMs() - last_activity_ms_.load();
    ScreenState s = state_.load();
    if (s == ScreenState::Active && idle >= kActiveToInfoMs) {
        EnterInfo();
    } else if (s == ScreenState::Info && idle >= kInfoToScreenOffMs) {
        EnterScreenOff();
    } else if (s == ScreenState::Info) {
        UpdateInfoClock();
    }
}

// ---- 背光 ----

void HomeLcdDisplay::WakeScreen() {
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->RestoreBrightness();  // NVS 记忆亮度 + esp_timer 渐变，跨任务安全
    }
}

void HomeLcdDisplay::SleepScreen() {
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(0);
    }
}

// ---- 信息屏构建 ----

void HomeLcdDisplay::BuildInfoScreen() {
    DisplayLockGuard lock(this);
    lv_style_transition_dsc_init(&g_value_fade_trans, kValueFadeProps, NULL, kValueFadeDurMs, 0,
                                 NULL);
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme == nullptr) {
        ESP_LOGE(TAG, "current_theme_ is null, skip info screen");
        return;
    }
    auto text_font = lvgl_theme->text_font();
    auto icon_font = lvgl_theme->icon_font();
    if (text_font == nullptr || icon_font == nullptr) {
        ESP_LOGE(TAG, "text/icon font is null, skip info screen");
        return;
    }
    lv_color_t text_color = lvgl_theme->text_color();

    // 全屏不透明容器，覆盖在基础 UI 之上；Active/ScreenOff 时隐藏。
    info_container_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(info_container_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(info_container_, 0, 0);
    lv_obj_set_style_pad_all(info_container_, 0, 0);
    lv_obj_set_style_border_width(info_container_, 0, 0);
    lv_obj_set_style_bg_color(info_container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(info_container_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(info_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(info_container_, LV_OBJ_FLAG_HIDDEN);

    // 主信息列：时间/日期/天气/连接，居中偏上。
    main_info_box_ = lv_obj_create(info_container_);
    lv_obj_set_size(main_info_box_, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(main_info_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_info_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(main_info_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(main_info_box_, 0, 0);
    lv_obj_set_style_pad_all(main_info_box_, 0, 0);
    lv_obj_set_style_pad_gap(main_info_box_, 6, 0);
    lv_obj_align(main_info_box_, LV_ALIGN_CENTER, 0, -24);

    auto make_info_label = [&](lv_obj_t* parent) {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_obj_set_style_text_font(lbl, text_font->font(), 0);
        lv_obj_set_style_text_color(lbl, text_color, 0);
        return lbl;
    };
    time_label_ = make_info_label(main_info_box_);
    lv_label_set_text(time_label_, "--:--");
    date_label_ = make_info_label(main_info_box_);
    lv_label_set_text(date_label_, "");
    weather_label_ = make_info_label(main_info_box_);
    lv_label_set_text(weather_label_, "天气 --");
    conn_label_ = make_info_label(main_info_box_);
    lv_label_set_text(conn_label_, "");

    // 底部控制条：音量组(左) + 亮度组(右)。位置避开圆屏边角（待真机校准）。
    controls_bar_ = lv_obj_create(info_container_);
    lv_obj_set_size(controls_bar_, lv_pct(80), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(controls_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls_bar_, 0, 0);
    lv_obj_set_style_pad_all(controls_bar_, 0, 0);
    lv_obj_align(controls_bar_, LV_ALIGN_BOTTOM_MID, 0, -70);

    // 构建一组 [图标 / (− 数值 +)]：column，顶部图标标识音量/亮度。
    auto build_group = [&](const char* icon, lv_obj_t** minus_btn, lv_obj_t** plus_btn,
                           lv_obj_t** value_label) -> lv_obj_t* {
        lv_obj_t* group = lv_obj_create(controls_bar_);
        lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(group, 0, 0);
        lv_obj_set_style_pad_all(group, 0, 0);
        lv_obj_set_style_pad_gap(group, 2, 0);

        lv_obj_t* ic = lv_label_create(group);
        lv_obj_set_style_text_font(ic, icon_font->font(), 0);
        lv_obj_set_style_text_color(ic, text_color, 0);
        lv_label_set_text(ic, icon);

        lv_obj_t* row = lv_obj_create(group);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_gap(row, 4, 0);

        *minus_btn = MakeStepButton(row, "-", text_font->font());
        lv_obj_add_event_cb(*minus_btn, step_button_event_cb, LV_EVENT_CLICKED, this);

        *value_label = lv_label_create(row);
        lv_obj_set_width(*value_label, 36);  // 固定宽，避免显隐引起按钮位移
        lv_obj_set_style_text_font(*value_label, text_font->font(), 0);
        lv_obj_set_style_text_color(*value_label, text_color, 0);
        lv_obj_set_style_text_align(*value_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(*value_label, "");
        lv_obj_set_style_transition(*value_label, &g_value_fade_trans, 0);  // 淡入淡出
        lv_obj_set_style_opa(*value_label, LV_OPA_TRANSP, 0);  // 初始不可见

        *plus_btn = MakeStepButton(row, "+", text_font->font());
        lv_obj_add_event_cb(*plus_btn, step_button_event_cb, LV_EVENT_CLICKED, this);
        return group;
    };

    build_group(MATERIAL_SYMBOLS_VOLUME_UP, &vol_minus_btn_, &vol_plus_btn_, &vol_value_label_);
    build_group(MATERIAL_SYMBOLS_BRIGHTNESS_6, &bright_minus_btn_, &bright_plus_btn_,
                &bright_value_label_);
}

// ---- 信息屏内容 ----

void HomeLcdDisplay::UpdateInfoClock() {
    if (time_label_ == nullptr || date_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    time_t now = time(NULL);
    struct tm tm_now;
    if (localtime_r(&now, &tm_now) != nullptr && tm_now.tm_year >= (2025 - 1900)) {
        char t[16];
        char d[32];
        strftime(t, sizeof(t), "%H:%M", &tm_now);
        snprintf(d, sizeof(d), "%d月%d日 周%s", tm_now.tm_mon + 1, tm_now.tm_mday,
                 kWeekdaysCn[tm_now.tm_wday]);
        lv_label_set_text(time_label_, t);
        lv_label_set_text(date_label_, d);
    } else {
        // 时间未同步（OTA settimeofday 之前）至 OTA 同步前无效，显示占位。
        lv_label_set_text(time_label_, "--:--");
        lv_label_set_text(date_label_, "");
    }
}

void HomeLcdDisplay::UpdateInfoConnection() {
    if (conn_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    const char* txt = nullptr;
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateIdle:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            txt = "已连接";
            break;
        case kDeviceStateStarting:
        case kDeviceStateWifiConfiguring:
        case kDeviceStateConnecting:
            txt = "连接中";
            break;
        default:
            txt = nullptr;  // 取不到则省略
            break;
    }
    if (txt != nullptr) {
        lv_label_set_text(conn_label_, txt);
        lv_obj_remove_flag(conn_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(conn_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void HomeLcdDisplay::UpdateInfoWeather() {
    if (weather_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    char buf[96];
    if (!weather_region_.empty()) {
        snprintf(buf, sizeof(buf), "%s %s°C %s 湿度%s%%", weather_region_.c_str(),
                 weather_temp_.c_str(), weather_desc_.c_str(), weather_humid_.c_str());
    } else {
        snprintf(buf, sizeof(buf), "%s°C %s 湿度%s%%", weather_temp_.c_str(),
                 weather_desc_.c_str(), weather_humid_.c_str());
    }
    lv_label_set_text(weather_label_, buf);
}

void HomeLcdDisplay::ShowValueLabel(lv_obj_t* label, const char* text) {
    if (label == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(label, text);
    lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    if (value_fade_timer_ != nullptr) {
        esp_timer_stop(value_fade_timer_);
        esp_timer_start_once(value_fade_timer_, kValueFadeDelayMs * 1000);
    }
}

// ---- 按钮动作（主任务）----

void HomeLcdDisplay::ChangeVolume(int delta) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int vol = codec->output_volume() + delta;
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 100) {
        vol = 100;
    }
    codec->SetOutputVolume(vol);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", vol);
    ShowValueLabel(vol_value_label_, buf);
    OnUserActivity();
}

void HomeLcdDisplay::ChangeBrightness(int delta) {
    auto backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        return;
    }
    int b = static_cast<int>(backlight->brightness()) + delta;
    if (b < 0) {
        b = 0;
    }
    if (b > 100) {
        b = 100;
    }
    backlight->SetBrightness(static_cast<uint8_t>(b), true);  // permanent: 存入 NVS
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", b);
    ShowValueLabel(bright_value_label_, buf);
    OnUserActivity();
}

// ---- 天气（独立任务）----

bool HomeLcdDisplay::FetchWeather() {
    char url[192];
    snprintf(url, sizeof(url), "http://wttr.in/%s?format=j1",
             CONFIG_WAVESHARE_ESP32_S3_TOUCH_LCD_1_85C_HOME_CITY);

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kWeatherTimeoutMs;
    config.buffer_size = kWeatherBufferSize;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(TAG, "weather: http init failed");
        return false;
    }

    std::string body;
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            // 流式累计 body（实测约 40KB，动态扩容，64KB 上限防跑飞）。
            body.reserve(16 * 1024);
            char buf[kWeatherBufferSize];
            int total = 0;
            int n = 0;
            while (total < static_cast<int>(kWeatherMaxBody) &&
                   (n = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
                body.append(buf, static_cast<size_t>(n));
                total += n;
            }
            ok = !body.empty();
        } else {
            ESP_LOGW(TAG, "weather: HTTP status %d", status);
        }
        esp_http_client_close(client);
    } else {
        ESP_LOGW(TAG, "weather: http open failed");
    }
    esp_http_client_cleanup(client);

    if (!ok) {
        return false;  // 失败保持上次值（或初始 "--"）
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "weather: json parse failed");
        return false;
    }
    std::string temp = "--";
    std::string desc = "--";
    std::string humid = "--";
    std::string region;
    cJSON* cc = cJSON_GetObjectItem(root, "current_condition");
    if (cJSON_IsArray(cc) && cJSON_GetArraySize(cc) > 0) {
        cJSON* c0 = cJSON_GetArrayItem(cc, 0);
        cJSON* t = cJSON_GetObjectItem(c0, "temp_C");
        if (cJSON_IsString(t) && t->valuestring != nullptr) {
            temp = t->valuestring;
        }
        cJSON* wc = cJSON_GetObjectItem(c0, "weatherCode");
        const char* zh = WeatherCodeToZh(cJSON_IsString(wc) ? wc->valuestring : nullptr);
        desc = (zh != nullptr) ? zh : "未知";
        cJSON* h = cJSON_GetObjectItem(c0, "humidity");
        if (cJSON_IsString(h) && h->valuestring != nullptr) {
            humid = h->valuestring;
        }
    }
    cJSON* na = cJSON_GetObjectItem(root, "nearest_area");
    if (cJSON_IsArray(na) && cJSON_GetArraySize(na) > 0) {
        cJSON* a0 = cJSON_GetArrayItem(na, 0);
        cJSON* rg = cJSON_GetObjectItem(a0, "region");
        if (cJSON_IsArray(rg) && cJSON_GetArraySize(rg) > 0) {
            cJSON* r0 = cJSON_GetArrayItem(rg, 0);
            cJSON* v = cJSON_GetObjectItem(r0, "value");
            if (cJSON_IsString(v) && v->valuestring != nullptr) {
                region = v->valuestring;
            }
        }
    }
    cJSON_Delete(root);

    // 结果经 Application::Schedule 回主任务更新 UI（weather_* 仅主任务访问）。
    Application::GetInstance().Schedule(
        [this, temp, desc, humid, region]() { ApplyWeather(temp, desc, humid, region); });
    return true;
}

void HomeLcdDisplay::ApplyWeather(const std::string& temp, const std::string& desc,
                                  const std::string& humid, const std::string& region) {
    weather_temp_ = temp;
    weather_desc_ = desc;
    weather_humid_ = humid;
    weather_region_ = region;
    if (state_.load() == ScreenState::Info) {
        UpdateInfoWeather();
    }
}

const char* HomeLcdDisplay::WeatherCodeToZh(const char* code) {
    if (code == nullptr) {
        return nullptr;
    }
    for (const auto& e : kWeatherCodeMap) {
        if (strcmp(e.code, code) == 0) {
            return e.zh;
        }
    }
    return nullptr;
}

int64_t HomeLcdDisplay::NowMs() {
    return esp_timer_get_time() / 1000;
}

// ---- 语音事件重写（先父类，再按需亮屏；防递归）----

void HomeLcdDisplay::SetStatus(const char* status) {
    SpiLcdDisplay::SetStatus(status);
    if (!suppress_wake_ && status != nullptr && status[0] != '\0') {
        WakeAndActivate();
    }
}

bool HomeLcdDisplay::SetEmotion(const char* emotion) {
    bool ret = SpiLcdDisplay::SetEmotion(emotion);
    // "sleepy" 表示正在入睡（SetPowerSaveMode(true) 内部会设置），不亮屏。
    if (!suppress_wake_ && emotion != nullptr && strcmp(emotion, "sleepy") != 0) {
        WakeAndActivate();
    }
    return ret;
}

void HomeLcdDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);
    // 空 content（如 SetPowerSaveMode 的 SetChatMessage("system","")）不亮屏。
    if (!suppress_wake_ && content != nullptr && content[0] != '\0') {
        WakeAndActivate();
    }
}

void HomeLcdDisplay::SetPowerSaveMode(bool on) {
    // 基类内部会调 SetEmotion("sleepy")/SetChatMessage("system","")，虚调用回到本类
    // 重写；suppress_wake_ 阻止它们把屏幕又点亮（防递归）。
    suppress_wake_ = true;
    SpiLcdDisplay::SetPowerSaveMode(on);
    suppress_wake_ = false;

    if (on) {
        EnterScreenOff();
    } else {
        WakeAndActivate();
    }
}
