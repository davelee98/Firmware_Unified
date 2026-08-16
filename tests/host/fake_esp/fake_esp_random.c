#include "esp_random.h"

#include <string.h>

void esp_fill_random(void *buf, size_t len)
{
    if (buf != NULL && len != 0u) {
        memset(buf, 0x5A, len);
    }
}
