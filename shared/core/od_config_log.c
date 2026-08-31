/* Shared DEBUG dump of parsed configuration, with credentials represented only by metadata. */

#include "od_config.h"

#include "od_log.h"

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
static size_t bounded_len(const uint8_t *text, size_t cap)
{
    size_t n = 0u;

    while (n < cap && text[n] != 0u) {
        ++n;
    }
    return n;
}
#endif

void od_config_log_dump(const struct od_config *cfg)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    uint8_t i;

    if (cfg == NULL || !cfg->loaded) {
        od_log_debug("Config not loaded");
        return;
    }

    od_log_debug("=== Configuration Summary ===");
    od_log_debug("Version: %u.%u", (unsigned)cfg->version, (unsigned)cfg->minor_version);
    od_log_debug("--- System Configuration ---");
    od_log_debug("IC Type: 0x%04X", (unsigned)cfg->system_config.ic_type);
    od_log_debug("Communication Modes: 0x%02X",
                 (unsigned)cfg->system_config.communication_modes);
    od_log_debug("  BLE: %s", (cfg->system_config.communication_modes & OD_COMM_MODE_BLE) != 0u
                 ? "enabled" : "disabled");
    od_log_debug("  OEPL: %s", (cfg->system_config.communication_modes & OD_COMM_MODE_OEPL) != 0u
                 ? "enabled" : "disabled");
    od_log_debug("  WiFi: %s", (cfg->system_config.communication_modes & OD_COMM_MODE_WIFI) != 0u
                 ? "enabled" : "disabled");
    od_log_debug("Device Flags: 0x%02X", (unsigned)cfg->system_config.device_flags);
    od_log_debug("  PWR_PIN: %s", (cfg->system_config.device_flags
                                    & OD_DEVICE_FLAG_PWR_PIN) != 0u ? "enabled" : "disabled");
    od_log_debug("  XIAO_INIT: %s", (cfg->system_config.device_flags
                                      & OD_DEVICE_FLAG_XIAO_INIT) != 0u ? "enabled" : "disabled");
    od_log_debug("  WS_PP_INIT: %s", (cfg->system_config.device_flags
                                       & OD_DEVICE_FLAG_WS_PP_INIT) != 0u ? "enabled" : "disabled");
    od_log_debug("  PWR_LATCH: %s", (cfg->system_config.device_flags
                                      & OD_DEVICE_FLAG_PWR_LATCH) != 0u ? "enabled" : "disabled");
    od_log_debug("  PWR_LATCH_DFF: %s", (cfg->system_config.device_flags
                                          & OD_DEVICE_FLAG_PWR_LATCH_DFF) != 0u
                 ? "enabled" : "disabled");
    od_log_debug("Power Pins: %u / %u / %u", (unsigned)cfg->system_config.pwr_pin,
                 (unsigned)cfg->system_config.pwr_pin_2,
                 (unsigned)cfg->system_config.pwr_pin_3);

    od_log_debug("--- Manufacturer Data ---");
    od_log_debug("Manufacturer ID: 0x%04X",
                 (unsigned)cfg->manufacturer_data.manufacturer_id);
    od_log_debug("Board Type / Revision: %u / %u",
                 (unsigned)cfg->manufacturer_data.board_type,
                 (unsigned)cfg->manufacturer_data.board_revision);
    od_log_debug("Simple Config Indices: driver=%u display=%u power=%u",
                 (unsigned)cfg->manufacturer_data.simple_config_driver_index,
                 (unsigned)cfg->manufacturer_data.simple_config_display_index,
                 (unsigned)cfg->manufacturer_data.simple_config_power_index);
    od_log_debug("Simple Config Timestamp LE: %02X%02X%02X%02X%02X%02X",
                 (unsigned)cfg->manufacturer_data.simple_config_configured_at[5],
                 (unsigned)cfg->manufacturer_data.simple_config_configured_at[4],
                 (unsigned)cfg->manufacturer_data.simple_config_configured_at[3],
                 (unsigned)cfg->manufacturer_data.simple_config_configured_at[2],
                 (unsigned)cfg->manufacturer_data.simple_config_configured_at[1],
                 (unsigned)cfg->manufacturer_data.simple_config_configured_at[0]);

    od_log_debug("--- Power Configuration ---");
    od_log_debug("Power Mode: %u", (unsigned)cfg->power_option.power_mode);
    od_log_debug("Battery Capacity Bytes: %u %u %u",
                 (unsigned)cfg->power_option.battery_capacity_mah[0],
                 (unsigned)cfg->power_option.battery_capacity_mah[1],
                 (unsigned)cfg->power_option.battery_capacity_mah[2]);
    od_log_debug("Awake / Deep Sleep / Minimum Wake: %u ms / %u s / %u s",
                 (unsigned)cfg->power_option.sleep_timeout_ms,
                 (unsigned)cfg->power_option.deep_sleep_time_seconds,
                 (unsigned)cfg->power_option.min_wake_time_seconds);
    od_log_debug("TX Power / Sleep Flags: %u / 0x%02X",
                 (unsigned)cfg->power_option.tx_power,
                 (unsigned)cfg->power_option.sleep_flags);
    od_log_debug("Button Wake: %s", (cfg->power_option.sleep_flags
                                      & OD_SLEEP_FLAG_BUTTON_WAKE_DISABLE) == 0u
                 ? "enabled" : "disabled");
    od_log_debug("Screen Timeout: %u s", (unsigned)cfg->power_option.screen_timeout_seconds);
    od_log_debug("Battery Sense: pin=%u enable=%u flags=0x%02X estimator=%u scale=%u",
                 (unsigned)cfg->power_option.battery_sense_pin,
                 (unsigned)cfg->power_option.battery_sense_enable_pin,
                 (unsigned)cfg->power_option.battery_sense_flags,
                 (unsigned)cfg->power_option.capacity_estimator,
                 (unsigned)cfg->power_option.voltage_scaling_factor);
    od_log_debug("  Sense Enable Active Low: %s", (cfg->power_option.battery_sense_flags
                                                    & OD_BATTERY_SENSE_FLAG_ENABLE_INVERTED) != 0u
                 ? "enabled" : "disabled");
    od_log_debug("Deep Sleep Current: %u uA", (unsigned)cfg->power_option.deep_sleep_current_ua);
    od_log_debug("Charger: enable=%u state=%u flags=0x%02X",
                 (unsigned)cfg->power_option.charge_enable_pin,
                 (unsigned)cfg->power_option.charge_state_pin,
                 (unsigned)cfg->power_option.charger_flags);
    od_log_debug("  Enable / State Active Low: %s / %s",
                 (cfg->power_option.charger_flags & OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW) != 0u
                     ? "enabled" : "disabled",
                 (cfg->power_option.charger_flags & OD_CHARGER_FLAG_STATE_ACTIVE_LOW) != 0u
                     ? "enabled" : "disabled");

    od_log_debug("--- Display Configurations (%u) ---", (unsigned)cfg->display_count);
    for (i = 0u; i < cfg->display_count && i < (uint8_t)OD_CONFIG_MAX_DISPLAYS; ++i) {
        const struct DisplayConfig *d = &cfg->displays[i];

        od_log_debug("Display %u: instance=%u technology=0x%02X ic=0x%04X",
                     (unsigned)i, (unsigned)d->instance_number,
                     (unsigned)d->display_technology, (unsigned)d->panel_ic_type);
        od_log_debug("  Resolution / Size: %ux%u px / %ux%u mm",
                     (unsigned)d->pixel_width, (unsigned)d->pixel_height,
                     (unsigned)d->active_width_mm, (unsigned)d->active_height_mm);
        od_log_debug("  Tag / Rotation / Partial: 0x%04X / %u deg / %u",
                     (unsigned)d->legacy_tag_type, (unsigned)d->rotation * 90u,
                     (unsigned)d->partial_update_support);
        od_log_debug("  Pins: RST=%u BUSY=%u DC=%u CS=%u CS2=%u DATA=%u CLK=%u",
                     (unsigned)d->reset_pin, (unsigned)d->busy_pin, (unsigned)d->dc_pin,
                     (unsigned)d->cs_pin, (unsigned)d->cs_pin_2,
                     (unsigned)d->data_pin, (unsigned)d->clk_pin);
        od_log_debug("  Color / Modes / Full Update: 0x%02X / 0x%02X / %u mC",
                     (unsigned)d->color_scheme, (unsigned)d->transmission_modes,
                     (unsigned)d->full_update_mC);
        od_log_debug("    ZIPXL=%s ZIP=%s G5=%s DIRECT=%s PIPE=%s CLEAR_ON_BOOT=%s",
                     (d->transmission_modes & OD_TRANSMISSION_MODE_STREAMING_DECOMPRESSION) != 0u
                         ? "enabled" : "disabled",
                     (d->transmission_modes & OD_TRANSMISSION_MODE_ZIP) != 0u
                         ? "enabled" : "disabled",
                     (d->transmission_modes & OD_TRANSMISSION_MODE_G5) != 0u
                         ? "enabled" : "disabled",
                     (d->transmission_modes & OD_TRANSMISSION_MODE_DIRECT_WRITE) != 0u
                         ? "enabled" : "disabled",
                     (d->transmission_modes & OD_TRANSMISSION_MODE_PIPE_WRITE) != 0u
                         ? "enabled" : "disabled",
                     (d->transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT) != 0u
                         ? "enabled" : "disabled");
    }

    od_log_debug("--- LED Configurations (%u) ---", (unsigned)cfg->led_count);
    for (i = 0u; i < cfg->led_count && i < (uint8_t)OD_CONFIG_MAX_LEDS; ++i) {
        const struct LedConfig *led = &cfg->leds[i];

        od_log_debug("LED %u: instance=%u type=0x%02X pins=%u/%u/%u/%u flags=0x%02X",
                     (unsigned)i, (unsigned)led->instance_number, (unsigned)led->led_type,
                     (unsigned)led->led_1_r, (unsigned)led->led_2_g,
                     (unsigned)led->led_3_b, (unsigned)led->led_4,
                     (unsigned)led->led_flags);
    }

    od_log_debug("--- Sensor Configurations (%u) ---", (unsigned)cfg->sensor_count);
    for (i = 0u; i < cfg->sensor_count && i < (uint8_t)OD_CONFIG_MAX_SENSORS; ++i) {
        const struct SensorData *sensor = &cfg->sensors[i];

        od_log_debug("Sensor %u: instance=%u type=0x%04X bus=%u addr=0x%02X msd=%u",
                     (unsigned)i, (unsigned)sensor->instance_number,
                     (unsigned)sensor->sensor_type,
                     (unsigned)sensor->bus_id, (unsigned)sensor->i2c_addr_7bit,
                     (unsigned)sensor->msd_data_start_byte);
    }

    od_log_debug("--- Data Bus Configurations (%u) ---", (unsigned)cfg->data_bus_count);
    for (i = 0u; i < cfg->data_bus_count && i < (uint8_t)OD_CONFIG_MAX_DATA_BUSES; ++i) {
        const struct DataBus *bus = &cfg->data_buses[i];

        od_log_debug("Data Bus %u: instance=%u type=0x%02X speed=%u Hz",
                     (unsigned)i, (unsigned)bus->instance_number, (unsigned)bus->bus_type,
                     (unsigned)bus->bus_speed_hz);
        od_log_debug("  Pins: %u/%u/%u/%u/%u/%u/%u flags=0x%02X up=0x%02X down=0x%02X",
                     (unsigned)bus->pin_1, (unsigned)bus->pin_2, (unsigned)bus->pin_3,
                     (unsigned)bus->pin_4, (unsigned)bus->pin_5, (unsigned)bus->pin_6,
                     (unsigned)bus->pin_7, (unsigned)bus->bus_flags,
                     (unsigned)bus->pullups, (unsigned)bus->pulldowns);
    }

    od_log_debug("--- Binary Input Configurations (%u) ---",
                 (unsigned)cfg->binary_input_count);
    for (i = 0u; i < cfg->binary_input_count
                 && i < (uint8_t)OD_CONFIG_MAX_BINARY_INPUTS; ++i) {
        const struct BinaryInputs *input = &cfg->binary_inputs[i];

        od_log_debug("Binary Input %u: instance=%u type=0x%02X display=0x%02X",
                     (unsigned)i, (unsigned)input->instance_number,
                     (unsigned)input->input_type, (unsigned)input->display_as);
        od_log_debug("  Pins: %u/%u/%u/%u/%u/%u/%u/%u",
                     (unsigned)input->input_pin_1, (unsigned)input->input_pin_2,
                     (unsigned)input->input_pin_3, (unsigned)input->input_pin_4,
                     (unsigned)input->input_pin_5, (unsigned)input->input_pin_6,
                     (unsigned)input->input_pin_7, (unsigned)input->input_pin_8);
        od_log_debug("  Used / Invert / Pullups / Pulldowns: 0x%02X / 0x%02X / 0x%02X / 0x%02X",
                     (unsigned)input->pins_used, (unsigned)input->invert,
                     (unsigned)input->pullups, (unsigned)input->pulldowns);
        od_log_debug("  MSD / Power Off: byte=%u flags=0x%02X hold=%u s",
                     (unsigned)input->button_data_byte_index,
                     (unsigned)input->power_off_flags,
                     (unsigned)input->power_off_hold_sec);
    }

