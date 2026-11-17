#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"
#include "doom_port.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

// 实体键分工（2026-11-17 按需求改）：
//   KEY3  (GPIO18) = 开火 FIRE
//   BOOT  (GPIO0)  = 开门/使用 USE
//   PWR   (AXP2101 PWRON 短按) = ESC / 菜单
// PWR 长按 ~6 秒是 AXP2101 硬件关机，软件不介入。
#define DOOM_PORT_BOOT_GPIO GPIO_NUM_0
#define DOOM_PORT_KEY3_GPIO GPIO_NUM_18

// AXP2101（PWR 键所在 PMU）：I2C 0x34
// 0x41 = IRQ 使能 2，bit3 = POWERON 短按
// 0x49 = IRQ 状态 1，bit3 = POWERON 短按（RW1C，写 1 清除）
#define DOOM_PORT_AXP2101_ADDR 0x34
#define DOOM_PORT_AXP2101_IRQ_EN2 0x41
#define DOOM_PORT_AXP2101_IRQ_STS1 0x49
#define DOOM_PORT_AXP2101_PKEY_SHORT 0x08

#define DOOM_PORT_FRAME_W DOOMGENERIC_RESX
#define DOOM_PORT_FRAME_H DOOMGENERIC_RESY
#define DOOM_PORT_CHUNK_LINES 20
#define DOOM_PORT_LANDSCAPE 1
// 画面旋转角度（顺时针，单位度）：0 / 90 / 180 / 270。
// 推屏映射与触摸坐标共用 physical_to_logical()，改这一处画面和触摸一起转，不会错位。
// 480x480 方屏：0/180 度时游戏区 480x360，上下留 60px 黑边；
//               90/270 度时变成竖着的 360x480，左右留 60px 黑边。
#define DOOM_PORT_ROTATION_DEG 0
#define DOOM_PORT_LOGICAL_W BSP_LCD_V_RES
#define DOOM_PORT_LOGICAL_H BSP_LCD_H_RES
#define DOOM_PORT_GAME_W BSP_LCD_V_RES
#define DOOM_PORT_GAME_H 360
#define DOOM_PORT_GAME_X ((DOOM_PORT_LOGICAL_W - DOOM_PORT_GAME_W) / 2)
#define DOOM_PORT_GAME_Y ((DOOM_PORT_LOGICAL_H - DOOM_PORT_GAME_H) / 2)
#define DOOM_PORT_BOOT_DEBOUNCE_US 30000
#define DOOM_PORT_EVENT_QUEUE_LEN 32
#define DOOM_PORT_TOUCH_CENTER_W 160
#define DOOM_PORT_TOUCH_CENTER_H 130
#define DOOM_PORT_TOUCH_CENTER_X_MIN ((DOOM_PORT_LOGICAL_W - DOOM_PORT_TOUCH_CENTER_W) / 2)
#define DOOM_PORT_TOUCH_CENTER_X_MAX (DOOM_PORT_TOUCH_CENTER_X_MIN + DOOM_PORT_TOUCH_CENTER_W)
#define DOOM_PORT_TOUCH_CENTER_Y_MIN ((DOOM_PORT_LOGICAL_H - DOOM_PORT_TOUCH_CENTER_H) / 2)
#define DOOM_PORT_TOUCH_CENTER_Y_MAX (DOOM_PORT_TOUCH_CENTER_Y_MIN + DOOM_PORT_TOUCH_CENTER_H)
#define DOOM_PORT_TOUCH_MENU_CORNER_PX 90
#define DOOM_PORT_STATS_LOG_US 5000000

/* 触屏分区（按 Davis 2026-11-17 的截图与说明）：
 *   中央 160x130 = 使用/确认，优先级最高
 *   顶栏（屏高约 21%）：左 = 上一把武器，右 = 下一把武器
 *   下半屏：左 = 移动（拖动=平移/前后，幅度大=奔跑），右 = 视角（拖动=转身）
 */
#define DOOM_PORT_TOP_BAR_H 100
#define DOOM_PORT_ZONE_SPLIT_X (DOOM_PORT_LOGICAL_W / 2)
#define DOOM_PORT_DRAG_DEADZONE 16   /* 移动区死区 px */
#define DOOM_PORT_RUN_THRESHOLD 170  /* 拖出这个距离才算奔跑 */
#define DOOM_PORT_TURN_DEADZONE 20   /* 视角区死区 px：拖出它才转，回到圈内就停 */
#define DOOM_PORT_MAX_TOUCHES 5      /* 驱动与 esp_lcd_touch 都按 5 点配置 */
#define DOOM_PORT_TOUCH_MATCH_PX 80  /* 多指跟踪：两帧之间超过这个距离就算新手指 */
#define DOOM_PORT_TOUCH_TELEPORT_PX 160 /* 同一 ID 两帧间跳这么远，视为抬手后 ID 被复用 */

