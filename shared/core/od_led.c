#include "od_led.h"

#include "od_led_app.h"
#include "od_hal_time.h"
#include "opendisplay_protocol.h"

#include <string.h>

/* One PWM slice. Seven per brightness step; the ramp below is the authority's
 * (../Firmware device_control.cpp flashLed), transcribed unchanged. */
#define OD_LED_PWM_DELAY_US 100u

/* The phase order is the pattern's structure: three loops, each with an inter-loop delay, wrapped
 * in a group that repeats. */
typedef enum {
    OD_LED_PHASE_IDLE = 0,
    OD_LED_PHASE_GROUP,
    OD_LED_PHASE_LOOP1,
    OD_LED_PHASE_LOOP1_DELAY,
    OD_LED_PHASE_INTER1_DELAY,
    OD_LED_PHASE_LOOP2,
    OD_LED_PHASE_LOOP2_DELAY,
    OD_LED_PHASE_INTER2_DELAY,
    OD_LED_PHASE_LOOP3,
    OD_LED_PHASE_LOOP3_DELAY,
    OD_LED_PHASE_INTER3_DELAY
} od_led_phase_t;

/* An inter-loop delay unit is 100 ms on the wire. */
#define OD_LED_DELAY_FACTOR_MS 100u

static struct {
    bool active;
    uint8_t instance;
    struct od_led_pins pins;

    uint8_t brightness;             /* 1..16 */
    uint8_t c1, c2, c3;
    uint8_t loop1delay, loop2delay, loop3delay;
    uint8_t loopcnt1, loopcnt2, loopcnt3;
    uint8_t ildelay1, ildelay2, ildelay3;
    uint8_t grouprepeats;
    bool repeat_forever;

    uint8_t group_pos;
    uint8_t i1, i2, i3;
    od_led_phase_t phase;

    bool waiting;
    uint32_t deadline_ms;
} s_run;

/* Wrap-safe: true once `now` has reached `deadline`, across the uint32 millisecond rollover. */
static bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000u;
}

static void led_all_off(void)
{
    const bool inv_r = (s_run.pins.flags & 0x01u) != 0u;
    const bool inv_g = (s_run.pins.flags & 0x02u) != 0u;
    const bool inv_b = (s_run.pins.flags & 0x04u) != 0u;

    od_led_app_write(s_run.pins.r, inv_r);
    od_led_app_write(s_run.pins.g, inv_g);
    od_led_app_write(s_run.pins.b, inv_b);
}

/* One flash: `brightness` passes of a seven-slice software PWM. The threshold order
 * (7,1,6,2,5,3,4 for red/green; 3,1,2 for blue) spreads the duty cycle evenly and is the
 * authority's; do not "simplify" it, the ordering is what makes intermediate levels look even. */
static void led_flash_pins(const struct od_led_pins *p, uint8_t color, uint8_t brightness)
{
    const uint8_t cr = (uint8_t)((color >> 5) & 0x07u);
    const uint8_t cg = (uint8_t)((color >> 2) & 0x07u);
    const uint8_t cb = (uint8_t)(color & 0x03u);
    const bool inv_r = (p->flags & 0x01u) != 0u;
    const bool inv_g = (p->flags & 0x02u) != 0u;
    const bool inv_b = (p->flags & 0x04u) != 0u;

    for (uint16_t i = 0; i < brightness; i++) {
        od_led_app_write(p->r, inv_r ? !(cr >= 7u) : (cr >= 7u));
        od_led_app_write(p->g, inv_g ? !(cg >= 7u) : (cg >= 7u));
        od_led_app_write(p->b, inv_b ? !(cb >= 3u) : (cb >= 3u));
        od_hal_delay_us(OD_LED_PWM_DELAY_US);
        od_led_app_write(p->r, inv_r ? !(cr >= 1u) : (cr >= 1u));
        od_led_app_write(p->g, inv_g ? !(cg >= 1u) : (cg >= 1u));
        od_hal_delay_us(OD_LED_PWM_DELAY_US);
        od_led_app_write(p->r, inv_r ? !(cr >= 6u) : (cr >= 6u));
        od_led_app_write(p->g, inv_g ? !(cg >= 6u) : (cg >= 6u));
        od_led_app_write(p->b, inv_b ? !(cb >= 1u) : (cb >= 1u));
        od_hal_delay_us(OD_LED_PWM_DELAY_US);
        od_led_app_write(p->r, inv_r ? !(cr >= 2u) : (cr >= 2u));
        od_led_app_write(p->g, inv_g ? !(cg >= 2u) : (cg >= 2u));
        od_hal_delay_us(OD_LED_PWM_DELAY_US);
        od_led_app_write(p->r, inv_r ? !(cr >= 5u) : (cr >= 5u));
        od_led_app_write(p->g, inv_g ? !(cg >= 5u) : (cg >= 5u));
        od_hal_delay_us(OD_LED_PWM_DELAY_US);
        od_led_app_write(p->r, inv_r ? !(cr >= 3u) : (cr >= 3u));
        od_led_app_write(p->g, inv_g ? !(cg >= 3u) : (cg >= 3u));
        od_led_app_write(p->b, inv_b ? !(cb >= 2u) : (cb >= 2u));
        od_hal_delay_us(OD_LED_PWM_DELAY_US);
        od_led_app_write(p->r, inv_r ? !(cr >= 4u) : (cr >= 4u));
        od_led_app_write(p->g, inv_g ? !(cg >= 4u) : (cg >= 4u));
        od_hal_delay_us(OD_LED_PWM_DELAY_US);
        od_led_app_write(p->r, inv_r);
        od_led_app_write(p->g, inv_g);
        od_led_app_write(p->b, inv_b);
    }
}

