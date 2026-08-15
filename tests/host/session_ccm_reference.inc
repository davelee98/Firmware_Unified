/* Differential fixture: the RFC 3610 soft CCM that targets/nordic-zephyr shipped, copied
 * VERBATIM from opendisplay_pipe.c:317-441 at commit 80cc028 before C1 deleted it. Its value is
 * that it is not a re-derivation, so do not "tidy" the arithmetic — if it ever disagrees with
 * shared/core/od_session.c, the sweep is doing its job.
 *
 * .inc, not .c, because it is NOT a translation unit: the includer must first define
 *
 *     static bool aes_ecb_encrypt_16(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
 *     static struct { uint8_t session_key[16]; } s_session;
 *
 * over its own AES-128 core (vendored in C3), plus <stdbool.h>, <stdint.h> and <string.h>. Those
 * two symbols are the only things the original took from its target; the CCM body below is
 * byte-identical to what shipped.
 */

/*
 * Minimal AES-CCM (RFC 3610) using AES-ECB as primitive.
 * Fixed parameters: nonce=13B, tag=12B, L=2, AAD=exactly 2 bytes.
 * B_0 flags = Adata(1) | M'=(12-2)/2<<3 | L-1 = 0x40|0x28|0x01 = 0x69
 */
static bool od_ccm_ecb(const uint8_t *in, uint8_t *out)
{
  return aes_ecb_encrypt_16(s_session.session_key, in, out);
}

static bool od_ccm_encrypt(const uint8_t *nonce,
                            const uint8_t *aad,
                            const uint8_t *plain,
                            uint16_t plain_len,
                            uint8_t *cipher,
                            uint8_t *tag)
{
  uint8_t blk[16], mac[16], stream[16];
  uint16_t i, full, rem;

  blk[0] = 0x69u;
  memcpy(&blk[1], nonce, 13u);
  blk[14] = (uint8_t)((plain_len >> 8) & 0xFFu);
  blk[15] = (uint8_t)(plain_len & 0xFFu);
  if (!od_ccm_ecb(blk, mac)) return false;

  memset(blk, 0, 16u);
  blk[0] = 0x00u; blk[1] = 0x02u;
  blk[2] = aad[0]; blk[3] = aad[1];
  for (i = 0; i < 16u; i++) mac[i] ^= blk[i];
  if (!od_ccm_ecb(mac, mac)) return false;

  full = plain_len / 16u; rem = plain_len % 16u;
  for (i = 0; i < full; i++) {
    for (uint8_t j = 0; j < 16u; j++) mac[j] ^= plain[i * 16u + j];
    if (!od_ccm_ecb(mac, mac)) return false;
  }
  if (rem > 0u) {
    memset(blk, 0, 16u);
    memcpy(blk, &plain[full * 16u], rem);
    for (uint8_t j = 0; j < 16u; j++) mac[j] ^= blk[j];
    if (!od_ccm_ecb(mac, mac)) return false;
  }

  blk[0] = 0x01u;
  memcpy(&blk[1], nonce, 13u);
  blk[14] = 0x00u; blk[15] = 0x00u;
  if (!od_ccm_ecb(blk, stream)) return false;
  for (i = 0; i < 12u; i++) tag[i] = mac[i] ^ stream[i];

  for (i = 0; i < full; i++) {
    blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
    blk[14] = (uint8_t)(((i + 1u) >> 8) & 0xFFu);
    blk[15] = (uint8_t)((i + 1u) & 0xFFu);
    if (!od_ccm_ecb(blk, stream)) return false;
    for (uint8_t j = 0; j < 16u; j++) cipher[i * 16u + j] = plain[i * 16u + j] ^ stream[j];
  }
  if (rem > 0u) {
    blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
    blk[14] = (uint8_t)(((full + 1u) >> 8) & 0xFFu);
    blk[15] = (uint8_t)((full + 1u) & 0xFFu);
    if (!od_ccm_ecb(blk, stream)) return false;
    for (uint8_t j = 0; j < rem; j++) cipher[full * 16u + j] = plain[full * 16u + j] ^ stream[j];
  }
  return true;
}

static bool od_ccm_decrypt(const uint8_t *nonce,
                            const uint8_t *aad,
                            const uint8_t *cipher,
                            uint16_t cipher_len,
                            const uint8_t *tag,
                            uint8_t *plain)
{
  uint8_t blk[16], mac[16], stream[16];
  uint16_t i, full, rem;
  uint8_t expected[12], diff;

  full = cipher_len / 16u; rem = cipher_len % 16u;
  for (i = 0; i < full; i++) {
    blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
    blk[14] = (uint8_t)(((i + 1u) >> 8) & 0xFFu);
    blk[15] = (uint8_t)((i + 1u) & 0xFFu);
    if (!od_ccm_ecb(blk, stream)) return false;
    for (uint8_t j = 0; j < 16u; j++) plain[i * 16u + j] = cipher[i * 16u + j] ^ stream[j];
  }
  if (rem > 0u) {
    blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
    blk[14] = (uint8_t)(((full + 1u) >> 8) & 0xFFu);
    blk[15] = (uint8_t)((full + 1u) & 0xFFu);
    if (!od_ccm_ecb(blk, stream)) return false;
    for (uint8_t j = 0; j < rem; j++) plain[full * 16u + j] = cipher[full * 16u + j] ^ stream[j];
  }

  blk[0] = 0x69u; memcpy(&blk[1], nonce, 13u);
  blk[14] = (uint8_t)((cipher_len >> 8) & 0xFFu);
  blk[15] = (uint8_t)(cipher_len & 0xFFu);
  if (!od_ccm_ecb(blk, mac)) return false;

  memset(blk, 0, 16u);
  blk[0] = 0x00u; blk[1] = 0x02u;
  blk[2] = aad[0]; blk[3] = aad[1];
  for (i = 0; i < 16u; i++) mac[i] ^= blk[i];
  if (!od_ccm_ecb(mac, mac)) return false;

  for (i = 0; i < full; i++) {
    for (uint8_t j = 0; j < 16u; j++) mac[j] ^= plain[i * 16u + j];
    if (!od_ccm_ecb(mac, mac)) return false;
  }
  if (rem > 0u) {
    memset(blk, 0, 16u);
    memcpy(blk, &plain[full * 16u], rem);
    for (uint8_t j = 0; j < 16u; j++) mac[j] ^= blk[j];
    if (!od_ccm_ecb(mac, mac)) return false;
  }

  blk[0] = 0x01u; memcpy(&blk[1], nonce, 13u);
  blk[14] = 0x00u; blk[15] = 0x00u;
  if (!od_ccm_ecb(blk, stream)) return false;
  for (i = 0; i < 12u; i++) expected[i] = mac[i] ^ stream[i];

  diff = 0u;
  for (i = 0; i < 12u; i++) diff |= (expected[i] ^ tag[i]);
  return (diff == 0u);
}
