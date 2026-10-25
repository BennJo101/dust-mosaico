// ESP-Mosaico glue, using Espressif's official board support package (esp-mosaico-bsp).
//
// The BSP reads the board revision from eFuse and picks the right pins for V1.0 or V1.2, so
// nothing here hard-codes a GPIO. For reference, V1.2 uses:
//   display  CO5300 QSPI on SPI2: CS 50, CLK 42, D0 36, D1 51, D2 35, D3 9, RST 44, TE 43
//   I2C0     SDA 56, SCL 3: CST9220 touch 0x5A (INT 6), BMM150 0x11 (right) and 0x12 (left),
//            BMI270 0x69, ES8311 0x19, BQ27220 0x55; shared sensor interrupt on GPIO 2
//   power    VCC_3V3_CTRL 60 (low = display and module rails on), POWER_SWITCH 57
//   audio    ES8311 codec (mic and speaker, through the BSP's esp_codec_dev), NS4150B amp on 45
//   other    motor 8 (active high), AI button 7 (active low), BOOT 61
// The BSP must be built without LVGL (sdkconfig.defaults sets CONFIG_BSP_DISPLAY_LVGL_ENABLE=n);
// Dust draws its own frames.
#include "board_port.h"
#include <string.h>
#include <math.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "bsp/esp_mosaico.h"
#include "bsp/display.h"
#include "bsp/power.h"
#include "bsp/imu.h"
#include "bsp/magnetometer.h"
#include "bsp/motor.h"
#include "bsp/battery.h"
#include "driver/gpio.h"
#include "dust_render.h"

static const char *TAG = "board";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static bool s_imu, s_mag, s_motor, s_batt;
static int s_rot;   // quarter turns clockwise, applied to the panel; touch is remapped here

#define BAND_ROWS 48          // push the frame in 48-row bands (46 KB each) to stay under SPI transfer limits
#define MAG_UT_PER_LSB 1.0f   // CHECK: BMM150 fixed-point output scale (the BSP builds it without floating point).
                              // Only the residual threshold in main.c depends on it; calibration is relative.

esp_err_t board_port_init(void)
{
    bsp_board_variant_t variant;
    esp_err_t err = bsp_board_variant_get(&variant);
    if (err != ESP_OK) { ESP_LOGE(TAG, "unknown board revision (%s)", esp_err_to_name(err)); return err; }
    ESP_RETURN_ON_ERROR(bsp_power_init(), TAG, "power init");
    ESP_RETURN_ON_ERROR(bsp_power_set_vcc_3v3(true), TAG, "3.3 V rail");   // GPIO 60 low: display + module rails on

    bsp_display_config_t cfg = BSP_DISPLAY_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(bsp_display_new(&cfg, &s_panel), TAG, "display");
    esp_lcd_panel_disp_on_off(s_panel, true);
    if (bsp_touch_new(BSP_DISPLAY_ROTATE_0, &s_touch) != ESP_OK) { s_touch = NULL; ESP_LOGW(TAG, "touch not available"); }

    if (bsp_imu_init() == ESP_OK) {
        bsp_imu_config_t ic = BSP_IMU_CONFIG_DEFAULT();
        s_imu = bsp_imu_start(&ic) == ESP_OK;
    }
    if (!s_imu) ESP_LOGW(TAG, "BMI270 not available: lifting falls back to long-press");
    s_mag = bsp_magnetometer_init_all() == ESP_OK;
    if (!s_mag) ESP_LOGW(TAG, "BMM150s not available: neighbour sensing off");
    s_motor = bsp_motor_init() == ESP_OK;
    s_batt = bsp_battery_init() == ESP_OK;
    if (!s_batt) ESP_LOGW(TAG, "battery gauge not answering");
    gpio_config_t btn = { .pin_bit_mask = 1ULL << BSP_BUTTON_AI_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);   // AI button, active low
    return ESP_OK;
}

