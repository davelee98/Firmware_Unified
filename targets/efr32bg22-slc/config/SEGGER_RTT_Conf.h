/* SEGGER_RTT_Conf.h -- TARGET-OWNED RTT configuration.
 *
 * The RTT *implementation* comes from the pinned Simplicity SDK (see
 * cmake_gcc/opendisplay-bg22.cmake); only this configuration is ours, and it is not
 * interchangeable with the SDK's default copy:
 *
 *   here      2 up / 2 down buffers, 1024 B up, 16 B down
 *   SDK       10 / 10 buffers,        1024 B up, 1024 B down
 *
 * On a part with 32 KB of RAM total that difference is not cosmetic. Taking the SDK default
 * would cost RAM this target has already fully accounted for. Kept in config/ beside the other
 * target-owned Silabs configuration rather than vendored beside the SDK source it configures.
 */
#ifndef SEGGER_RTT_CONF_H
#define SEGGER_RTT_CONF_H

#define SEGGER_RTT_MAX_NUM_UP_BUFFERS       2
#define SEGGER_RTT_MAX_NUM_DOWN_BUFFERS     2
#define BUFFER_SIZE_UP                      1024
#define BUFFER_SIZE_DOWN                    16
#define SEGGER_RTT_PRINTF_BUFFER_SIZE       256

#endif
