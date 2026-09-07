#include "od_nrf51_config.h"

#include "od_nrf51_profile.h"
#include "opendisplay_structs.h"

#include <stddef.h>

struct od_nrf51_static_config {
    struct OuterPacketHeader outer;
    struct SinglePacketHeader system_header;
    struct SystemConfig system;
    struct SinglePacketHeader manufacturer_header;
    struct ManufacturerData manufacturer;
    struct SinglePacketHeader power_header;
    struct PowerOption power;
    struct SinglePacketHeader display_header;
    struct DisplayConfig display;
    uint16_t crc;
} __attribute__((packed));

static const struct od_nrf51_static_config config = {
    .outer = {
        .length = sizeof(struct od_nrf51_static_config),
        .version = OD_CONFIG_VERSION,
    },
    .system_header = { .number = 0u, .id = OD_PKT_SYSTEM },
    .system = {
        .ic_type = 0xFFFFu,
        .communication_modes = OD_COMM_MODE_BLE,
        .pwr_pin = OD_PIN_UNUSED,
        .pwr_pin_2 = OD_PIN_UNUSED,
        .pwr_pin_3 = OD_PIN_UNUSED,
    },
    .manufacturer_header = { .number = 1u, .id = OD_PKT_MANUFACTURER },
    .manufacturer = { .manufacturer_id = OD_MANUFACTURER_DIY },
    .power_header = { .number = 2u, .id = OD_PKT_POWER },
    .power = {
        .power_mode = OD_POWER_MODE_BATTERY,
        .battery_sense_pin = OD_PIN_UNUSED,
        .battery_sense_enable_pin = OD_PIN_UNUSED,
        .charge_enable_pin = OD_PIN_UNUSED,
        .charge_state_pin = OD_PIN_UNUSED,
    },
    .display_header = { .number = 3u, .id = OD_PKT_DISPLAY },
    .display = {
        .instance_number = 0u,
        .display_technology = OD_DISPLAY_TECH_E_PAPER,
        .panel_ic_type = OD_PANEL_IC_EP213_104X212,
        .pixel_width = OD_NRF51_WIDTH,
        .pixel_height = OD_NRF51_HEIGHT,
        .rotation = OD_ROTATION_90,
        .reset_pin = 3u,
        .busy_pin = 4u,
        .dc_pin = 2u,
        .cs_pin = 1u,
        .data_pin = 30u,
        .partial_update_support = OD_PARTIAL_UPDATE_NONE,
        .color_scheme = OD_COLOR_SCHEME_MONO,
        .transmission_modes = OD_TRANSMISSION_MODE_DIRECT_WRITE,
        .clk_pin = 0u,
        .cs_pin_2 = OD_PIN_UNUSED,
    },
    .crc = 0x0709u,
};

OD_STATIC_ASSERT(sizeof(config) == 133u, "LT213A config wire size");
OD_STATIC_ASSERT(sizeof(config) <= OD_NRF51_STATIC_CONFIG_MAX_SIZE,
                 "LT213A config exceeds nRF51 static cap");

const uint8_t *od_nrf51_config_blob(uint16_t *length)
{
    if (length != NULL) {
        *length = (uint16_t)sizeof(config);
    }
    return (const uint8_t *)&config;
}
