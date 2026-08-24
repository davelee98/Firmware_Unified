#include "od_buzzer.h"

#include "od_buzzer_app.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include <string.h>

/* Quarter-tone frequency table in centi-Hz. This is the live Firmware authority's integer table:
 * round(100 * 13.75 * 2^(idx/24)); entry zero is the rest sentinel. */
static const uint32_t s_centihz[256] = {
          0u,    1415u,    1457u,    1499u,    1543u,    1589u,    1635u,    1683u,
       1732u,    1783u,    1835u,    1889u,    1945u,    2002u,    2060u,    2121u,
       2183u,    2247u,    2312u,    2380u,    2450u,    2522u,    2596u,    2672u,
       2750u,    2831u,    2914u,    2999u,    3087u,    3177u,    3270u,    3366u,
       3465u,    3566u,    3671u,    3778u,    3889u,    4003u,    4120u,    4241u,
       4365u,    4493u,    4625u,    4760u,    4900u,    5044u,    5191u,    5343u,
       5500u,    5661u,    5827u,    5998u,    6174u,    6354u,    6541u,    6732u,
       6930u,    7133u,    7342u,    7557u,    7778u,    8006u,    8241u,    8482u,
       8731u,    8987u,    9250u,    9521u,    9800u,   10087u,   10383u,   10687u,
      11000u,   11322u,   11654u,   11996u,   12347u,   12709u,   13081u,   13465u,
      13859u,   14265u,   14683u,   15113u,   15556u,   16012u,   16481u,   16964u,
      17461u,   17973u,   18500u,   19042u,   19600u,   20174u,   20765u,   21374u,
      22000u,   22645u,   23308u,   23991u,   24694u,   25418u,   26163u,   26929u,
      27718u,   28530u,   29366u,   30227u,   31113u,   32024u,   32963u,   33929u,
      34923u,   35946u,   36999u,   38084u,   39200u,   40348u,   41530u,   42747u,
      44000u,   45289u,   46616u,   47982u,   49388u,   50836u,   52325u,   53858u,
      55437u,   57061u,   58733u,   60454u,   62225u,   64049u,   65926u,   67857u,
      69846u,   71892u,   73999u,   76167u,   78399u,   80696u,   83061u,   85495u,
      88000u,   90579u,   93233u,   95965u,   98777u,  101671u,  104650u,  107717u,
     110873u,  114122u,  117466u,  120908u,  124451u,  128097u,  131851u,  135715u,
     139691u,  143785u,  147998u,  152334u,  156798u,  161393u,  166122u,  170990u,
     176000u,  181157u,  186466u,  191929u,  197553u,  203342u,  209300u,  215433u,
     221746u,  228244u,  234932u,  241816u,  248902u,  256195u,  263702u,  271429u,
     279383u,  287569u,  295996u,  304669u,  313596u,  322785u,  332244u,  341979u,
     352000u,  362314u,  372931u,  383859u,  395107u,  406684u,  418601u,  430867u,
     443492u,  456488u,  469864u,  483632u,  497803u,  512390u,  527404u,  542858u,
     558765u,  575138u,  591991u,  609338u,  627193u,  645571u,  664488u,  683958u,
     704000u,  724629u,  745862u,  767717u,  790213u,  813368u,  837202u,  861734u,
     886984u,  912975u,  939727u,  967263u,  995606u, 1024780u, 1054808u, 1085716u,
    1117530u, 1150276u, 1183982u, 1218675u, 1254385u, 1291142u, 1328975u, 1367917u,
    1408000u, 1449258u, 1491724u, 1535435u, 1580427u, 1626737u, 1674404u, 1723467u,
    1773969u, 1825950u, 1879455u, 1934527u, 1991213u, 2049560u, 2109616u, 2171433u
};

#define OD_BUZZER_MIN_INDEX 117u
#define OD_BUZZER_MAX_INDEX 234u

typedef enum {
    OD_BUZZER_PHASE_STEP = 0,
    OD_BUZZER_PHASE_GAP
} od_buzzer_phase_t;

static struct {
    bool active;
    bool waiting;
    bool tone_on;
    struct od_buzzer_config config;
    uint8_t payload[OD_BUZZER_PAYLOAD_MAX];
    uint16_t payload_len;
    uint8_t outer;
    uint8_t repeat;
    uint8_t pattern_count;
    uint8_t pattern;
    uint16_t offset;
    uint8_t step_count;
    uint8_t step;
    od_buzzer_phase_t phase;
    uint32_t started_ms;
    uint32_t deadline_ms;
} s_run;

static bool deadline_reached(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000u;
}

static uint8_t fold_index(uint8_t index)
{
    uint16_t folded = index;

    if (index == 0u) {
        return 0u;
    }
    while (folded < OD_BUZZER_MIN_INDEX) {
        folded += 24u;
    }
    while (folded > OD_BUZZER_MAX_INDEX) {
        folded -= 24u;
    }
    return (uint8_t)folded;
}

