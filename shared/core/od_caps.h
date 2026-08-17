/* od_caps.h -- compile-time facts declared by a target.
 *
 * Defaults preserve the fully capable profile. A target that omits a subsystem must say so on
 * its compile line; shared code derives sizing and reservation consequences from these facts.
 * Capabilities never remove opcode routes or od_cmd_app hooks -- every target still has to state
 * its wire behaviour for every canonical command.
 */

#ifndef OD_CAPS_H
#define OD_CAPS_H

#ifndef OD_CAP_PIPE
#define OD_CAP_PIPE 1
#endif

#ifndef OD_CAP_PARTIAL
#define OD_CAP_PARTIAL 1
#endif

#ifndef OD_CAP_RXQ
#define OD_CAP_RXQ 1
#endif

#if (OD_CAP_PIPE != 0) && (OD_CAP_PIPE != 1)
#error "OD_CAP_PIPE must be 0 or 1"
#endif
#if (OD_CAP_PARTIAL != 0) && (OD_CAP_PARTIAL != 1)
#error "OD_CAP_PARTIAL must be 0 or 1"
#endif
#if (OD_CAP_RXQ != 0) && (OD_CAP_RXQ != 1)
#error "OD_CAP_RXQ must be 0 or 1"
#endif

#endif /* OD_CAPS_H */
