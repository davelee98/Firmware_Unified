/* esp_log.h -- host stand-in, just enough for the ESP32 sources tests/host compiles.
 *
 * The bodies are discarded, not captured: no suite asserts on a log line, and a fake that recorded
 * them would invite one to. What matters is that the format string and its arguments are still
 * COMPILED, so a wrong conversion specifier is a build error here as it is on the target. */

#ifndef OD_TEST_FAKE_ESP_LOG_H
#define OD_TEST_FAKE_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) do { if (0) { (void)printf("%s" fmt, (tag), ##__VA_ARGS__); } } while (0)
#define ESP_LOGW(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)

#endif
