#include "boot_screen.h"

#include "od_boot_app.h"
#include "od_boot_screen.h"
#include "opendisplay_battery.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"

#include <bb_epaper.h>

static BBEPDISP *s_boot_epd;
static uint8_t s_boot_row[680];
static uint8_t s_boot_qr[256];

int od_boot_app_begin_frame(uint16_t width, uint16_t height, uint8_t segments)
{
  (void)width;
  (void)height;
  return s_boot_epd != nullptr && segments == 1u ? 0 : -1;
}

int od_boot_app_begin_plane(int plane)
{
  if (s_boot_epd == nullptr) return -1;
  const struct od_config *cfg = opendisplay_get_global_config();
  if (cfg == nullptr || cfg->display_count == 0u) return -1;
  bbepSetAddrWindow(s_boot_epd, 0, 0, cfg->displays[0].pixel_width,
                    cfg->displays[0].pixel_height);
  bbepStartWrite(s_boot_epd, plane == OD_BOOT_PLANE_PRIMARY ? PLANE_0 : PLANE_1);
  return 0;
}

int od_boot_app_write_row(uint16_t y, uint8_t segment, const uint8_t *row, uint16_t len)
{
  (void)y;
  if (s_boot_epd == nullptr || segment != 0u) return -1;
  bbepWriteData(s_boot_epd, (uint8_t *)(void *)row, len);
  return 0;
}

int od_boot_app_end_plane(int plane)
{
  (void)plane;
  return 0;
}

int od_boot_app_end_frame(void) { return 0; }

int od_boot_app_bits_per_pixel(uint8_t color_scheme)
{
  switch (color_scheme) {
    case OD_COLOR_SCHEME_BWGBRY:
    case OD_COLOR_SCHEME_BWGBRY_SPLIT:
    case OD_COLOR_SCHEME_GRAY16:
      return 4;
    case OD_COLOR_SCHEME_BWRY:
    case OD_COLOR_SCHEME_GRAY4:
      return 2;
    case OD_COLOR_SCHEME_MONO:
    case OD_COLOR_SCHEME_BWR:
    case OD_COLOR_SCHEME_BWY:
    case OD_COLOR_SCHEME_SEVEN_COLOR:
      return 1;
    default:
      return 0;
  }
}

int od_boot_app_default_plane(uint8_t color_scheme)
{
  switch (color_scheme) {
    case OD_COLOR_SCHEME_MONO:
    case OD_COLOR_SCHEME_BWR:
    case OD_COLOR_SCHEME_BWY:
    case OD_COLOR_SCHEME_GRAY16:
      return OD_BOOT_PLANE_PRIMARY;
    default:
      return OD_BOOT_PLANE_SECOND;
  }
}

bool od_boot_app_direct_2bpp(void) { return false; }

uint8_t od_boot_app_segments(void)
{
  /* Split panels remain refused until the held-CS transport is hardware-qualified. */
  return 1u;
}

uint32_t od_boot_app_device_id24(void)
{
  return opendisplay_ble_chip_id_last24();
}

void od_boot_app_firmware_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
  uint16_t version = opendisplay_ble_get_app_version();
  *major = (uint8_t)(version >> 8);
  *minor = (uint8_t)version;
  *patch = opendisplay_ble_get_app_version_patch();
}

float od_boot_app_battery_volts(void)
{
  return opendisplay_battery_read_voltage_volts();
}

float od_boot_app_chip_temp_c(void)
{
  return opendisplay_ble_get_chip_temperature();
}

bool writeBootScreenWithQr(BBEPDISP &epd)
{
  const struct od_config *cfg = opendisplay_get_global_config();
  const struct SecurityConfig *security = od_get_parsed_security();
  const struct od_boot_bufs bufs = {s_boot_row, sizeof(s_boot_row),
                                    s_boot_qr, sizeof(s_boot_qr)};
  s_boot_epd = &epd;
  bool result = od_boot_screen_render(cfg, security, &bufs);
  s_boot_epd = nullptr;
  return result;
}
