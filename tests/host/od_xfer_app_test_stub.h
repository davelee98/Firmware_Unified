#ifndef OD_XFER_APP_TEST_STUB_H
#define OD_XFER_APP_TEST_STUB_H

#include <stdbool.h>

void od_test_xfer_app_reset(void);
void od_test_xfer_app_set_panel_ready(bool ready);
unsigned od_test_xfer_app_abort_calls(void);

#endif /* OD_XFER_APP_TEST_STUB_H */