/* 45 秒无操作进入休眠（熄屏 + 静音），只有 PWR 键能唤醒 */
#define DOOM_PORT_IDLE_SLEEP_US (45LL * 1000 * 1000)

static const char *TAG = "doom_port";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static uint16_t *s_draw_buffers[2];
static uint8_t s_next_draw_buffer;
static uint16_t s_palette_swapped[256];
static bool s_palette_ready;

typedef enum {
    DOOM_INPUT_UP,
    DOOM_INPUT_DOWN,
    DOOM_INPUT_LEFT,
    DOOM_INPUT_RIGHT,
    DOOM_INPUT_FIRE,
    DOOM_INPUT_USE,
    DOOM_INPUT_ENTER,
    DOOM_INPUT_ESCAPE,
    DOOM_INPUT_STRAFE_L,
    DOOM_INPUT_STRAFE_R,
    DOOM_INPUT_RUN,
    DOOM_INPUT_PREV_WEAPON,
    DOOM_INPUT_NEXT_WEAPON,
    DOOM_INPUT_COUNT,
} doom_input_id_t;

typedef struct {
    int pressed;
    unsigned char key;
} doom_input_event_t;

static const unsigned char s_input_keys[DOOM_INPUT_COUNT] = {
    [DOOM_INPUT_UP] = KEY_UPARROW,
    [DOOM_INPUT_DOWN] = KEY_DOWNARROW,
    [DOOM_INPUT_LEFT] = KEY_LEFTARROW,
    [DOOM_INPUT_RIGHT] = KEY_RIGHTARROW,
    [DOOM_INPUT_FIRE] = KEY_FIRE,
    [DOOM_INPUT_USE] = KEY_USE,
    [DOOM_INPUT_ENTER] = KEY_ENTER,
    [DOOM_INPUT_ESCAPE] = KEY_ESCAPE,
    /* 平移用引擎自带的 strafeleft/straferight（不需要再按 strafe 修饰键） */
    [DOOM_INPUT_STRAFE_L] = KEY_STRAFE_L,
    [DOOM_INPUT_STRAFE_R] = KEY_STRAFE_R,
    /* key_speed 默认就是 KEY_RSHIFT，按住即奔跑 */
    [DOOM_INPUT_RUN] = KEY_RSHIFT,
    /* 换枪：与 m_controls.c 里的 key_prevweapon / key_nextweapon 绑定一致 */
    [DOOM_INPUT_PREV_WEAPON] = '[',
    [DOOM_INPUT_NEXT_WEAPON] = ']',
};

static bool s_input_down[DOOM_INPUT_COUNT];
static doom_input_event_t s_input_events[DOOM_PORT_EVENT_QUEUE_LEN];
static uint8_t s_input_event_head;
static uint8_t s_input_event_tail;
/* 一个按键的消抖状态；KEY3 与 BOOT 共用同一套逻辑 */
typedef struct {
    gpio_num_t gpio;
    bool raw;
    bool stable;
    int64_t last_change_us;
} doom_key_t;

static doom_key_t s_key_boot = {.gpio = DOOM_PORT_BOOT_GPIO};
static doom_key_t s_key_key3 = {.gpio = DOOM_PORT_KEY3_GPIO};
static i2c_master_dev_handle_t s_axp_dev;

static esp_err_t doom_port_pwr_key_init(void);
static void doom_port_enter_sleep(void);
static void doom_port_exit_sleep(void);

/* 触屏手势状态：按下时锁定分区，整个手势都按这个分区处理，手指滑过边界也不切区 */
typedef enum {
    DOOM_PORT_ZONE_NONE,
    DOOM_PORT_ZONE_USE,
    DOOM_PORT_ZONE_PREV_WEAPON,
    DOOM_PORT_ZONE_NEXT_WEAPON,
    DOOM_PORT_ZONE_MOVE,
    DOOM_PORT_ZONE_LOOK,
} doom_touch_zone_t;

/* 多指：每个触点一个槽位。驱动不提供 track_id，所以按“离上一帧位置最近”匹配 */
typedef struct {
    bool active;
    bool seen;
    uint8_t track_id;
    doom_touch_zone_t zone;
    int start_x;
    int start_y;
    int last_x;
    int last_y;
    int cur_x;
    int cur_y;
} doom_touch_slot_t;

static doom_touch_slot_t s_touch_slots[DOOM_PORT_MAX_TOUCHES];

/* 闲置休眠：屏幕关掉 + 静音，任何触摸或按键唤醒 */
static bool s_asleep;
static int64_t s_last_activity_us;

