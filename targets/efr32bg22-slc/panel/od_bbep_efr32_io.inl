//
// bb_epaper I/O for EFR32BG22 (GPIO bit-bang SPI, packed port/pin like OpenDisplay LEDs).
//
// TARGET-OWNED, NOT VENDORED. Imported from Firmware_NRF54's
// third_party/bb_epaper/src/silabs_efr32_io.inl and moved here so third_party/bb_epaper stays
// byte-identical to upstream -- the same rule targets/esp32-idf/panel/od_bbep_idf_io.inl and
// targets/nordic-zephyr/panel/od_bbep_zephyr_io.inl follow. This is the third target on that
// pattern; it is deliberately not a fourth variation of it.
//
// THREE EDITS ON THE WAY IN, all forced by bb_epaper 5dccfbb -- the revision this repo vendors,
// which is NEWER than the one the source backend was written against:
//
//   1. delay(int) -> delay(long). The vendored bb_epaper.h declares `void delay(long)`
//      (OD-PATCH). Keeping the old signature binds the wrong overload or fails to link, and it
//      does so quietly.
//   2. cs_mode gating. iCS1Pin was REMOVED from BBEPDISP: iCSPin IS CS1 now, and dual-controller
//      panels are addressed by cs_mode (CMD_CS1 / CMD_CS2 / CMD_CS1_CS2) rather than by mutating
//      iCSPin around each write.
//   3. bbepWriteCmdData(). New in the contract -- bb_ep.inl calls it directly, so a backend
//      without it no longer links.
//
#ifndef __SILABS_EFR32_IO__
#define __SILABS_EFR32_IO__

#include "em_gpio.h"
#include "em_cmu.h"
#include "sl_sleeptimer.h"
#include "sl_udelay.h"

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
static bool silabs_bb_pin_decode(int iPin, GPIO_Port_TypeDef *port_out, uint8_t *pin_out);

static bool s_hw_spi_enabled = false;

static void silabs_hw_spi_init(uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed)
{
  GPIO_Port_TypeDef mosi_port;
  GPIO_Port_TypeDef sck_port;
  uint8_t mosi_pin;
  uint8_t sck_pin;

  s_hw_spi_enabled = false;
  if (u32Speed == 0u) {
    return;
  }
  if (!silabs_bb_pin_decode((int)u8MOSI, &mosi_port, &mosi_pin) ||
      !silabs_bb_pin_decode((int)u8SCK, &sck_port, &sck_pin)) {
    return;
  }

  CMU_ClockEnable(cmuClock_GPIO, true);
  CMU_ClockEnable(cmuClock_USART1, true);

  GPIO_PinModeSet(mosi_port, mosi_pin, gpioModePushPull, 0);
  GPIO_PinModeSet(sck_port, sck_pin, gpioModePushPull, 0);

  USART1->EN = 0;
  USART1->CMD = USART_CMD_RXDIS | USART_CMD_TXDIS | USART_CMD_MASTERDIS;
  USART1->CTRL = USART_CTRL_SYNC | USART_CTRL_MSBF;
  USART1->FRAME = USART_FRAME_DATABITS_EIGHT;

  {
    uint32_t usart_clk = CMU_ClockFreqGet(cmuClock_USART1);
    if (u32Speed > 0u && usart_clk > (2u * u32Speed)) {
      uint32_t clkdiv = (uint32_t)(((256ULL * (uint64_t)usart_clk) / (2ULL * (uint64_t)u32Speed)) - 256ULL);
      USART1->CLKDIV = (clkdiv << _USART_CLKDIV_DIV_SHIFT) & _USART_CLKDIV_DIV_MASK;
    } else {
      USART1->CLKDIV = 0u;
    }
  }

  GPIO->USARTROUTE[1].TXROUTE = ((uint32_t)mosi_port << _GPIO_USART_TXROUTE_PORT_SHIFT)
                              | ((uint32_t)mosi_pin << _GPIO_USART_TXROUTE_PIN_SHIFT);
  GPIO->USARTROUTE[1].CLKROUTE = ((uint32_t)sck_port << _GPIO_USART_CLKROUTE_PORT_SHIFT)
                               | ((uint32_t)sck_pin << _GPIO_USART_CLKROUTE_PIN_SHIFT);
  GPIO->USARTROUTE[1].ROUTEEN = GPIO_USART_ROUTEEN_TXPEN | GPIO_USART_ROUTEEN_CLKPEN;
  USART1->EN = USART_EN_EN;
  USART1->CMD = USART_CMD_MASTEREN | USART_CMD_TXEN | USART_CMD_RXDIS;

  s_hw_spi_enabled = true;
}

