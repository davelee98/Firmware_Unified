#ifndef OD_TEST_FAKE_SILABS_STATUS_H
#define OD_TEST_FAKE_SILABS_STATUS_H

#include <stdint.h>

typedef uint32_t sl_status_t;

#define SL_STATUS_OK                ((sl_status_t)0u)
#define SL_STATUS_NO_MORE_RESOURCE  ((sl_status_t)1u)
#define SL_STATUS_FAIL              ((sl_status_t)2u)
#define SL_STATUS_NOT_FOUND         ((sl_status_t)3u)
#define SL_STATUS_INVALID_PARAMETER ((sl_status_t)4u)
#define SL_STATUS_COMMAND_TOO_LONG  ((sl_status_t)5u)

#endif /* OD_TEST_FAKE_SILABS_STATUS_H */