#if OD_CONFIG_WITH_WIFI
    if (cfg->wifi_config_loaded) {
        const struct WifiConfig *wifi = &cfg->wifi_config;
        const uint8_t *port = (const uint8_t *)(const void *)&wifi->server_port;
        const uint16_t server_port = (uint16_t)(((uint16_t)port[0] << 8) | port[1]);
        const size_t ssid_len = bounded_len(wifi->ssid, sizeof wifi->ssid);
        const bool password_set = bounded_len(wifi->password, sizeof wifi->password) != 0u;
        const bool server_set = bounded_len(wifi->server_host, sizeof wifi->server_host) != 0u;

        od_log_debug("--- WiFi Configuration ---");
        od_log_debug("SSID / Password: %u chars / %s",
                     (unsigned)ssid_len, password_set ? "set" : "empty");
        od_log_debug("Encryption / Server: 0x%02X / %s / port %u",
                     (unsigned)wifi->encryption_type, server_set ? "configured" : "empty",
                     (unsigned)server_port);
    }
#endif

    if (cfg->security_loaded) {
        const bool key_set = od_config_security_key_set(&cfg->security);

        od_log_debug("--- Security Configuration ---");
        od_log_debug("Encryption / Key Set / Timeout: %s / %s / %u s",
                     cfg->security.encryption_enabled != 0u ? "enabled" : "disabled",
                     key_set ? "yes" : "no",
                     (unsigned)cfg->security.session_timeout_seconds);
        od_log_debug("Flags / Reset Pin: 0x%02X / %u",
                     (unsigned)cfg->security.flags, (unsigned)cfg->security.reset_pin);
        od_log_debug("  Rewrite / Show Key / Reset Enabled: %s / %s / %s",
                     (cfg->security.flags & OD_SECURITY_FLAG_REWRITE_ALLOWED) != 0u
                         ? "enabled" : "disabled",
                     (cfg->security.flags & OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN) != 0u
                         ? "enabled" : "disabled",
                     (cfg->security.flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED) != 0u
                         ? "enabled" : "disabled");
        od_log_debug("  Reset Active High / Pull Up / Pull Down: %s / %s / %s",
                     (cfg->security.flags & OD_SECURITY_FLAG_RESET_PIN_POLARITY) != 0u
                         ? "enabled" : "disabled",
                     (cfg->security.flags & OD_SECURITY_FLAG_RESET_PIN_PULLUP) != 0u
                         ? "enabled" : "disabled",
                     (cfg->security.flags & OD_SECURITY_FLAG_RESET_PIN_PULLDOWN) != 0u
                         ? "enabled" : "disabled");
    }

