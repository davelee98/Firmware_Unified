#include "opendisplay_display_color.h"

/*
 * Byte counts follow Firmware handleDirectWriteStart (display_service.cpp) and the
 * py-opendisplay encoders (encoding/images.py, encoding/bitplanes.py) that hosts
 * actually send. All sizes are ROW-PADDED: each row occupies ceil(w * bpp / 8) bytes,
 * matching np.packbits(axis=1). This diverges from a flat pixel-count size only on
 * panels whose (width * bpp) is not a multiple of 8 (e.g. 122-wide EP213); the flat
 * form under-counts there and auto-completes before the bottom rows are written.
 *
 * color_scheme 5 (4-gray, GRAY4) and 1/2 (BWR/BWY) are two-plane schemes: they arrive
 * as two pre-split, row-padded 1-bit controller planes concatenated (plane0 then
 * plane1) and stream to PLANE_0/PLANE_1 with a switch at the single-plane boundary
 * (Firmware streamGray4Bytes; py-opendisplay encode_gray4_bitplanes / encode_bitplanes).
 *
 * color_scheme 6 (16 gray, GRAY16) uses 4bpp packed nibbles: py-opendisplay sends
 * encode_4bpp for it. The Arduino Firmware's getBitsPerPixel() returns 1 for scheme 6,
 * which is the outlier/bug; NRF54 deliberately expects 4bpp to match the client.
 */

uint32_t opendisplay_color_bitplane_plane_bytes(uint32_t w, uint32_t h)
{
  /* Row-padded 1-bit plane: ceil(w / 8) bytes per row (np.packbits axis=1). */
  return ((w + 7u) / 8u) * h;
}

int opendisplay_color_bits_per_pixel(uint8_t color_scheme)
{
  if (color_scheme == 4u) {
    return 4;
  }
  if (color_scheme == 3u || color_scheme == 5u) {
    return 2;
  }
  if (color_scheme == 6u) {
    return 4;
  }
  return 1;
}

bool opendisplay_color_is_bitplanes(uint8_t color_scheme)
{
  /* BWR(1), BWY(2) and GRAY4(5) all stream as two row-padded 1-bit planes,
     plane0 then plane1 (Firmware streamGray4Bytes / encode_gray4_bitplanes). */
  return (color_scheme == 1u || color_scheme == 2u || color_scheme == 5u);
}

int opendisplay_color_start_plane(uint8_t color_scheme)
{
  /* MONO(0), GRAY16(6) and the two-plane schemes (BWR/BWY/GRAY4) all begin
     streaming into PLANE_0; packed multi-color schemes (BWRY/BWGBRY) use PLANE_1.
     Note: Firmware getplane() returns PLANE_1 for GRAY4, but its direct-write path
     (streamGray4Bytes) starts at PLANE_0 regardless, which is what we match here. */
  if (color_scheme == 0u || color_scheme == 6u) {
    return 0;
  }
  if (opendisplay_color_is_bitplanes(color_scheme)) {
    return 0;
  }
  return 1;
}

uint32_t opendisplay_color_direct_write_total_bytes(uint32_t w, uint32_t h, uint8_t color_scheme)
{
  if (opendisplay_color_is_bitplanes(color_scheme)) {
    return opendisplay_color_bitplane_plane_bytes(w, h) * 2u;
  }
  int bpp = opendisplay_color_bits_per_pixel(color_scheme);
  /* Row-padded: ceil(w * bpp / 8) bytes per row, matching the sender's per-row
     byte boundary (np.packbits(axis=1) after padding width to a whole byte). */
  uint32_t bytes_per_row = (uint32_t)(((uint64_t)w * (uint64_t)bpp + 7u) / 8u);
  return bytes_per_row * h;
}
