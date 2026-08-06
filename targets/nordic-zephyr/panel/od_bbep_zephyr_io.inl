//
// bb_epaper I/O for nRF54 + Zephyr (OpenDisplay) -- bit-bang SPI only.
//
// TARGET-OWNED, NOT VENDORED. Imported from Firmware_NRF54's
// third_party/bb_epaper/src/nrf54_zephyr_io.inl and moved here so that
// third_party/bb_epaper stays byte-identical to upstream -- the same rule
// targets/esp32-idf/panel/od_bbep_idf_io.inl follows, and the reason
// third_party/NOTICE.md's re-verify list does not grow when a target gains a backend.
//
// ONE EDIT ON THE WAY IN: delay(int) -> delay(long). The vendored bb_epaper.h declares
// `void delay(long)` (OD-PATCH), because delay(int) made every delay(uint32_t) call inside the
// library ambiguous. The source repo never hit this: its vendored header still declares
// delay(int). Keeping the old signature here would either fail to link or bind the wrong
// overload, and it would do so quietly.
//
#ifndef NRF54_ZEPHYR_IO_INL
#define NRF54_ZEPHYR_IO_INL

#include "nrf54_gpio.h"
#include "nrf54_zephyr_compat.h"
#include "od_log.h"

#include <string.h>

#undef INPUT
#undef OUTPUT
#undef INPUT_PULLUP
#define INPUT           0
#define OUTPUT          1
#define INPUT_PULLUP    2
#define INPUT_PULLDOWN  3
#define HIGH            1
#define LOW             0

#define pgm_read_byte(a)   (*(const uint8_t *)(a))
#define pgm_read_word(a)   (*(const uint16_t *)(a))
#define pgm_read_dword(a)  (*(const uint32_t *)(a))
#define memcpy_P           memcpy

void bbepWakeUp(BBEPDISP *pBBEP);
void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);

static void bb_spi_bitbang(BBEPDISP *pBBEP, const uint8_t *pData, int iLen)
{
	for (int i = 0; i < iLen; i++) {
		uint8_t uc = pData[i];
		for (int j = 0; j < 8; j++) {
			nrf54_gpio_write(pBBEP->iCLKPin, false);
			nrf54_gpio_write(pBBEP->iMOSIPin, (uc & 0x80u) != 0u);
			nrf54_gpio_write(pBBEP->iCLKPin, true);
			uc <<= 1;
		}
	}
	nrf54_gpio_write(pBBEP->iCLKPin, false);
}

static void bb_spi_write(BBEPDISP *pBBEP, const uint8_t *pData, int iLen)
{
	bb_spi_bitbang(pBBEP, pData, iLen);
}

static void bb_spi_init(uint8_t mosi, uint8_t sck, uint32_t speed)
{
	uint8_t port;
	uint8_t pin;

	(void)speed;
	if (nrf54_pin_decode(mosi, &port, &pin)) {
		nrf54_gpio_configure_output(mosi, false);
	}
	if (nrf54_pin_decode(sck, &port, &pin)) {
		nrf54_gpio_configure_output(sck, false);
	}
}

void digitalWrite(int iPin, int iState)
{
	nrf54_gpio_write((uint8_t)iPin, iState != 0);
}

void pinMode(int iPin, int iMode)
{
	if (iMode == INPUT) {
		nrf54_gpio_configure_input((uint8_t)iPin, false, false);
	} else if (iMode == INPUT_PULLUP) {
		nrf54_gpio_configure_input((uint8_t)iPin, true, false);
	} else if (iMode == INPUT_PULLDOWN) {
		nrf54_gpio_configure_input((uint8_t)iPin, false, true);
	} else {
		nrf54_gpio_configure_output((uint8_t)iPin, false);
	}
}

int digitalRead(int iPin)
{
	return nrf54_gpio_read((uint8_t)iPin);
}

void delay(long ms)
{
	if (ms > 0) {
		od_msleep(ms);
	}
}

void delayMicroseconds(long us)
{
	if (us <= 0) {
		return;
	}
	if (us > 100000L) {
		od_msleep((int32_t)(us / 1000L));
		return;
	}
	od_busy_wait((uint32_t)us);
}