#if OD_CONFIG_WITH_TOUCH
    od_log_debug("--- Touch Controllers (%u) ---", (unsigned)cfg->touch_controller_count);
    for (i = 0u; i < cfg->touch_controller_count && i < (uint8_t)OD_CONFIG_MAX_TOUCH; ++i) {
        const struct TouchController *touch = &cfg->touch_controllers[i];

        od_log_debug("Touch %u: instance=%u ic=%u bus=%u addr=0x%02X display=%u flags=0x%02X",
                     (unsigned)i, (unsigned)touch->instance_number,
                     (unsigned)touch->touch_ic_type, (unsigned)touch->bus_id,
                     (unsigned)touch->i2c_addr_7bit, (unsigned)touch->display_instance,
                     (unsigned)touch->flags);
        od_log_debug("  INT / RST / EN / Poll / MSD: %u / %u / %u / %u ms / %u",
                     (unsigned)touch->int_pin, (unsigned)touch->rst_pin,
                     (unsigned)touch->enable_pin, (unsigned)touch->poll_interval_ms,
                     (unsigned)touch->touch_data_start_byte);
    }
#endif

#if OD_CONFIG_WITH_BUZZER
    od_log_debug("--- Buzzers (%u) ---", (unsigned)cfg->passive_buzzer_count);
    for (i = 0u; i < cfg->passive_buzzer_count && i < (uint8_t)OD_CONFIG_MAX_BUZZERS; ++i) {
        const struct BuzzerConfig *buzzer = &cfg->passive_buzzers[i];

        od_log_debug("Buzzer %u: instance=%u drive=%u enable=%u flags=0x%02X duty=%u%%",
                     (unsigned)i, (unsigned)buzzer->instance_number,
                     (unsigned)buzzer->drive_pin, (unsigned)buzzer->enable_pin,
                     (unsigned)buzzer->flags, (unsigned)buzzer->duty_percent);
    }
