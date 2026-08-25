/* od_hal_gpio for ESP-IDF. See od_hal_gpio.h.
 *
 * Lifted from compat/arduino_compat.h's pinMode/digitalWrite/digitalRead, unchanged in
 * behaviour including the validity guard and its reasoning.
 */

#include "od_hal_gpio.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

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

void od_hal_gpio_set_mode_output(uint8_t cfg)
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
    /* No gpio_set_level(): gpio_config() does not touch the output latch, so the pad keeps
     * whatever it last held -- which is exactly what Arduino's pinMode(OUTPUT) did. */
    gpio_config(&io);
}

void od_hal_gpio_write(uint8_t cfg, bool level_high)
{
    if (!od_pin_valid(cfg)) {
        return;
    }
    gpio_set_level((gpio_num_t)cfg, level_high ? 1 : 0);
}

bool od_hal_gpio_pin_valid(uint8_t cfg)
{
    return od_pin_valid(cfg);
}

int od_hal_gpio_read(uint8_t cfg)
{
    if (!od_pin_valid(cfg)) {
        return 0;
    }
    return gpio_get_level((gpio_num_t)cfg);
}

/* ONE flag for both attach functions. Two would double-install: gpio_install_isr_service()
 * returns ESP_ERR_INVALID_STATE when it is already up, which is harmless here but relies on
 * IDF's behaviour rather than on this file being correct. */
static void ensure_isr_service(void)
{
    static bool started = false;
    if (!started) {
        gpio_install_isr_service(0);
        started = true;
    }
}

static gpio_int_type_t edge_to_idf(od_hal_gpio_edge_t edge)
{
    switch (edge) {
        case OD_GPIO_EDGE_RISING:  return GPIO_INTR_POSEDGE;
        case OD_GPIO_EDGE_FALLING: return GPIO_INTR_NEGEDGE;
        default:                   return GPIO_INTR_ANYEDGE;
    }
}

int od_hal_gpio_config_irq(uint8_t cfg, od_hal_gpio_edge_t edge, od_hal_gpio_irq_fn handler)
{
    if (!od_pin_valid(cfg) || handler == NULL) {
        return -1;
    }
    ensure_isr_service();
    gpio_set_intr_type((gpio_num_t)cfg, edge_to_idf(edge));
    /* Remove before add: re-attaching a pin must REPLACE its handler rather than fail, which
     * is what the callers assume -- touch_input re-attaches on every controller re-init. */
    gpio_isr_handler_remove((gpio_num_t)cfg);
    if (gpio_isr_handler_add((gpio_num_t)cfg, (gpio_isr_t)handler, NULL) != ESP_OK) {
        return -1;
    }
    gpio_intr_enable((gpio_num_t)cfg);
    return 0;
}

int od_hal_gpio_config_irq_arg(uint8_t cfg, od_hal_gpio_edge_t edge,
                               od_hal_gpio_irq_arg_fn handler, void *arg)
{
    if (!od_pin_valid(cfg) || handler == NULL) {
        return -1;
    }
    ensure_isr_service();
    gpio_set_intr_type((gpio_num_t)cfg, edge_to_idf(edge));
    gpio_isr_handler_remove((gpio_num_t)cfg);
    if (gpio_isr_handler_add((gpio_num_t)cfg, handler, arg) != ESP_OK) {
        return -1;
    }
    gpio_intr_enable((gpio_num_t)cfg);
    return 0;
}

void od_hal_gpio_irq_enable(uint8_t cfg)
{
    if (od_pin_valid(cfg)) {
        gpio_intr_enable((gpio_num_t)cfg);
    }
}

void od_hal_gpio_irq_disable(uint8_t cfg)
{
    if (od_pin_valid(cfg)) {
        gpio_intr_disable((gpio_num_t)cfg);
    }
}

void od_hal_gpio_clear_irq(uint8_t cfg)
{
    if (!od_pin_valid(cfg)) {
        return;
    }
    gpio_intr_disable((gpio_num_t)cfg);
    gpio_isr_handler_remove((gpio_num_t)cfg);
}

void od_hal_gpio_irq_lock(void)
{
    portDISABLE_INTERRUPTS();
}

void od_hal_gpio_irq_unlock(void)
{
    portENABLE_INTERRUPTS();
}
