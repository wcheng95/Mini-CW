#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_i2c_init(void);
esp_err_t board_i2c_get_bus(i2c_master_bus_handle_t* out_bus);

#ifdef __cplusplus
}
#endif