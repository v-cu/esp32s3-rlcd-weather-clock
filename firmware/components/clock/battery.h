#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t Battery_Init(void);
esp_err_t Battery_ReadPercent(int *percent_out);
esp_err_t Battery_ReadVoltageMv(int *millivolts_out);

/* Li-ion open-circuit voltage -> approximate state of charge (0..100 %). */
int Battery_VoltageToPercent(int mv);

#ifdef __cplusplus
}
#endif