extern void doom_app_audio_mute(bool mute);
static bool s_input_ready;
static bool s_landscape_map_ready;
#if DOOM_PORT_ROTATION_DEG == 90 || DOOM_PORT_ROTATION_DEG == 270
/* 90/270 度：源 x 只随物理行变化，源 y 只随物理列变化 */
static int16_t s_landscape_src_x_by_phys_y[BSP_LCD_V_RES];
static int16_t s_landscape_src_y_by_phys_x[BSP_LCD_H_RES];
#else
/* 0/180 度：源 x 只随物理列变化，源 y 只随物理行变化 */
static int16_t s_landscape_src_x_by_phys_x[BSP_LCD_H_RES];
static int16_t s_landscape_src_y_by_phys_y[BSP_LCD_V_RES];
#endif
static uint32_t s_input_queue_drops;
static uint32_t s_touch_read_errors;
static uint32_t s_axp_read_errors;
static uint32_t s_axp_write_errors;
static uint32_t s_pwr_short_events;
static uint32_t s_draw_errors;

static uint16_t rgb565_swap(uint16_t color)
{
    return (uint16_t)((color << 8) | (color >> 8));
}

static uint16_t rgb565_from_color(struct color color)
{
    return (uint16_t)(((uint16_t)(color.r & 0xF8) << 8) |
                      ((uint16_t)(color.g & 0xFC) << 3) |
                      ((uint16_t)color.b >> 3));
}

/* 物理像素 -> 逻辑画布坐标。四个方向都支持，角度含义为「图案顺时针旋转多少度」。 */
static void physical_to_logical(int phys_x, int phys_y, int *logical_x, int *logical_y)
{
#if DOOM_PORT_ROTATION_DEG == 0
    *logical_x = phys_x;
    *logical_y = phys_y;
#elif DOOM_PORT_ROTATION_DEG == 90
    *logical_x = phys_y;
    *logical_y = BSP_LCD_H_RES - 1 - phys_x;
#elif DOOM_PORT_ROTATION_DEG == 180
    *logical_x = BSP_LCD_H_RES - 1 - phys_x;
    *logical_y = BSP_LCD_V_RES - 1 - phys_y;
#elif DOOM_PORT_ROTATION_DEG == 270
    *logical_x = BSP_LCD_V_RES - 1 - phys_y;
    *logical_y = phys_x;
#else
#error "DOOM_PORT_ROTATION_DEG 只支持 0 / 90 / 180 / 270"
#endif
}

static void ensure_landscape_map(void)
{
    if (s_landscape_map_ready) {
        return;
    }

#if DOOM_PORT_ROTATION_DEG == 90 || DOOM_PORT_ROTATION_DEG == 270
    for (int phys_y = 0; phys_y < BSP_LCD_V_RES; phys_y++) {
        int logical_x;
        int logical_y;
        physical_to_logical(0, phys_y, &logical_x, &logical_y);

        if (logical_x < DOOM_PORT_GAME_X || logical_x >= DOOM_PORT_GAME_X + DOOM_PORT_GAME_W) {
            s_landscape_src_x_by_phys_y[phys_y] = -1;
        } else {
            s_landscape_src_x_by_phys_y[phys_y] = (int16_t)(((logical_x - DOOM_PORT_GAME_X) * DOOM_PORT_FRAME_W) / DOOM_PORT_GAME_W);
        }
    }

    for (int phys_x = 0; phys_x < BSP_LCD_H_RES; phys_x++) {
        int logical_x;
        int logical_y;
        physical_to_logical(phys_x, 0, &logical_x, &logical_y);

        if (logical_y < DOOM_PORT_GAME_Y || logical_y >= DOOM_PORT_GAME_Y + DOOM_PORT_GAME_H) {
            s_landscape_src_y_by_phys_x[phys_x] = -1;
        } else {
            s_landscape_src_y_by_phys_x[phys_x] = (int16_t)(((logical_y - DOOM_PORT_GAME_Y) * DOOM_PORT_FRAME_H) / DOOM_PORT_GAME_H);
        }
    }
#else
    for (int phys_x = 0; phys_x < BSP_LCD_H_RES; phys_x++) {
        int logical_x;
        int logical_y;
        physical_to_logical(phys_x, 0, &logical_x, &logical_y);

        if (logical_x < DOOM_PORT_GAME_X || logical_x >= DOOM_PORT_GAME_X + DOOM_PORT_GAME_W) {
            s_landscape_src_x_by_phys_x[phys_x] = -1;
        } else {
            s_landscape_src_x_by_phys_x[phys_x] = (int16_t)(((logical_x - DOOM_PORT_GAME_X) * DOOM_PORT_FRAME_W) / DOOM_PORT_GAME_W);
        }
    }

    for (int phys_y = 0; phys_y < BSP_LCD_V_RES; phys_y++) {
        int logical_x;
        int logical_y;
        physical_to_logical(0, phys_y, &logical_x, &logical_y);

        if (logical_y < DOOM_PORT_GAME_Y || logical_y >= DOOM_PORT_GAME_Y + DOOM_PORT_GAME_H) {
            s_landscape_src_y_by_phys_y[phys_y] = -1;
        } else {
            s_landscape_src_y_by_phys_y[phys_y] = (int16_t)(((logical_y - DOOM_PORT_GAME_Y) * DOOM_PORT_FRAME_H) / DOOM_PORT_GAME_H);
        }
    }
#endif

    s_landscape_map_ready = true;
}

