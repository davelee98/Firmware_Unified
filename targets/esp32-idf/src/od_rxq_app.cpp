/* od_rxq_app.cpp -- this target's answers to shared/core/od_rxq_app.h.
 *
 * The arrival and drop lines are no longer here: od_rxq.c logs them, one wording for both
 * targets. What is left is the two facts the shared line cannot answer from the ring -- whether
 * the CCM envelope is in force, and whether an opcode's per-frame line should be suppressed.
 *
 * Both run on the STACK CALLBACK TASK and are only reached in a build compiled at OD_LOG_DEBUG.
 */

#include "od_rxq_app.h"

#include "encryption.h"   // isEncryptionEnabled()
#include "opendisplay_protocol.h"

// Defined in display_service.cpp. True while an image stream is actually mid-flight, which is
// what makes the suppression state-aware rather than a blanket opcode filter: outside a stream
// these frames are worth a line.
bool imageWriteLogQuietCmd(void);

extern "C" bool od_rxq_app_encryption_enabled(void)
{
    return isEncryptionEnabled();
}

extern "C" bool od_rxq_app_quiet(uint16_t cmd)
{
    return (cmd == CMD_DIRECT_WRITE_DATA || cmd == CMD_PIPE_WRITE_DATA) &&
           imageWriteLogQuietCmd();
}