#endif

#if OD_CONFIG_WITH_NFC
    od_log_debug("--- NFC Configurations (%u) ---", (unsigned)cfg->nfc_config_count);
    for (i = 0u; i < cfg->nfc_config_count && i < (uint8_t)OD_CONFIG_MAX_NFC; ++i) {
        const struct NfcConfig *nfc = &cfg->nfc_configs[i];

        od_log_debug("NFC %u: instance=%u ic=%u bus=%u flags=0x%02X",
                     (unsigned)i, (unsigned)nfc->instance_number,
                     (unsigned)nfc->nfc_ic_type, (unsigned)nfc->bus_instance,
                     (unsigned)nfc->flags);
        od_log_debug("  Field: pin=%u mode=%u active=%u debounce=%u ms adv=%u/%u",
                     (unsigned)nfc->field_detect_pin, (unsigned)nfc->field_detect_mode,
                     (unsigned)nfc->field_detect_active,
                     (unsigned)nfc->field_detect_debounce_ms,
                     (unsigned)nfc->adv_button_byte_index,
                     (unsigned)nfc->adv_button_button_id);
        od_log_debug("  Power: pin=%u active=%u on=%u ms off=%u ms",
                     (unsigned)nfc->power_pin, (unsigned)nfc->power_active,
                     (unsigned)nfc->power_on_delay_ms,
                     (unsigned)nfc->power_off_delay_ms);
    }