static esp_err_t ensure_draw_buffers(void)
{
    if (s_draw_buffers[0] != NULL && s_draw_buffers[1] != NULL) {
        return ESP_OK;
    }

    const size_t buffer_pixels = BSP_LCD_H_RES * DOOM_PORT_CHUNK_LINES;
    const size_t buffer_bytes = buffer_pixels * sizeof(uint16_t);

    for (int i = 0; i < 2; i++) {
        if (s_draw_buffers[i] == NULL) {
            s_draw_buffers[i] = heap_caps_malloc(buffer_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (s_draw_buffers[i] == NULL) {
                ESP_LOGE(TAG, "Failed to allocate %u-byte DMA draw buffer %d", (unsigned)buffer_bytes, i);
                return ESP_ERR_NO_MEM;
            }
        }
    }

    return ESP_OK;
}

static uint16_t *next_draw_buffer(void)
{
    uint16_t *buffer = s_draw_buffers[s_next_draw_buffer];
    s_next_draw_buffer ^= 1;
    return buffer;
}

void doom_port_set_panel(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;
}

void doom_port_set_touch(esp_lcd_touch_handle_t touch)
{
    s_touch = touch;
}

esp_err_t doom_port_init_input(void)
{
    const gpio_config_t key_cfg = {
        .pin_bit_mask = (1ULL << DOOM_PORT_BOOT_GPIO) | (1ULL << DOOM_PORT_KEY3_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&key_cfg);
    if (err != ESP_OK) {
        return err;
    }

    int64_t now_us = esp_timer_get_time();
    s_key_boot.raw = gpio_get_level(DOOM_PORT_BOOT_GPIO) == 0;
    s_key_boot.stable = s_key_boot.raw;
    s_key_boot.last_change_us = now_us;
    s_key_key3.raw = gpio_get_level(DOOM_PORT_KEY3_GPIO) == 0;
    s_key_key3.stable = s_key_key3.raw;
    s_key_key3.last_change_us = now_us;

    /* PWR 键走 AXP2101 中断状态位；失败只告警，不影响其他按键 */
    err = doom_port_pwr_key_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PWR key (AXP2101) init failed: %s", esp_err_to_name(err));
    }

    s_input_ready = true;
    s_last_activity_us = now_us;

    ESP_LOGI(TAG, "Input map: KEY3=fire, BOOT=use, PWR(short)=esc/wake, touch: center=use/enter, top=fprev/fnext weapon, lower-left=move(drag, big=run), lower-right=look");
    return ESP_OK;
}

static bool queue_input_event(int pressed, unsigned char key)
{
    uint8_t next_head = (uint8_t)((s_input_event_head + 1) % DOOM_PORT_EVENT_QUEUE_LEN);
    if (next_head == s_input_event_tail) {
        ESP_LOGW(TAG, "Input event queue full; dropping key %u", (unsigned)key);
        s_input_queue_drops++;
        return false;
    }

    s_input_events[s_input_event_head].pressed = pressed;
    s_input_events[s_input_event_head].key = key;
    s_input_event_head = next_head;
    return true;
}

static bool dequeue_input_event(doom_input_event_t *event)
{
    if (s_input_event_tail == s_input_event_head) {
        return false;
    }

    *event = s_input_events[s_input_event_tail];
    s_input_event_tail = (uint8_t)((s_input_event_tail + 1) % DOOM_PORT_EVENT_QUEUE_LEN);
    return true;
}

static void update_input_key(doom_input_id_t input, bool desired_down)
{
    if (s_input_down[input] == desired_down) {
        return;
    }

    if (queue_input_event(desired_down ? 1 : 0, s_input_keys[input])) {
        s_input_down[input] = desired_down;
    }
}

/* 读一个 GPIO 按键（低电平有效），30 ms 消抖，返回稳定后的按下状态 */
static bool poll_debounced_key(doom_key_t *key, int64_t now_us)
{
    bool raw_pressed = gpio_get_level(key->gpio) == 0;

    if (raw_pressed != key->raw) {
        key->raw = raw_pressed;
        key->last_change_us = now_us;
    }

    if ((now_us - key->last_change_us) >= DOOM_PORT_BOOT_DEBOUNCE_US) {
        key->stable = key->raw;
    }

    return key->stable;
}

/* AXP2101 单字节读写，只服务 PWR 键 */
static esp_err_t axp_read_reg(uint8_t reg, uint8_t *value)
{
    if (s_axp_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_axp_dev, &reg, 1, value, 1, 100);
}

static esp_err_t axp_write_reg(uint8_t reg, uint8_t value)
{
    if (s_axp_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_axp_dev, buf, sizeof(buf), 100);
}

/* 打开 PWR 短按中断，并清掉可能残留的旧状态 */
static esp_err_t doom_port_pwr_key_init(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DOOM_PORT_AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_axp_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t en = 0;
    err = axp_read_reg(DOOM_PORT_AXP2101_IRQ_EN2, &en);
    if (err != ESP_OK) {
        return err;
    }
    err = axp_write_reg(DOOM_PORT_AXP2101_IRQ_EN2, (uint8_t)(en | DOOM_PORT_AXP2101_PKEY_SHORT));
    if (err != ESP_OK) {
        return err;
    }

    /* 清一次旧状态，避免把上电那一下当成 ESC */
    return axp_write_reg(DOOM_PORT_AXP2101_IRQ_STS1, DOOM_PORT_AXP2101_PKEY_SHORT);
}

/* 休眠：熄屏 + 静音。Doom 主循环继续跑，但不推屏、不出声 */
static void doom_port_enter_sleep(void)
{
    if (s_asleep) {
        return;
    }

    s_asleep = true;
    for (int i = 0; i < DOOM_PORT_MAX_TOUCHES; i++) {
        s_touch_slots[i].active = false;
        s_touch_slots[i].seen = false;
    }

    if (s_panel != NULL) {
        esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Sleep: display off failed: %s", esp_err_to_name(err));
        }
    }
    doom_app_audio_mute(true);

    ESP_LOGI(TAG, "Idle sleep: display off + muted (touch / KEY3 / BOOT / PWR wakes)");
}

