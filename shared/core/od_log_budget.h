#ifndef OD_LOG_BUDGET_H
#define OD_LOG_BUDGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t last_ms;
    bool armed;
} od_log_budget_t;

static inline bool od_log_budget_allows(od_log_budget_t *budget, uint32_t now_ms,
                                        uint32_t interval_ms)
{
    if (budget == NULL
        || (budget->armed && (uint32_t)(now_ms - budget->last_ms) < interval_ms)) {
        return false;
    }
    budget->last_ms = now_ms;
    budget->armed = true;
    return true;
}

#endif /* OD_LOG_BUDGET_H */
