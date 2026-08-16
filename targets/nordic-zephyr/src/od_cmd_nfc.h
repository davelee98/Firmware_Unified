/* od_cmd_nfc.h -- see od_cmd_config.h; the chunked NFC write has the same shape and the same
 * reason to be torn down when its connection goes. */

#ifndef OD_CMD_NFC_H
#define OD_CMD_NFC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Drop any chunked NFC write in progress. Consumer/main context only. */
void od_cmd_nfc_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_NFC_H */
