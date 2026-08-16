#ifndef DEVICE_CONTROL_H
#define DEVICE_CONTROL_H

#include "od_cmd.h"

#include <stdint.h>   // was <Arduino.h>, which this header used for uint8_t alone

void reboot();
void processButtonEvents();
void flashLed(uint8_t color, uint8_t brightness);
void processLedFlash();
void initButtons();
od_cmd_result_t handleLedActivate(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len);
od_cmd_result_t handleLedStop(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len);
/**
 * Stop LED playback immediately. DEEP SLEEP ONLY -- see buzzerStopForSleep() for
 * why this must not be called from abortToKnownState().
 */
void ledStopForSleep(void);
void enterDFUMode();
od_cmd_result_t handleDeepSleepCommand(const od_cmd_ctx_t *ctx, const uint8_t* payload, uint16_t payloadLen);
od_cmd_result_t handlePowerOffCommand(const od_cmd_ctx_t *ctx, const uint8_t* payload, uint16_t payloadLen);

#endif