static bool silabs_bb_pin_decode(int iPin, GPIO_Port_TypeDef *port_out, uint8_t *pin_out)
{
  uint8_t v = (uint8_t)iPin;
  if (v == 0xFFu) {
    return false;
  }
  unsigned pr = (unsigned)(v >> 4) & 0x0Fu;
  unsigned pn = (unsigned)(v & 0x0Fu);
  if (pr > (unsigned)GPIO_PORT_MAX || pn > 15u) {
    return false;
  }
  *port_out = (GPIO_Port_TypeDef)(gpioPortA + pr);
  *pin_out = (uint8_t)pn;
  return true;
}

void digitalWrite(int iPin, int iState)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (!silabs_bb_pin_decode(iPin, &port, &pin)) {
    return;
  }
  if (iState) {
    GPIO_PinOutSet(port, pin);
  } else {
    GPIO_PinOutClear(port, pin);
  }
}

void pinMode(int iPin, int iMode)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (!silabs_bb_pin_decode(iPin, &port, &pin)) {
    return;
  }
  if (iMode == INPUT) {
    GPIO_PinModeSet(port, pin, gpioModeInput, 0);
  } else if (iMode == INPUT_PULLUP) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 1);
  } else if (iMode == INPUT_PULLDOWN) {
    GPIO_PinModeSet(port, pin, gpioModeInputPull, 0);
  } else {
    GPIO_PinModeSet(port, pin, gpioModePushPull, 0);
  }
}

int digitalRead(int iPin)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (!silabs_bb_pin_decode(iPin, &port, &pin)) {
    return 0;
  }
  return (int)GPIO_PinInGet(port, pin);
}

void delay(long ms)
{
  if (ms <= 0) {
    return;
  }
  sl_sleeptimer_delay_millisecond((uint16_t)ms);
}

void delayMicroseconds(long l)
{
  if (l <= 0) {
    return;
  }
  if (l > 100000L) {
    sl_sleeptimer_delay_millisecond((uint16_t)(l / 1000L));
    return;
  }
  sl_udelay_wait((unsigned)l);
}

long millis(void)
{
  return (long)sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
}

/* cs_mode gating, once. arduino_io.inl repeats this test at every CS site; routing them through
 * one helper leaves a single place to get it wrong.
 *
 * cs_mode == 0 is treated as CMD_CS1: a memset-zeroed BBEPDISP -- which is how this target
 * creates its -- would otherwise assert no CS line at all and the panel would sit silent. */
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

static void SPI_Write(BBEPDISP *pBBEP, uint8_t *pData, int iLen)
{
  int i, j;
  uint8_t uc;

  if (s_hw_spi_enabled) {
    uint32_t guard = 200000u;
    while ((USART1->STATUS & USART_STATUS_TXBL) == 0u && guard-- > 0u) {
    }
    if (guard == 0u) {
      s_hw_spi_enabled = false; // fail-safe fallback to bit-bang
    }
  }
  if (s_hw_spi_enabled) {
    for (i = 0; i < iLen; i++) {
      uint32_t guard = 200000u;
      while ((USART1->STATUS & USART_STATUS_TXBL) == 0u && guard-- > 0u) {
      }
      if (guard == 0u) {
        s_hw_spi_enabled = false;
        break;
      }
      USART1->TXDATA = (uint32_t)(*pData++);
    }
    if (s_hw_spi_enabled) {
      uint32_t guard = 200000u;
      while ((USART1->STATUS & USART_STATUS_TXC) == 0u && guard-- > 0u) {
      }
      if (guard == 0u) {
        s_hw_spi_enabled = false;
      }
    }
    if (s_hw_spi_enabled) {
      return;
    }
  }

  for (i = 0; i < iLen; i++) {
    uc = *pData++;
    for (j = 0; j < 8; j++) {
      digitalWrite(pBBEP->iCLKPin, 0);
      digitalWrite(pBBEP->iMOSIPin, uc & 0x80);
      digitalWrite(pBBEP->iCLKPin, 1);
      uc <<= 1;
    }
  }
  digitalWrite(pBBEP->iCLKPin, 0); // return CLK to idle (LOW) for SPI Mode 0
}