uint32_t od_buzzer_index_centihz(uint8_t index)
{
    return s_centihz[fold_index(index)];
}

static void set_enable(bool on)
{
    bool high;

    if (s_run.config.enable_pin == OD_PIN_UNUSED) {
        return;
    }
    high = on;
    if ((s_run.config.flags & OD_BUZZER_FLAG_ENABLE_ACTIVE_HIGH) == 0u) {
        high = !high;
    }
    od_buzzer_app_enable_write(s_run.config.enable_pin, high);
}

static void stop_tone(void)
{
    od_buzzer_app_tone_stop(s_run.config.drive_pin);
    s_run.tone_on = false;
}

static void finish(void)
{
    if (s_run.active) {
        stop_tone();
        set_enable(false);
    }
    memset(&s_run, 0, sizeof s_run);
}

void od_buzzer_stop(void)
{
    finish();
}

int od_buzzer_activate(const struct od_buzzer_config *config,
                       const uint8_t *payload, uint16_t payload_len, uint32_t now_ms)
{
    uint16_t scan;
    uint8_t pattern;

    if (config == NULL || payload == NULL || payload_len < 3u) {
        return 1;
    }
    if (payload[2] == 0u) {
        return 4;
    }
    scan = 3u;
    for (pattern = 0u; pattern < payload[2]; pattern++) {
        uint32_t needed;

        if (scan >= payload_len) {
            return 5;
        }
        needed = (uint32_t)payload[scan++] * 2u;
        if ((uint32_t)scan + needed > payload_len) {
            return 5;
        }
        scan = (uint16_t)((uint32_t)scan + needed);
    }
    if (scan != payload_len) {
        return 6;
    }
    if (payload_len > OD_BUZZER_PAYLOAD_MAX) {
        return 5;
    }

    finish();
    s_run.config = *config;
    memcpy(s_run.payload, payload, payload_len);
    s_run.payload_len = payload_len;
    s_run.outer = payload[1] == 0u ? 1u : payload[1];
    s_run.pattern_count = payload[2];
    s_run.offset = 3u;
    s_run.step_count = s_run.payload[s_run.offset++];
    s_run.phase = OD_BUZZER_PHASE_STEP;
    s_run.started_ms = now_ms;
    s_run.deadline_ms = now_ms;
    s_run.active = true;
    return 0;
}

static uint32_t schedule(uint32_t now_ms, uint32_t delay_ms)
{
    s_run.waiting = true;
    s_run.deadline_ms = now_ms + delay_ms;
    return delay_ms;
}

uint32_t od_buzzer_service(uint32_t now_ms)
{
    if (!s_run.active) {
        return OD_BUZZER_IDLE;
    }
    if (s_run.waiting && !deadline_reached(now_ms, s_run.deadline_ms)) {
        return (uint32_t)(s_run.deadline_ms - now_ms);
    }
    if (s_run.waiting) {
        if (s_run.tone_on) {
            stop_tone();
        }
        s_run.waiting = false;
    }

    for (;;) {
        uint32_t elapsed = (uint32_t)(now_ms - s_run.started_ms);

        if (elapsed >= OD_BUZZER_MAX_TOTAL_MS) {
            finish();
            return OD_BUZZER_IDLE;
        }
        if (s_run.phase == OD_BUZZER_PHASE_GAP) {
            s_run.pattern++;
            s_run.step_count = s_run.payload[s_run.offset++];
            s_run.step = 0u;
            s_run.phase = OD_BUZZER_PHASE_STEP;
            continue;
        }
        if (s_run.step < s_run.step_count) {
            const uint8_t index = s_run.payload[s_run.offset++];
            const uint8_t units = s_run.payload[s_run.offset++];
            uint32_t duration_ms = (uint32_t)units * OD_BUZZER_DURATION_UNIT_MS;
            const uint32_t remaining_ms = OD_BUZZER_MAX_TOTAL_MS - elapsed;
            uint32_t centihz;

            s_run.step++;
            if (duration_ms > remaining_ms) {
                duration_ms = remaining_ms;
            }
            if (duration_ms == 0u) {
                continue;
            }
            set_enable(true);
            centihz = od_buzzer_index_centihz(index);
            if (centihz != 0u) {
                s_run.tone_on = od_buzzer_app_tone_start(s_run.config.drive_pin, centihz,
                                                         s_run.config.duty_percent);
            } else {
                stop_tone();
            }
            return schedule(now_ms, duration_ms);
        }

        if ((uint8_t)(s_run.pattern + 1u) < s_run.pattern_count) {
            s_run.phase = OD_BUZZER_PHASE_GAP;
            return schedule(now_ms, OD_BUZZER_INTER_PATTERN_MS);
        }
        s_run.repeat++;
        if (s_run.repeat >= s_run.outer) {
            finish();
            return OD_BUZZER_IDLE;
        }
        s_run.pattern = 0u;
        s_run.offset = 3u;
        s_run.step_count = s_run.payload[s_run.offset++];
        s_run.step = 0u;
    }
}