#endif

    od_log_debug("--- Flash Configurations (%u) ---", (unsigned)cfg->flash_config_count);
    for (i = 0u; i < cfg->flash_config_count && i < (uint8_t)OD_CONFIG_MAX_FLASH; ++i) {
        const struct FlashConfig *flash = &cfg->flash_configs[i];

        od_log_debug("Flash %u: instance=%u ic=%u bus=%u flags=0x%02X mode=%u",
                     (unsigned)i, (unsigned)flash->instance_number,
                     (unsigned)flash->flash_ic_type, (unsigned)flash->bus_instance,
                     (unsigned)flash->flags, (unsigned)flash->mode);
        od_log_debug("  MOSI / SCK / CS / Power: %u / %u / %u / %u",
                     (unsigned)flash->mosi_pin, (unsigned)flash->sck_pin,
                     (unsigned)flash->cs_pin, (unsigned)flash->power_pin);
        od_log_debug("  Power Active / On Delay / Off Delay: %u / %u ms / %u ms",
                     (unsigned)flash->power_active, (unsigned)flash->power_on_delay_ms,
                     (unsigned)flash->power_off_delay_ms);
    }

#if OD_CONFIG_WITH_DATA_EXTENDED
    if (cfg->data_extended_loaded) {
        od_log_debug("--- Data Extended ---");
        od_log_debug("  manufacturer_name: %s",
                     (const char *)(const void *)cfg->data_extended.manufacturer_name);
        od_log_debug("  model_name: %s",
                     (const char *)(const void *)cfg->data_extended.model_name);
        od_log_debug("  serial_number: %s",
                     (const char *)(const void *)cfg->data_extended.serial_number);
        od_log_debug("  friendly_name: %s",
                     (const char *)(const void *)cfg->data_extended.friendly_name);
        od_log_debug("  device_location: %s",
                     (const char *)(const void *)cfg->data_extended.device_location);
        od_log_debug("  device_id: %s",
                     (const char *)(const void *)cfg->data_extended.device_id);
        od_log_debug("  custom_string_1: %s",
                     (const char *)(const void *)cfg->data_extended.custom_string_1);
        od_log_debug("  custom_string_2: %s",
                     (const char *)(const void *)cfg->data_extended.custom_string_2);
        od_log_debug("  custom_string_3: %s",
                     (const char *)(const void *)cfg->data_extended.custom_string_3);
    }
#endif
    od_log_debug("=============================");
#else
    (void)cfg;
#endif
}