long millis(void)
{
	return (long)od_uptime_get_32();
}

/* ------------------------------------------------------- cs_mode (bb_epaper 5dccfbb) ---
 *
 * bb_epaper gained a `cs_mode` field and DROPPED `iCS1Pin`. Dual-controller panels used to be
 * driven by mutating iCSPin between iCS1Pin and iCS2Pin; now iCSPin IS CS1 and cs_mode
 * (CMD_CS1 / CMD_CS2 / CMD_CS1_CS2) selects which line(s) a transfer asserts. Split-controller
 * panels need CMD_CS1_CS2 to assert BOTH.
 *
 * arduino_io.inl repeats this test at every CS site; this backend routes them through one
 * pair of helpers so there is a single place to get it wrong. cs_mode == 0 is treated as
 * CMD_CS1: a memset-zeroed BBEPDISP (which is how this target creates its) would otherwise
 * assert nothing and the panel would sit silent. */
static inline void od_bbep_cs(BBEPDISP *pBBEP, int level)
{
	const uint8_t mode = pBBEP->cs_mode ? pBBEP->cs_mode : (uint8_t)CMD_CS1;
	if (mode == CMD_CS1 || mode == CMD_CS1_CS2) {
		digitalWrite(pBBEP->iCSPin, level);
	}
	if (mode == CMD_CS2 || mode == CMD_CS1_CS2) {
		digitalWrite(pBBEP->iCS2Pin, level);
	}
}

void bbepSetCS2(BBEPDISP *pBBEP, uint8_t cs)
{
	/* No iCS1Pin: the field was REMOVED at 5dccfbb because iCSPin is CS1. */
	pBBEP->iCS2Pin = cs;
	pinMode(cs, OUTPUT);
	digitalWrite(cs, HIGH);
}

void bbepInitIO(BBEPDISP *pBBEP, uint8_t u8DC, uint8_t u8RST, uint8_t u8BUSY, uint8_t u8CS,
		uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed)
{
	pBBEP->iDCPin = u8DC;
	pBBEP->iCSPin = u8CS;
	pBBEP->iMOSIPin = u8MOSI;
	pBBEP->iCLKPin = u8SCK;
	pBBEP->iRSTPin = u8RST;
	pBBEP->iBUSYPin = u8BUSY;
	pBBEP->iSpeed = (int)u32Speed;

	/*
	 * PIN RESOLUTION IS THE FIRST THING TO CHECK when the panel is silent, because a pin
	 * that fails to decode does not fault -- nrf54_gpio_write() simply returns and the
	 * bit-bang writes go nowhere. That failure mode already cost one bring-up session
	 * (config bytes decoded to a port the chip does not have), and it is invisible without
	 * this dump. "ok=0" on any row means every transfer on that line is a no-op.
	 */
	{
		static const char *const names[6] = { "DC", "RST", "BUSY", "CS", "MOSI", "SCK" };
		const uint8_t cfgs[6] = { u8DC, u8RST, u8BUSY, u8CS, u8MOSI, u8SCK };

		od_dbg("[OD] panel pins (speed=%u Hz):\r\n", (unsigned)u32Speed);
		for (unsigned i = 0; i < 6u; i++) {
			uint8_t port = 0;
			uint8_t pin = 0;
			bool ok = nrf54_pin_decode(cfgs[i], &port, &pin);

			od_dbg("[OD]   %-4s cfg=%3u (0x%02X) -> P%u.%02u ok=%d\r\n", names[i],
			       (unsigned)cfgs[i], (unsigned)cfgs[i], (unsigned)port,
			       (unsigned)pin, (int)ok);
		}
	}

	pinMode(pBBEP->iDCPin, OUTPUT);
	if (pBBEP->iRSTPin != 0xff) {
		pinMode(pBBEP->iRSTPin, OUTPUT);
		digitalWrite(pBBEP->iRSTPin, HIGH);
	}
	if (pBBEP->iBUSYPin != 0xff) {
		pinMode(pBBEP->iBUSYPin,
			(pBBEP->chip_type == BBEP_CHIP_UC81xx) ? INPUT_PULLUP : INPUT_PULLDOWN);
	}
	pinMode(pBBEP->iCSPin, OUTPUT);
	digitalWrite(pBBEP->iCSPin, HIGH);
	bb_spi_init(u8MOSI, u8SCK, u32Speed);
}