/* 唤醒：亮屏 + 恢复音量。唤醒这一帧的输入会被丢弃，不会误触 */
static void doom_port_exit_sleep(void)
{
    if (!s_asleep) {
        return;
    }

    s_asleep = false;

    if (s_panel != NULL) {
        esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wake: display on failed: %s", esp_err_to_name(err));
        }
    }
    doom_app_audio_mute(false);

    ESP_LOGI(TAG, "Wake");
}

/* PWR 短按是一次性中断位：读到即清除，本次轮询产生一个 ESC 脉冲 */
static bool poll_pwr_key_short(void)
{
    uint8_t sts = 0;
    if (axp_read_reg(DOOM_PORT_AXP2101_IRQ_STS1, &sts) != ESP_OK) {
        s_axp_read_errors++;
        return false;
    }

    if ((sts & DOOM_PORT_AXP2101_PKEY_SHORT) == 0) {
        return false;
    }

    if (axp_write_reg(DOOM_PORT_AXP2101_IRQ_STS1, DOOM_PORT_AXP2101_PKEY_SHORT) != ESP_OK) {
        s_axp_write_errors++;
    }
    return true;
}

/* 读一帧最多 5 个触点，转成逻辑坐标；同时带出触点 ID */
static uint8_t poll_touch_points(int *xs, int *ys, uint8_t *ids)
{
    if (s_touch == NULL) {
        return 0;
    }

    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    if (err != ESP_OK) {
        static int64_t last_warn_us;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_warn_us > 2000000) {
            ESP_LOGW(TAG, "Touch read failed: %s", esp_err_to_name(err));
            last_warn_us = now_us;
        }
        s_touch_read_errors++;
        return 0;
    }

    esp_lcd_touch_point_data_t points[DOOM_PORT_MAX_TOUCHES];
    uint8_t count = 0;
    err = esp_lcd_touch_get_data(s_touch, points, &count, DOOM_PORT_MAX_TOUCHES);
    if (err != ESP_OK || count == 0) {
        return 0;
    }
    if (count > DOOM_PORT_MAX_TOUCHES) {
        count = DOOM_PORT_MAX_TOUCHES;
    }

    for (uint8_t i = 0; i < count; i++) {
        physical_to_logical(points[i].x, points[i].y, &xs[i], &ys[i]);
        ids[i] = points[i].track_id;
    }

    /* 多指验证用：只在点数变化且 ≥2 时打一行，确认面板真的报多点 */
    static uint8_t s_last_count_logged;
    if (count != s_last_count_logged && count >= 2) {
        ESP_LOGI(TAG, "Multi-touch: %u points", (unsigned)count);
    }
    s_last_count_logged = count;

    return count;
}

/* 按落点判定分区；中央「使用/确认」优先级最高 */
static doom_touch_zone_t classify_touch(int x, int y)
{
    if (x >= DOOM_PORT_TOUCH_CENTER_X_MIN && x <= DOOM_PORT_TOUCH_CENTER_X_MAX &&
        y >= DOOM_PORT_TOUCH_CENTER_Y_MIN && y <= DOOM_PORT_TOUCH_CENTER_Y_MAX) {
        return DOOM_PORT_ZONE_USE;
    }

    if (y < DOOM_PORT_TOP_BAR_H) {
        return (x < DOOM_PORT_ZONE_SPLIT_X) ? DOOM_PORT_ZONE_PREV_WEAPON : DOOM_PORT_ZONE_NEXT_WEAPON;
    }

    return (x < DOOM_PORT_ZONE_SPLIT_X) ? DOOM_PORT_ZONE_MOVE : DOOM_PORT_ZONE_LOOK;
}

