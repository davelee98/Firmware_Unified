#include "boot_screen.h"

#include "communication.h"
#include "display_fastepd.h"
#include "display_service.h"
#include "encryption.h"
#include "od_boot_app.h"
#include "od_boot_screen.h"
#include "structs.h"

#include <bb_epaper.h>
#include <stdlib.h>

extern struct od_config globalConfig;
extern struct SecurityConfig securityConfig;
extern BBEPDISP bbep;
extern uint8_t staticRowBuffer[BOOT_ROW_BUFFER_SIZE];

void bbepSetAddrWindow(BBEPDISP *epd, int x, int y, int width, int height);
void bbepStartWrite(BBEPDISP *epd, int plane);
void bbepWriteData(BBEPDISP *epd, uint8_t *data, int len);

static uint8_t s_boot_qr[256];

int od_boot_app_begin_frame(uint16_t width, uint16_t height, uint8_t segments)
{
  (void)width;
  (void)height;
  return segments == 1u ? 0 : -1;
}

int od_boot_app_begin_plane(int plane)
{
#if defined(OPENDISPLAY_FASTEPD)
  if (fastepd_driver_used()) return 0;
#endif
  bbepSetAddrWindow(&bbep, 0, 0, globalConfig.displays[0].pixel_width,
                    globalConfig.displays[0].pixel_height);
  bbepStartWrite(&bbep, plane == OD_BOOT_PLANE_PRIMARY ? PLANE_0 : PLANE_1);
  return 0;
}

int od_boot_app_write_row(uint16_t y, uint8_t segment, const uint8_t *row, uint16_t len)
{
  if (segment != 0u) return -1;
#if defined(OPENDISPLAY_FASTEPD)
  if (fastepd_driver_used()) {
    fastepd_boot_write_row(y, row, len);
    return 0;
  }
#else
  (void)y;
#endif
  bbepWriteData(&bbep, (uint8_t *)(void *)row, len);
  return 0;
}

int od_boot_app_end_plane(int plane)
{
  (void)plane;
  return 0;
}

int od_boot_app_end_frame(void)
{
#if defined(OPENDISPLAY_FASTEPD)
  if (fastepd_driver_used()) fastepd_boot_skip_planes();
#endif
  return 0;
}

int od_boot_app_bits_per_pixel(uint8_t color_scheme)
{
  (void)color_scheme;
  return getBitsPerPixel();
}

int od_boot_app_default_plane(uint8_t color_scheme)
{
  (void)color_scheme;
  return getplane() == PLANE_0 ? OD_BOOT_PLANE_PRIMARY : OD_BOOT_PLANE_SECOND;
}

bool od_boot_app_direct_2bpp(void)
{
#if defined(OPENDISPLAY_FASTEPD)
  return fastepd_driver_used();
#else
  return false;
#endif
}

uint8_t od_boot_app_segments(void)
{
  /* Split panels remain refused until the held-CS transport is hardware-qualified. */
  return 1u;
}

uint32_t od_boot_app_device_id24(void)
{
  char id[7];
  getChipIdHex(id, sizeof(id));
  return (uint32_t)strtoul(id, NULL, 16) & 0xFFFFFFu;
}

void od_boot_app_firmware_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
  *major = getFirmwareMajor();
  *minor = getFirmwareMinor();
  *patch = getFirmwarePatch();
}

float od_boot_app_battery_volts(void) { return readBatteryVoltage(); }
float od_boot_app_chip_temp_c(void) { return readChipTemperature(); }

bool writeBootScreenWithQr()
{
  const struct od_boot_bufs bufs = {
      staticRowBuffer, BOOT_ROW_BUFFER_SIZE, s_boot_qr, sizeof(s_boot_qr)};
  return od_boot_screen_render(&globalConfig, &securityConfig, &bufs);
}