void board_port_flush(const uint16_t *fb)
{
    for (int y = 0; y < DUST_PANEL_PX; y += BAND_ROWS)
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, DUST_PANEL_PX, y + BAND_ROWS, fb + y * DUST_PANEL_PX);
}

bool board_port_touch(int *x, int *y)
{
    if (!s_touch) return false;
    uint16_t tx[1], ty[1], strength[1]; uint8_t n = 0;
    esp_lcd_touch_read_data(s_touch);
    if (!esp_lcd_touch_get_coordinates(s_touch, tx, ty, strength, &n, 1) || n == 0) return false;
    // touch is created at rotation 0; turn the point to match the rotated picture
    const int P = DUST_PANEL_PX - 1, a = tx[0], b = ty[0];
    switch (s_rot) {
        case 1: *x = P - b; *y = a; break;
        case 2: *x = P - a; *y = P - b; break;
        case 3: *x = b; *y = P - a; break;
        default: *x = a; *y = b; break;
    }
    return true;
}

bool board_port_accel(float g[3])
{
    if (!s_imu) return false;
    return bsp_imu_get_accel(&g[0], &g[1], &g[2]) == ESP_OK;
}

bool board_port_mag3(float v[2][3])
{
    if (!s_mag) return false;
    // v[0] = left sensor (BMM150 0x12, BSP_MAGNETOMETER_1), v[1] = right sensor (0x11, BSP_MAGNETOMETER_0).
    // Four samples averaged keeps the noise near 2 µT. Axes stay in the sensor's own frame: the
    // neighbour classifier works from each tile's own calibration, so it doesn't need them rotated.
    static const bsp_magnetometer_t ID[2] = { BSP_MAGNETOMETER_1, BSP_MAGNETOMETER_0 };
    float acc[2][3] = { { 0 } };
    for (int n = 0; n < 4; n++) for (int s = 0; s < 2; s++) {
        struct bmm150_mag_data d;
        if (bsp_magnetometer_read(ID[s], &d) != ESP_OK) return false;
        acc[s][0] += d.x; acc[s][1] += d.y; acc[s][2] += d.z;
    }
    for (int s = 0; s < 2; s++) for (int a = 0; a < 3; a++) v[s][a] = acc[s][a] / 4.0f * MAG_UT_PER_LSB;
    return true;
}

bool board_port_mag(float *ut)
{
    float v[2][3];
    if (!board_port_mag3(v)) return false;
    float best = 0;
    for (int s = 0; s < 2; s++) { float m = v[s][0] * v[s][0] + v[s][1] * v[s][1] + v[s][2] * v[s][2]; if (m > best) best = m; }
    *ut = sqrtf(best);
    return true;
}

void board_port_set_rotation(int q)
{
    // 0: as built, 1: 90° clockwise, 2: 180°, 3: 270°. The BSP rotates the CO5300 through MADCTL.
    static const bsp_display_rotation_t R[4] = { BSP_DISPLAY_ROTATE_0, BSP_DISPLAY_ROTATE_90, BSP_DISPLAY_ROTATE_180, BSP_DISPLAY_ROTATE_270 };
    s_rot = q & 3;
    if (s_panel) bsp_display_set_rotation(R[s_rot]);
}

void board_port_buzz(bool on)
{
    if (s_motor) bsp_motor_set(on);   // GPIO 8
}

bool board_port_battery(int *percent, float *temp_c)
{
    if (!s_batt) return false;
    bsp_battery_status_t st;
    if (bsp_battery_read(&st) != ESP_OK) return false;
    *percent = st.state_of_charge; *temp_c = st.temperature_c;
    return true;
}

bool board_port_ai_button(void) { return gpio_get_level(BSP_BUTTON_AI_GPIO) == 0; }
bool board_port_has_imu(void) { return s_imu; }
bool board_port_has_mag(void) { return s_mag; }

void *board_port_alloc(size_t bytes)
{
    void *p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
    return p;
}

bool board_port_swap_bytes(void) { return true; }   // CO5300 takes big-endian RGB565
