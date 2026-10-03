#include "battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "Battery";

#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_3
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12
#define BATTERY_DIVIDER_RATIO   3.0f
#define BATTERY_CALIBRATION_FACTOR 1.0203f

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static bool                      s_cali_ok = false;

esp_err_t Battery_Init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_adc_handle);
    if (err != ESP_OK) return err;

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = BATTERY_ADC_ATTEN,
    };
    err = adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) return err;

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK);
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "ADC calibration unavailable, using raw conversion");
    }
    return ESP_OK;
}

static esp_err_t read_battery_mv(int *battery_mv_out)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw);
    if (err != ESP_OK) return err;

    int mv_adc;
    if (s_cali_ok) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &mv_adc);
    } else {
        mv_adc = raw * 3300 / 4095;
    }

    float battery_mv = mv_adc * BATTERY_DIVIDER_RATIO * BATTERY_CALIBRATION_FACTOR;
    *battery_mv_out = (int)(battery_mv + 0.5f);
    return ESP_OK;
}

int Battery_VoltageToPercent(int mv)
{
    static const struct { int mv; int pct; } curve[] = {
        {4200, 100}, {4100, 95}, {4000, 87}, {3900, 78}, {3800, 63},
        {3700, 45}, {3600, 25}, {3500, 12}, {3400, 6}, {3300, 2}, {3000, 0},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);

    if (mv >= curve[0].mv) return 100;
    if (mv <= curve[n - 1].mv) return 0;

    for (int i = 0; i < n - 1; i++) {
        if (mv <= curve[i].mv && mv >= curve[i + 1].mv) {
            int mv_hi = curve[i].mv, mv_lo = curve[i + 1].mv;
            int pct_hi = curve[i].pct, pct_lo = curve[i + 1].pct;
            return pct_lo + (mv - mv_lo) * (pct_hi - pct_lo) / (mv_hi - mv_lo);
        }
    }
    return 0;
}

esp_err_t Battery_ReadPercent(int *percent_out)
{
    int battery_mv;
    esp_err_t err = read_battery_mv(&battery_mv);
    if (err != ESP_OK) return err;

    *percent_out = Battery_VoltageToPercent(battery_mv);
    return ESP_OK;
}

esp_err_t Battery_ReadVoltageMv(int *millivolts_out)
{
    return read_battery_mv(millivolts_out);
}