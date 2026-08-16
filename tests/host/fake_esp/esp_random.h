/* esp_random.h -- host stand-in, C11.0 scaffolding ONLY.
 *
 * targets/esp32-idf/hal/od_hal_crypto_random.c still calls esp_fill_random(), whose void return is
 * the defect under test: it cannot report failure. This header exists so the failing test can
 * compile that production file on the host, and goes away with the PSA rewrite at C11.1. */

#ifndef OD_TEST_FAKE_ESP_RANDOM_H
#define OD_TEST_FAKE_ESP_RANDOM_H

#include <stddef.h>
#include <stdint.h>

void esp_fill_random(void *buf, size_t len);

#endif
