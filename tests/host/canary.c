/* canary.c — proves the host build contract is real while shared/ is still empty.
 *
 * Without this, tests/host/ would be a build with nothing to build: green, but never
 * exercised, so the first shared/core file to land would be the first thing to discover
 * whether -Werror was actually wired up. This translation unit closes that gap by
 * compiling the one thing shared/ already contains — the synced protocol headers —
 * under the exact flags every later source will face.
 *
 * It also makes a real assertion nobody has made before: the headers under
 * shared/protocol had never been compiled in this repo at all. They are a byte-for-byte
 * copy from opendisplay-protocol
 * that arrived by file copy, and CI only ever grepped it. "It parses as C99 with
 * -Wall -Wextra -Werror" is a genuine, previously-unverified property — and one the
 * header's own authors intended to be checked: see the comment above OD_STATIC_ASSERT in
 * opendisplay_structs.h, which names -std=c99 as "the gate used for this header".
 *
 * Delete this file when shared/ has real sources to compile. It is scaffolding.
 */

#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

/* OD_STATIC_ASSERT comes from opendisplay_structs.h, which already picks the right
 * mechanism per language level (static_assert / _Static_assert / negative-array typedef).
 * Do not define a local one — that is a macro redefinition under -Werror, which is how
 * this file first failed to build. */

/* Wire invariants worth pinning. Each is citable, and each would be a real breakage if it
 * changed silently under a --push from the canonical repo — currently unpoliced here,
 * since the sync tool's copy map does not list this repo yet (see CLAUDE.md). */

/* The chunk-size relationship the chunked CONFIG_WRITE path depends on: the first chunk
 * carries a 2-byte total ahead of a full-size payload. opendisplay_protocol.h:887-888. */
OD_STATIC_ASSERT(CONFIG_CHUNK_SIZE_WITH_PREFIX == CONFIG_CHUNK_SIZE + 2u,
                 "first chunk payload must be total(2) + CONFIG_CHUNK_SIZE");

/* Protocol version the corpus's min_protocol fields are written against. */
OD_STATIC_ASSERT(OD_PROTOCOL_VERSION_MAJOR == 2u, "protocol major is 2");

/* OD_PKT_NFC is canonical schema, not an extension — the fact that settles the NFC
 * placement decision (docs/SHARED_API_DESIGN.md § "NFC: standard packet, optional
 * support"). If this stops being true, that decision needs revisiting. */
OD_STATIC_ASSERT((int)OD_PKT_NFC == 0x2A, "NFC config packet is canonical schema");

/* The host build links this as an executable so CTest can run it; there is nothing to
 * run yet, so success is "it compiled and the assertions above held". */
int main(void)
{
    return 0;
}
