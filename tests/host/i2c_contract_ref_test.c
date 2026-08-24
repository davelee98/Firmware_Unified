/* The od_hal_i2c contract, bound to the reference engine (staging step 3).
 *
 * Steps 4, 5 and 9 add executables that bind ESP32's, Nordic's and BG22's production adapters to
 * the same tests/host/i2c_contract.inc body.
 */

#include "od_hal_i2c.h"
#include "od_config.h"
#include "fake_i2c/i2c_wire.h"

#include "od_check.h"

extern const struct od_config *i2c_ref_cfg;

static void i2c_contract_install(struct od_config *cfg) { i2c_ref_cfg = cfg; }

#include "i2c_contract.inc"

int main(void)
{
    od_i2c_contract_run();
    return OD_CHECK_REPORT_NONEMPTY("i2c_contract_ref", 50u);
}
