/* od_hal_gpio for ESP-IDF. See od_hal_gpio.h.
 *
 * Lifted from compat/arduino_compat.h's pinMode/digitalWrite/digitalRead, unchanged in
 * behaviour including the validity guard and its reasoning.
 */

#include "od_hal_gpio.h"

#include "driver/gpio.h"

/* Pin numbers reach here from host-written config packets, where 0xFF is the "unused"
 * sentinel. Most call sites filter it, but `1ULL << 255` is undefined behaviour and one
 * unguarded path is enough -- Arduino-ESP32's own pinMode validates with GPIO_IS_VALID_GPIO
 * for the same reason. Reject rather than trust the caller.
 *
 * Note this rejects OD_PIN_UNUSED for free: 0xFF is not a valid GPIO on any supported part. */
static inline bool od_pin_valid(uint8_t cfg)
{
    return cfg < GPIO_NUM_MAX && GPIO_IS_VALID_GPIO(cfg);
}

void od_hal_gpio_config_output(uint8_t cfg, bool initial_high)
{
    if (!od_pin_valid(cfg)) {
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << (uint32_t)cfg),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level((gpio_num_t)cfg, initial_high ? 1 : 0);
}

void od_hal_gpio_config_input(uint8_t cfg, bool pull_up, bool pull_down)
{
    if (!od_pin_valid(cfg)) {
        return;
    }
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << (uint32_t)cfg),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        /* Pull-up wins if a caller asks for both -- see the header. */
        .pull_down_en = (pull_down && !pull_up) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

void od_hal_gpio_write(uint8_t cfg, bool level_high)
{
    if (!od_pin_valid(cfg)) {
        return;
    }
    gpio_set_level((gpio_num_t)cfg, level_high ? 1 : 0);
}

int od_hal_gpio_read(uint8_t cfg)
{
    if (!od_pin_valid(cfg)) {
        return 0;
    }
    return gpio_get_level((gpio_num_t)cfg);
}