void bbepWriteIT8951Cmd(BBEPDISP *pBBEP, uint16_t cmd)
{
	uint8_t ucTemp[4] = { 0x60, 0, (uint8_t)(cmd >> 8), (uint8_t)cmd };

	digitalWrite(pBBEP->iCSPin, LOW);
	bb_spi_write(pBBEP, ucTemp, 4);
	digitalWrite(pBBEP->iCSPin, HIGH);
}

void bbepWriteIT8951Data(BBEPDISP *pBBEP, uint8_t *pData, int iLen)
{
	uint8_t z[2] = { 0, 0 };

	digitalWrite(pBBEP->iCSPin, LOW);
	bb_spi_write(pBBEP, z, 2);
	bb_spi_write(pBBEP, pData, iLen);
	digitalWrite(pBBEP->iCSPin, HIGH);
}

void bbepWriteIT8951CmdArgs(BBEPDISP *pBBEP, uint16_t cmd, uint16_t *pArgs, int iCount)
{
	bbepWriteIT8951Cmd(pBBEP, cmd);
	for (int i = 0; i < iCount; i++) {
		pArgs[i] = __builtin_bswap16(pArgs[i]);
	}
	bbepWriteIT8951Data(pBBEP, (uint8_t *)pArgs, iCount * 2);
}

void bbepWriteCmd(BBEPDISP *pBBEP, uint8_t cmd)
{
	if (!pBBEP->is_awake) {
		bbepWakeUp(pBBEP);
		pBBEP->is_awake = 1;
	}
	digitalWrite(pBBEP->iDCPin, LOW);
	delay(1);
	od_bbep_cs(pBBEP, LOW);
	bb_spi_write(pBBEP, &cmd, 1);
	od_bbep_cs(pBBEP, HIGH);
	digitalWrite(pBBEP->iDCPin, HIGH);
}

void bbepWriteData(BBEPDISP *pBBEP, uint8_t *pData, int iLen)
{
	if (pBBEP->iFlags & BBEP_CS_EVERY_BYTE) {
		for (int i = 0; i < iLen; i++) {
			od_bbep_cs(pBBEP, LOW);
			bb_spi_write(pBBEP, &pData[i], 1);
			od_bbep_cs(pBBEP, HIGH);
		}
	} else {
		od_bbep_cs(pBBEP, LOW);
		bb_spi_write(pBBEP, pData, iLen);
		od_bbep_cs(pBBEP, HIGH);
	}
}

/* NEW IN THE CONTRACT at 5dccfbb: bb_ep.inl calls this directly, so a backend without it no
 * longer links. Modelled on arduino_io.inl -- command byte with DC low, payload with DC high,
 * all inside ONE CS assertion. Splitting it into bbepWriteCmd() + bbepWriteData() would toggle
 * CS in between, and controllers that latch on the CS edge would see two transactions where the
 * library intends one. */
void bbepWriteCmdData(BBEPDISP *pBBEP, uint8_t cmd, uint8_t *pData, int iLen)
{
	if (!pBBEP->is_awake) {
		bbepWakeUp(pBBEP);
		pBBEP->is_awake = 1;
	}
	digitalWrite(pBBEP->iDCPin, LOW);
	delay(1);
	od_bbep_cs(pBBEP, LOW);
	bb_spi_write(pBBEP, &cmd, 1);
	digitalWrite(pBBEP->iDCPin, HIGH);
	delay(1);
	if (pData != NULL && iLen > 0) {
		bb_spi_write(pBBEP, pData, iLen);
	}
	od_bbep_cs(pBBEP, HIGH);
}

void bbepCMD2(BBEPDISP *pBBEP, uint8_t cmd1, uint8_t cmd2)
{
	bbepWriteCmd(pBBEP, cmd1);
	bbepWriteData(pBBEP, &cmd2, 1);
}

#endif /* NRF54_ZEPHYR_IO_INL */
