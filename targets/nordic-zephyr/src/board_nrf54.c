#include "board_nrf54.h"
#include "nrf54_gpio.h"

#include <zephyr/kernel.h>

#if defined(NRF54_BOARD_LM20)

/*
 * P1.12 — Seeed power_en (regulator-fixed in DTS). Do not use CONFIG_REGULATOR
 * here: enabling it breaks nordic,nrf-usbhs-wrapper on this board.
 * P1.13 is PDM mic CLK. SHPHLD is a separate board pad (not this GPIO).
 */
#define NRF54LM20_POWER_EN 0x1Cu
/* Mic pads only — RGB comes from OpenDisplay LED TLV (nrf54lm20-xiao). */
#define NRF54LM20_MIC_CLK  0x1Du /* P1.13 */
#define NRF54LM20_MIC_DIN  0x1Eu /* P1.14 */
/* uart20 console pins (MCUboot default); park so USB-UART is not back-fed. */
#define NRF54LM20_UART20_RX 0x1Au /* P1.10 */
#define NRF54LM20_UART20_TX 0x1Bu /* P1.11 */

void board_nrf54_early_init(void)
{
	nrf54_gpio_configure_output(NRF54LM20_POWER_EN, true);
	/* Disconnect unused Sense mic pads so they do not float. */
	nrf54_gpio_park(NRF54LM20_MIC_CLK);
	nrf54_gpio_park(NRF54LM20_MIC_DIN);
	/* MCUboot may have left UART TX driven; app has SERIAL=n and never
	 * reclaims the pins unless we park them here. */
	nrf54_gpio_park(NRF54LM20_UART20_RX);
	nrf54_gpio_park(NRF54LM20_UART20_TX);
	/* nPM1300 needs the rail up before bit-bang I2C; 20ms was marginal. */
	k_msleep(50);
}

void board_nrf54_prepare_epd_rail(void)
{
	k_msleep(50);
}

#elif defined(OD_BOARD_XIAO_NRF52840)

/*
 * XIAO nRF52840 has NO board-level init to do here, and that is a finding rather than a
 * shrug. Before this arm existed the board compiled as NRF54_BOARD_L15 and ran the L15
 * init, driving P2.3 / P2.5 / P2.10 -- nRF54L15 RF-switch and boost-select pins on ports
 * this chip does not have (nRF52840 is P0/P1 only). Nothing faulted, because
 * nrf54_pin_decode() rejects a port whose gpio node is not okay, so all three writes were
 * no-ops. The board was getting the wrong init and no complaint about it.
 *
 * The real board needs nothing: its antenna is hard-wired (no RF switch to steer) and it
 * has no EPD boost rail to sequence. VBAT sense enable (P0.14) belongs to the battery
 * driver, not to early init.
 */
void board_nrf54_early_init(void)
{
}

void board_nrf54_prepare_epd_rail(void)
{
	/* No board rail to bring up; the delay matches the other boards so panel timing
	 * does not silently differ by 50 ms across boards. */
	k_msleep(50);
}

#else

#define NRF54L15_RFSW_PWR  0x23u /* P2.3 — RF switch power (keep high) */
#define NRF54L15_RFSW_SEL  0x25u /* P2.5 — RF switch select: low=ceramic, high=external */
#define NRF54L15_BS_PIN    0x2Au

void board_nrf54_early_init(void)
{
	/* Seeed wiki: rfsw-ctl low selects the onboard ceramic antenna. */
	nrf54_gpio_configure_output(NRF54L15_RFSW_PWR, true);
	nrf54_gpio_configure_output(NRF54L15_RFSW_SEL, false);
	nrf54_gpio_configure_output(NRF54L15_BS_PIN, false);
	k_msleep(10);
}

void board_nrf54_prepare_epd_rail(void)
{
	nrf54_gpio_write(NRF54L15_BS_PIN, false);
	k_msleep(50);
}

#endif

/* Bit-bang the 0xB9 deep power-down command to an external SPI NOR flash,
 * then park the bus — mirrors Firmware powerDownExternalFlash():
 * MOSI/MISO/SCK low, CS/WP/HOLD high. Optional pins (0 / 0xFF) are skipped. */
void board_nrf54_flash_powerdown(uint8_t mosi_cfg, uint8_t sck_cfg, uint8_t cs_cfg,
				 uint8_t miso_cfg, uint8_t wp_cfg, uint8_t hold_cfg)
{
	uint8_t cmd = 0xB9u;

	nrf54_gpio_configure_output(mosi_cfg, false);
	nrf54_gpio_configure_output(sck_cfg, false);
	nrf54_gpio_configure_output(cs_cfg, false);
	if (miso_cfg != 0u && miso_cfg != 0xFFu) {
		nrf54_gpio_configure_output(miso_cfg, false);
	}
	if (wp_cfg != 0u && wp_cfg != 0xFFu) {
		nrf54_gpio_configure_output(wp_cfg, true);
	}
	if (hold_cfg != 0u && hold_cfg != 0xFFu) {
		nrf54_gpio_configure_output(hold_cfg, true);
	}

	nrf54_gpio_write(cs_cfg, false);
	for (uint8_t bit = 0; bit < 8u; bit++) {
		nrf54_gpio_write(mosi_cfg, (cmd & 0x80u) != 0u);
		cmd = (uint8_t)(cmd << 1);
		k_busy_wait(1);
		nrf54_gpio_write(sck_cfg, true);
		k_busy_wait(1);
		nrf54_gpio_write(sck_cfg, false);
	}
	nrf54_gpio_write(cs_cfg, true);
	k_busy_wait(30);

	nrf54_gpio_write(mosi_cfg, false);
	nrf54_gpio_write(sck_cfg, false);
	nrf54_gpio_write(cs_cfg, true);
	if (miso_cfg != 0u && miso_cfg != 0xFFu) {
		nrf54_gpio_write(miso_cfg, false);
	}
	if (wp_cfg != 0u && wp_cfg != 0xFFu) {
		nrf54_gpio_write(wp_cfg, true);
	}
	if (hold_cfg != 0u && hold_cfg != 0xFFu) {
		nrf54_gpio_write(hold_cfg, true);
	}
}