void bbepInitIO(BBEPDISP *pBBEP, uint8_t u8DC, uint8_t u8RST, uint8_t u8BUSY, uint8_t u8CS, uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed)
{
  pBBEP->iDCPin = u8DC;
  pBBEP->iCSPin = u8CS;
  pBBEP->iMOSIPin = u8MOSI;
  pBBEP->iCLKPin = u8SCK;
  pBBEP->iRSTPin = u8RST;
  pBBEP->iBUSYPin = u8BUSY;

  pinMode(pBBEP->iDCPin, OUTPUT);
  if (pBBEP->iRSTPin != 0xff) {
    pinMode(pBBEP->iRSTPin, OUTPUT);
    digitalWrite(pBBEP->iRSTPin, HIGH);
  }
  if (pBBEP->iBUSYPin != 0xff) {
    // Keep BUSY stable on BG22: UC81xx panels idle high, SSD16xx idle low.
    pinMode(pBBEP->iBUSYPin,
            (pBBEP->chip_type == BBEP_CHIP_UC81xx) ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
  pBBEP->iSpeed = (int)u32Speed;
  pinMode(pBBEP->iCSPin, OUTPUT);
  digitalWrite(pBBEP->iCSPin, HIGH);
  if (u32Speed == 0u) {
    pinMode(pBBEP->iMOSIPin, OUTPUT);
    pinMode(pBBEP->iCLKPin, OUTPUT);
  }
  silabs_hw_spi_init(u8MOSI, u8SCK, u32Speed);
}

void bbepWriteIT8951Cmd(BBEPDISP *pBBEP, uint16_t cmd)
{
  uint8_t ucTemp[4];
  ucTemp[0] = 0x60;
  ucTemp[1] = 0;
  ucTemp[2] = (uint8_t)(cmd >> 8);
  ucTemp[3] = (uint8_t)cmd;
  digitalWrite(pBBEP->iCSPin, LOW);
  SPI_Write(pBBEP, ucTemp, 4);
  digitalWrite(pBBEP->iCSPin, HIGH);
}

void bbepWriteIT8951Data(BBEPDISP *pBBEP, uint8_t *pData, int iLen)
{
  uint8_t z[2] = { 0, 0 };
  digitalWrite(pBBEP->iCSPin, LOW);
  SPI_Write(pBBEP, z, 2);
  SPI_Write(pBBEP, pData, iLen);
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
  SPI_Write(pBBEP, &cmd, 1);
  od_bbep_cs(pBBEP, HIGH);
  digitalWrite(pBBEP->iDCPin, HIGH);
}

void bbepWriteData(BBEPDISP *pBBEP, uint8_t *pData, int iLen)
{
  if (pBBEP->iFlags & BBEP_CS_EVERY_BYTE) {
    for (int i = 0; i < iLen; i++) {
      od_bbep_cs(pBBEP, LOW);
      SPI_Write(pBBEP, &pData[i], 1);
      od_bbep_cs(pBBEP, HIGH);
    }
  } else {
    od_bbep_cs(pBBEP, LOW);
    SPI_Write(pBBEP, pData, iLen);
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
  SPI_Write(pBBEP, &cmd, 1);
  digitalWrite(pBBEP->iDCPin, HIGH);
  delay(1);
  if (pData != NULL && iLen > 0) {
    SPI_Write(pBBEP, pData, iLen);
  }
  od_bbep_cs(pBBEP, HIGH);
}

void bbepCMD2(BBEPDISP *pBBEP, uint8_t cmd1, uint8_t cmd2)
{
  bbepWriteCmd(pBBEP, cmd1);
  bbepWriteData(pBBEP, &cmd2, 1);
}

#endif /* __SILABS_EFR32_IO__ */
