/* od_cmd_config.h -- the only thing outside od_cmd_config.c that needs to reach into it.
 *
 * A chunked CONFIG_WRITE spans several frames, so it is the one configuration command with state
 * that outlives a dispatch. When the connection that opened it goes, that state must go too --
 * otherwise the next peer continues somebody else's transfer. */

#ifndef OD_CMD_CONFIG_H
#define OD_CMD_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Drop any chunked write in progress. Consumer/main context only. */
void od_cmd_config_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_CONFIG_H */