/* 按槽位驱动按键；坐标用 cur，和 last 的差就是这一帧的位移 */
static void apply_touch_slot(doom_touch_slot_t *slot, bool desired[DOOM_INPUT_COUNT])
{
    switch (slot->zone) {
    case DOOM_PORT_ZONE_USE:
        desired[DOOM_INPUT_USE] = true;
        desired[DOOM_INPUT_ENTER] = true;
        break;

    case DOOM_PORT_ZONE_PREV_WEAPON:
        desired[DOOM_INPUT_PREV_WEAPON] = true;
        break;

    case DOOM_PORT_ZONE_NEXT_WEAPON:
        desired[DOOM_INPUT_NEXT_WEAPON] = true;
        break;

    case DOOM_PORT_ZONE_MOVE: {
        int dx = slot->cur_x - slot->start_x;
        int dy = slot->cur_y - slot->start_y;
        int ax = abs(dx);
        int ay = abs(dy);

        if (dx > DOOM_PORT_DRAG_DEADZONE) {
            desired[DOOM_INPUT_STRAFE_R] = true;
        } else if (dx < -DOOM_PORT_DRAG_DEADZONE) {
            desired[DOOM_INPUT_STRAFE_L] = true;
        }

        if (dy < -DOOM_PORT_DRAG_DEADZONE) {
            desired[DOOM_INPUT_UP] = true;
        } else if (dy > DOOM_PORT_DRAG_DEADZONE) {
            desired[DOOM_INPUT_DOWN] = true;
        }

        /* 拖得远就叠加奔跑 */
        if ((ax > ay ? ax : ay) >= DOOM_PORT_RUN_THRESHOLD) {
            desired[DOOM_INPUT_RUN] = true;
        }
        break;
    }

    case DOOM_PORT_ZONE_LOOK: {
        /* 右摇杆语义：看相对起点的位移，离开死区就一直转，回中即停 */
        int dx = slot->cur_x - slot->start_x;

        if (dx > DOOM_PORT_TURN_DEADZONE) {
            desired[DOOM_INPUT_RIGHT] = true;
        } else if (dx < -DOOM_PORT_TURN_DEADZONE) {
            desired[DOOM_INPUT_LEFT] = true;
        }
        break;
    }

    default:
        break;
    }
}

/* 把这一帧的触点分配到槽位（优先按 track_id，退路是最近邻），再按槽位生成按键 */
static void update_touch_slots(uint8_t count, const int *xs, const int *ys, const uint8_t *ids,
                               bool desired[DOOM_INPUT_COUNT])
{
    for (int s = 0; s < DOOM_PORT_MAX_TOUCHES; s++) {
        s_touch_slots[s].seen = false;
    }

    for (uint8_t p = 0; p < count; p++) {
        int best = -1;

        /* 1) 先按触点 ID 精确匹配：手指靠得再近也不会串。
         *    但要求位置没有“瞬移”——抬手后芯片可能把同一个 ID 复用给新落的手指，
         *    不加这道判断，新手指出现在 3 区会被当成 4 区的旧手指继续转视角。 */
        for (int s = 0; s < DOOM_PORT_MAX_TOUCHES; s++) {
            if (!s_touch_slots[s].active || s_touch_slots[s].seen ||
                s_touch_slots[s].track_id != ids[p]) {
                continue;
            }
            int dx = xs[p] - s_touch_slots[s].last_x;
            int dy = ys[p] - s_touch_slots[s].last_y;
            if (dx * dx + dy * dy <= DOOM_PORT_TOUCH_TELEPORT_PX * DOOM_PORT_TOUCH_TELEPORT_PX) {
                best = s;
                break;
            }
        }

        /* 2) ID 对不上时退回最近邻（防止某些固件不给 ID） */
        if (best < 0) {
            int best_d2 = DOOM_PORT_TOUCH_MATCH_PX * DOOM_PORT_TOUCH_MATCH_PX;
            for (int s = 0; s < DOOM_PORT_MAX_TOUCHES; s++) {
                if (!s_touch_slots[s].active || s_touch_slots[s].seen) {
                    continue;
                }
                int dx = xs[p] - s_touch_slots[s].last_x;
                int dy = ys[p] - s_touch_slots[s].last_y;
                int d2 = dx * dx + dy * dy;
                if (d2 <= best_d2) {
                    best_d2 = d2;
                    best = s;
                }
            }
        }

        if (best < 0) {
            /* 新手指：找一个空槽位，按落点锁定分区 */
            for (int s = 0; s < DOOM_PORT_MAX_TOUCHES; s++) {
                if (!s_touch_slots[s].active) {
                    best = s;
                    break;
                }
            }
            if (best < 0) {
                continue; /* 槽位用满，忽略多出来的手指 */
            }
            s_touch_slots[best].active = true;
            s_touch_slots[best].zone = classify_touch(xs[p], ys[p]);
            s_touch_slots[best].start_x = xs[p];
            s_touch_slots[best].start_y = ys[p];
            s_touch_slots[best].last_x = xs[p];
            s_touch_slots[best].last_y = ys[p];
        }

        s_touch_slots[best].seen = true;
        s_touch_slots[best].track_id = ids[p];
        s_touch_slots[best].cur_x = xs[p];
        s_touch_slots[best].cur_y = ys[p];
    }

    for (int s = 0; s < DOOM_PORT_MAX_TOUCHES; s++) {
        doom_touch_slot_t *slot = &s_touch_slots[s];

        if (!slot->seen) {
            slot->active = false;
            continue;
        }
        if (!slot->active) {
            continue;
        }

        apply_touch_slot(slot, desired);
        slot->last_x = slot->cur_x;
        slot->last_y = slot->cur_y;
    }
}

