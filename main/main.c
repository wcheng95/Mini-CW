/*
 * Mini-CW ESP-IDF entry point.
 *
 * Responsibility: hand control to app_core.
 * Hardware ownership: none. Hardware is initialized only through service/HAL APIs.
 */

#include "app_core.h"

void app_main(void)
{
    app_core_init();
    app_core_run();
}