static void led_flash(uint8_t color)
{
    led_flash_pins(&s_run.pins, color, s_run.brightness);
}

void od_led_flash_once(const struct od_led_pins *pins, uint8_t color, uint8_t brightness)
{
    if (pins == NULL || brightness == 0u) {
        return;
    }
    led_flash_pins(pins, color, brightness);
}

/* Arm the next call. The delay is always positive -- see od_led.h. */
static uint32_t schedule(uint32_t now_ms, uint32_t delay_ms)
{
    s_run.waiting = true;
    s_run.deadline_ms = now_ms + delay_ms;
    return delay_ms;
}

static void run_finish(void)
{
    if (s_run.active) {
        led_all_off();
    }
    memset(&s_run, 0, sizeof s_run);
}

int od_led_activate(uint8_t instance, const struct od_led_pins *pins,
                    const uint8_t pattern[OD_LED_PATTERN_LEN], uint32_t now_ms)
{
    if (pins == NULL || pattern == NULL) {
        return 2;
    }
    if ((pattern[0] & 0x0Fu) != 1u) {
        return 2;                       /* not a run request */
    }

    /* Displacing a run parks the outgoing instance's LEDs. All three donors do this; the
     * authority (../Firmware handleLedActivate -> led_stop_internal(false)) deliberately does NOT
     * clear the outgoing mode nibble, and that is what is kept -- see DIVERGENCE_MATRIX 11.3. */
    if (s_run.active) {
        led_all_off();
    }
    memset(&s_run, 0, sizeof s_run);
    s_run.instance = instance;
    s_run.pins = *pins;

    s_run.brightness = (uint8_t)(((pattern[0] >> 4) & 0x0Fu) + 1u);
    s_run.c1 = pattern[1];
    s_run.c2 = pattern[4];
    s_run.c3 = pattern[7];
    s_run.loop1delay = (uint8_t)((pattern[2] >> 4) & 0x0Fu);
    s_run.loop2delay = (uint8_t)((pattern[5] >> 4) & 0x0Fu);
    s_run.loop3delay = (uint8_t)((pattern[8] >> 4) & 0x0Fu);
    s_run.loopcnt1 = (uint8_t)(pattern[2] & 0x0Fu);
    s_run.loopcnt2 = (uint8_t)(pattern[5] & 0x0Fu);
    s_run.loopcnt3 = (uint8_t)(pattern[8] & 0x0Fu);
    s_run.ildelay1 = pattern[3];
    s_run.ildelay2 = pattern[6];
    s_run.ildelay3 = pattern[9];
    /* The count is stored minus one and the host caps a finite request at 254, so raw 0xFE and
     * 0xFF both mean indefinite: py-opendisplay encodes 0xFE and decodes either. The canonical
     * header names only 0xFF. */
    s_run.repeat_forever = (pattern[10] >= 0xFEu);
    s_run.grouprepeats = (uint8_t)(pattern[10] + 1u);

    s_run.phase = OD_LED_PHASE_GROUP;
    s_run.active = true;
    s_run.waiting = false;
    s_run.deadline_ms = now_ms;
    return 0;
}

int od_led_stop(uint8_t instance, bool instance_given)
{
    uint8_t running;

    if (!s_run.active) {
        return 0;
    }
    if (instance_given && instance != s_run.instance) {
        return 2;
    }
    running = s_run.instance;
    run_finish();
    /* An explicit stop clears the mode nibble, as the donors do. The machine reports it because
     * only the machine knows which instance was running -- `instance` is unset when the client
     * sent no instance byte. */
    od_led_app_finished(running);
    return 0;
}