static void poll_input(void)
{
    bool desired[DOOM_INPUT_COUNT] = {0};

    if (!s_input_ready) {
        return;
    }

    int64_t now_us = esp_timer_get_time();

    /* KEY3 = 开火；BOOT = 开门/使用；PWR 短按 = ESC（同时也能唤醒） */
    desired[DOOM_INPUT_FIRE] = poll_debounced_key(&s_key_key3, now_us);
    desired[DOOM_INPUT_USE] = poll_debounced_key(&s_key_boot, now_us);
    desired[DOOM_INPUT_ESCAPE] = poll_pwr_key_short();

    int xs[DOOM_PORT_MAX_TOUCHES];
    int ys[DOOM_PORT_MAX_TOUCHES];
    uint8_t ids[DOOM_PORT_MAX_TOUCHES];
    uint8_t touch_count = poll_touch_points(xs, ys, ids);
    update_touch_slots(touch_count, xs, ys, ids, desired);

    bool activity = touch_count > 0;
    for (doom_input_id_t i = 0; i < DOOM_INPUT_COUNT; i++) {
        if (desired[i]) {
            activity = true;
        }
    }

    /* 唤醒只认 PWR（休眠时它的 ESC 脉冲就代表唤醒） */
    bool wake_request = desired[DOOM_INPUT_ESCAPE];

    if (s_asleep) {
        if (wake_request) {
            doom_port_exit_sleep();
            s_last_activity_us = now_us;
        }
        /* 休眠期间/刚唤醒这一帧：输入全部作废，避免误开枪或弹菜单 */
        memset(desired, 0, sizeof(desired));
    } else if (activity) {
        s_last_activity_us = now_us;
    } else if ((now_us - s_last_activity_us) >= DOOM_PORT_IDLE_SLEEP_US) {
        doom_port_enter_sleep();
        memset(desired, 0, sizeof(desired));
    }

    for (doom_input_id_t input = 0; input < DOOM_INPUT_COUNT; input++) {
        update_input_key(input, desired[input]);
    }
}

static esp_err_t fill_screen_rgb565(uint16_t color);

/* 开机把面板刷成黑色。原来这里画七彩自检图，看着像马赛克，2026-11-17 按 Davis 要求去掉。 */
esp_err_t doom_port_clear_screen(void)
{
    return fill_screen_rgb565(0x0000);
}

