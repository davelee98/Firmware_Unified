#ifndef OPENDISPLAY_BUTTON_H
#define OPENDISPLAY_BUTTON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void opendisplay_button_init(void);
void opendisplay_button_process(void);

/*
 * Sleep up to timeout_ms, returning EARLY if a button edge arrives. Use this instead of a
 * bare k_msleep() in any idle loop: while disconnected the loop sleeps in 1000 ms chunks, so
 * a plain sleep delays publishing a press by up to a second -- and loses one shorter than
 * that entirely.
 */
void opendisplay_button_wait(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
