/*
 * gps_service
 *
 * Responsibility: Owns PortA GPS UART parsing and baud detection.
 * Hardware ownership: UART1 on PortA pins RX=GPIO1, TX=GPIO2.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    bool valid_fix;
    uint8_t satellites;
    char date_utc[11];
    char time_utc[9];
    char grid8[9];
    uint32_t last_rx_ms;
    int active_baud;
    bool baud_locked;
} gps_service_state_t;

void gps_service_start(int preload_baud);
void gps_service_stop(void);
void gps_service_update(void);
bool gps_service_get_state(gps_service_state_t *out_state);
bool gps_service_take_baud_update(int *out_baud);

#ifdef __cplusplus
}
#endif