static esp_err_t fill_screen_rgb565(uint16_t color)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ensure_draw_buffers();
    if (err != ESP_OK) {
        return err;
    }

    const uint16_t panel_color = rgb565_swap(color);

    for (int y = 0; y < BSP_LCD_V_RES; y += DOOM_PORT_CHUNK_LINES) {
        int lines = BSP_LCD_V_RES - y;
        if (lines > DOOM_PORT_CHUNK_LINES) {
            lines = DOOM_PORT_CHUNK_LINES;
        }

        uint16_t *draw_buffer = next_draw_buffer();
        for (int i = 0; i < BSP_LCD_H_RES * lines; i++) {
            draw_buffer[i] = panel_color;
        }

        err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_H_RES, y + lines, draw_buffer);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t draw_doom_frame(void)
{
    /* 休眠时不推屏：省电，也避免往关掉的屏上写 */
    if (s_asleep) {
        return ESP_OK;
    }

    if (s_panel == NULL || DG_ScreenBuffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ensure_draw_buffers();
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t *src_frame = (const uint8_t *)DG_ScreenBuffer;
    ensure_landscape_map();

    if (palette_changed || !s_palette_ready) {
        for (int i = 0; i < 256; i++) {
            s_palette_swapped[i] = rgb565_swap(rgb565_from_color(colors[i]));
        }
        palette_changed = false;
        s_palette_ready = true;
    }

    for (int y = 0; y < BSP_LCD_V_RES; y += DOOM_PORT_CHUNK_LINES) {
        int lines = BSP_LCD_V_RES - y;
        if (lines > DOOM_PORT_CHUNK_LINES) {
            lines = DOOM_PORT_CHUNK_LINES;
        }

        uint16_t *draw_buffer = next_draw_buffer();

        for (int row = 0; row < lines; row++) {
            uint16_t *dst = draw_buffer + row * BSP_LCD_H_RES;

#if DOOM_PORT_ROTATION_DEG == 90 || DOOM_PORT_ROTATION_DEG == 270
            /* 90/270：源 x 整行不变，源 y 逐列查表 */
            int16_t src_x = s_landscape_src_x_by_phys_y[y + row];
            for (int x = 0; x < BSP_LCD_H_RES; x++) {
                int16_t src_y = s_landscape_src_y_by_phys_x[x];
                dst[x] = (src_x < 0 || src_y < 0)
                             ? 0
                             : s_palette_swapped[src_frame[src_y * DOOM_PORT_FRAME_W + src_x]];
            }
#else
            /* 0/180：源 y 整行不变，源 x 逐列查表 */
            int16_t src_y = s_landscape_src_y_by_phys_y[y + row];
            for (int x = 0; x < BSP_LCD_H_RES; x++) {
                int16_t src_x = s_landscape_src_x_by_phys_x[x];
                dst[x] = (src_x < 0 || src_y < 0)
                             ? 0
                             : s_palette_swapped[src_frame[src_y * DOOM_PORT_FRAME_W + src_x]];
            }
#endif
        }

        err = esp_lcd_panel_draw_bitmap(s_panel,
                                        0,
                                        y,
                                        BSP_LCD_H_RES,
                                        y + lines,
                                        draw_buffer);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

void DG_Init(void)
{
    ESP_LOGI(TAG, "DG_Init: display %s", s_panel ? "ready" : "not initialized");

    if (s_panel != NULL) {
        esp_err_t err = fill_screen_rgb565(0x0000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to clear diagnostic frame: %s", esp_err_to_name(err));
        }
    }
}

void DG_DrawFrame(void)
{
    static uint32_t frame_count;
    static int64_t last_log_us;
    static uint64_t draw_total_us;
    static uint32_t draw_max_us;

    int64_t draw_start_us = esp_timer_get_time();
    esp_err_t err = draw_doom_frame();
    int64_t draw_us = esp_timer_get_time() - draw_start_us;
    if (err != ESP_OK) {
        s_draw_errors++;
        ESP_LOGW(TAG, "Draw frame skipped: %s", esp_err_to_name(err));
        DG_SleepMs(1000);
        return;
    }

    frame_count++;
    draw_total_us += (uint64_t)draw_us;
    if (draw_us > draw_max_us) {
        draw_max_us = (uint32_t)draw_us;
    }

    int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us >= DOOM_PORT_STATS_LOG_US) {
        uint32_t fps = 0;
        uint32_t draw_avg_us = frame_count > 0 ? (uint32_t)(draw_total_us / frame_count) : 0;

        if (last_log_us != 0) {
            fps = (uint32_t)((frame_count * 1000000ULL) / (now_us - last_log_us));
        }

        ESP_LOGI(TAG,
                 "DG_DrawFrame: frames=%" PRIu32 " fps=%" PRIu32 " draw_avg_us=%" PRIu32 " draw_max_us=%" PRIu32
                 " internal=%u/%u min=%u psram=%u/%u min=%u stack_free=%u draw_err=%" PRIu32
                 " input_drop=%" PRIu32 " touch_err=%" PRIu32 " axp_rd_err=%" PRIu32 " axp_wr_err=%" PRIu32 " pwr=%" PRIu32,
                 frame_count,
                 fps,
                 draw_avg_us,
                 draw_max_us,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 s_draw_errors,
                 s_input_queue_drops,
                 s_touch_read_errors,
                 s_axp_read_errors,
                 s_axp_write_errors,
                 s_pwr_short_events);

        frame_count = 0;
        draw_total_us = 0;
        draw_max_us = 0;
        last_log_us = now_us;
    }
}

void DG_SleepMs(uint32_t ms)
{
    if (ms == 0) {
        taskYIELD();
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    doom_input_event_t event;
    if (dequeue_input_event(&event)) {
        *pressed = event.pressed;
        *key = event.key;
        return 1;
    }

    poll_input();

    if (!dequeue_input_event(&event)) {
        return 0;
    }

    *pressed = event.pressed;
    *key = event.key;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    ESP_LOGI(TAG, "Doom title: %s", title ? title : "(null)");
}
