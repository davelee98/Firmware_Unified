#ifndef OD_RUNTIME_TYPES_H
#define OD_RUNTIME_TYPES_H

/* Firmware-local RAM-only types, plus the canonical wire contract they are built from.
 *
 * WHY THIS FILE EXISTS. shared/protocol/opendisplay_structs.h is the single source of truth for
 * every config-packet struct, and it deliberately carries NO in-memory types -- its own header
 * says so: "NO repo-specific values (GPIO pin values, buffer sizes, GlobalConfig,
 * EncryptionSession, ImageData, PipeWriteState, ButtonState). Those move to a repo-local header
 * on adoption." This is that header for targets/nordic-zephyr.
 *
 * It replaces src/opendisplay_structs.h, which was a hand-written 319-line SUBSET of the
 * canonical 1242-line contract: 14 wire structs re-declared by hand, plus these two RAM-only
 * aggregates, plus unprefixed spellings of flag macros the canonical header carries with an OD_
 * prefix. Every one of those re-declarations was a copy that could drift silently -- and three
 * had: FlashConfig gained miso_pin/wp_pin/hold_pin names for bytes canonical still reserves,
 * DisplayConfig kept tag_type where canonical renamed it legacy_tag_type, and BinaryInputs kept
 * reserved_pin_N where canonical named them input_pin_N.
 *
 * The adoption was verified, not assumed: all 14 shared structs compile to identical sizes, and
 * all 150 common fields to identical offsets and sizes, under both headers. The only offset
 * differences were the reserved[] blocks that necessarily shrank as canonical named bytes out
 * of them -- a backward-compatible carve, per that header's own MINOR-bump policy.
 *
 * NAMING. targets/esp32-idf calls its equivalent src/structs.h. This one is not called that
 * because this target's include path also carries third_party/bb_epaper and the panel and uzlib
 * directories, where a header named structs.h is a collision waiting to happen; the od_ prefix
 * matches the rest of this target's local headers.
 */

#include <stdbool.h>
#include <stdint.h>

/* The canonical wire contract: config + message payload structs, OD_-prefixed enums and flag
 * macros, and (transitively) opendisplay_protocol.h framing constants -- CMD_*, RESP_*, PIPE_*,
 * and the config limits. Resolved from shared/protocol via OD_SHARED_INCLUDE_DIRS. */
#include "opendisplay_structs.h"

/* ------------------------------------------------------------------- RAM-only aggregates --- */

/* The parsed config, as this firmware holds it in memory. NOT a wire struct: it is not packed,
 * its field order is ours, and the counts and the `loaded` flags have no on-wire representation.
 * The wire form is the TLV blob that opendisplay_config_parser.c walks to fill this in. */
struct GlobalConfig {
	struct SystemConfig system_config;
	struct ManufacturerData manufacturer_data;
	struct PowerOption power_option;
	struct DisplayConfig displays[4];
	uint8_t display_count;
	struct LedConfig leds[4];
	uint8_t led_count;
	struct SensorData sensors[4];
	uint8_t sensor_count;
	struct DataBus data_buses[4];
	uint8_t data_bus_count;
	struct BinaryInputs binary_inputs[4];
	uint8_t binary_input_count;
	struct TouchController touch_controllers[4];
	uint8_t touch_controller_count;
	/* Canonical spelling is BuzzerConfig; the member keeps the passive_buzzers name the parser
	 * and every caller already use, matching targets/esp32-idf/src/structs.h. */
	struct BuzzerConfig passive_buzzers[4];
	uint8_t passive_buzzer_count;
	struct NfcConfig nfc_configs[2];
	uint8_t nfc_config_count;
	struct FlashConfig flash_configs[2];
	uint8_t flash_config_count;
	struct DataExtended data_extended;
	bool data_extended_loaded;
	struct WifiConfig wifi_config;
	bool wifi_config_loaded;
	uint8_t version;
	uint8_t minor_version;
	bool loaded;
};

/* Live session state for an authenticated link. Also RAM-only: the replay window, the activity
 * clocks and the attempt counters are this firmware's business and never leave the device. */
struct EncryptionSession {
	bool authenticated;
	uint8_t session_key[16];
	uint8_t session_id[8];
	uint64_t nonce_counter;
	uint64_t last_seen_counter;
	uint64_t replay_window[64];
	uint8_t replay_idx;
	uint32_t last_activity_ms;
	uint8_t integrity_failures;
	uint32_t session_start_ms;
	uint8_t auth_attempts;
	uint32_t last_auth_time_ms;
	uint8_t client_nonce[16];
	uint8_t server_nonce[16];
	uint8_t pending_server_nonce[16];
	uint32_t server_nonce_time_ms;
};

#endif /* OD_RUNTIME_TYPES_H */
