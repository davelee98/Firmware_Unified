#ifndef OPENDISPLAY_CONFIG_PARSER_H
#define OPENDISPLAY_CONFIG_PARSER_H

#include <stdbool.h>
#include <stdint.h>
#include "od_runtime_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool parseConfigBytes(uint8_t *configData, uint32_t configLen, struct od_config *globalConfig);

bool loadGlobalConfig(struct od_config *globalConfig);

const struct SecurityConfig *od_get_parsed_security(void);
bool od_security_key_set(void);

/* Atomic target-local view for callbacks that run outside the loop thread. */
bool od_security_enabled_snapshot(void);

#ifdef __cplusplus
}
#endif

#endif