uint32_t od_led_service(uint32_t now_ms)
{
    uint8_t instance;

    if (!s_run.active) {
        return OD_LED_IDLE;
    }
    /* Live, not copied: an external clear stops the pattern. Nothing to clear on the way out. */
    if (od_led_app_mode(s_run.instance) != 1u) {
        run_finish();
        return OD_LED_IDLE;
    }
    if (s_run.waiting && !deadline_reached(now_ms, s_run.deadline_ms)) {
        return (uint32_t)(s_run.deadline_ms - now_ms);   /* early call: advance nothing */
    }
    s_run.waiting = false;
    instance = s_run.instance;

    for (;;) {
        switch (s_run.phase) {
        case OD_LED_PHASE_GROUP:
            if (!s_run.repeat_forever && s_run.group_pos >= s_run.grouprepeats) {
                run_finish();
                od_led_app_finished(instance);
                return OD_LED_IDLE;
            }
            s_run.i1 = 0u;
            s_run.i2 = 0u;
            s_run.i3 = 0u;
            s_run.phase = OD_LED_PHASE_LOOP1;
            break;

        case OD_LED_PHASE_LOOP1:
            if (s_run.i1 >= s_run.loopcnt1) {
                if (s_run.ildelay1 > 0u) {
                    s_run.phase = OD_LED_PHASE_INTER1_DELAY;
                    return schedule(now_ms, (uint32_t)s_run.ildelay1 * OD_LED_DELAY_FACTOR_MS);
                }
                s_run.phase = OD_LED_PHASE_LOOP2;
                break;
            }
            led_flash(s_run.c1);
            s_run.i1++;
            if (s_run.loop1delay > 0u) {
                s_run.phase = OD_LED_PHASE_LOOP1_DELAY;
                return schedule(now_ms, (uint32_t)s_run.loop1delay * OD_LED_DELAY_FACTOR_MS);
            }
            return schedule(now_ms, OD_LED_MIN_STEP_DELAY_MS);

        case OD_LED_PHASE_LOOP1_DELAY:
            s_run.phase = OD_LED_PHASE_LOOP1;
            break;

        case OD_LED_PHASE_INTER1_DELAY:
            s_run.phase = OD_LED_PHASE_LOOP2;
            break;

        case OD_LED_PHASE_LOOP2:
            if (s_run.i2 >= s_run.loopcnt2) {
                if (s_run.ildelay2 > 0u) {
                    s_run.phase = OD_LED_PHASE_INTER2_DELAY;
                    return schedule(now_ms, (uint32_t)s_run.ildelay2 * OD_LED_DELAY_FACTOR_MS);
                }
                s_run.phase = OD_LED_PHASE_LOOP3;
                break;
            }
            led_flash(s_run.c2);
            s_run.i2++;
            if (s_run.loop2delay > 0u) {
                s_run.phase = OD_LED_PHASE_LOOP2_DELAY;
                return schedule(now_ms, (uint32_t)s_run.loop2delay * OD_LED_DELAY_FACTOR_MS);
            }
            return schedule(now_ms, OD_LED_MIN_STEP_DELAY_MS);

        case OD_LED_PHASE_LOOP2_DELAY:
            s_run.phase = OD_LED_PHASE_LOOP2;
            break;

        case OD_LED_PHASE_INTER2_DELAY:
            s_run.phase = OD_LED_PHASE_LOOP3;
            break;

        case OD_LED_PHASE_LOOP3:
            if (s_run.i3 >= s_run.loopcnt3) {
                if (s_run.ildelay3 > 0u) {
                    s_run.phase = OD_LED_PHASE_INTER3_DELAY;
                    return schedule(now_ms, (uint32_t)s_run.ildelay3 * OD_LED_DELAY_FACTOR_MS);
                }
                /* Group-closing edge: also the only yield when every loop count is zero and no
                 * flash ever runs. */
                s_run.group_pos++;
                s_run.phase = OD_LED_PHASE_GROUP;
                return schedule(now_ms, OD_LED_MIN_STEP_DELAY_MS);
            }
            led_flash(s_run.c3);
            s_run.i3++;
            if (s_run.loop3delay > 0u) {
                s_run.phase = OD_LED_PHASE_LOOP3_DELAY;
                return schedule(now_ms, (uint32_t)s_run.loop3delay * OD_LED_DELAY_FACTOR_MS);
            }
            return schedule(now_ms, OD_LED_MIN_STEP_DELAY_MS);

        case OD_LED_PHASE_LOOP3_DELAY:
            s_run.phase = OD_LED_PHASE_LOOP3;
            break;

        case OD_LED_PHASE_INTER3_DELAY:
            s_run.group_pos++;
            s_run.phase = OD_LED_PHASE_GROUP;
            break;

        case OD_LED_PHASE_IDLE:
        default:
            run_finish();
            return OD_LED_IDLE;
        }
    }
}
