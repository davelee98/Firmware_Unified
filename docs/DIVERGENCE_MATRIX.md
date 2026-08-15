# Divergence matrix

Where the four repos' protocol semantics actually differ, verified by reading the
implementations side by side (2026-07-25). This is the input MIGRATION.md's "silent
behavioural divergence" risk asks for: every difference below must be resolved
*deliberately* when its subsystem is promoted to `shared/core`, and the resolution column
here is the proposed default. Citations are `repo-relative path:line` in the source repos;
`Firmware` cites are the local checkout unless marked `upstream/main`.

> **Currency: behavioural tables surveyed before `#124`; refreshed for `#124` on 2026-07-25.**
> `Firmware` is now at `2e2131b` (PR #124 — WiFi/LAN transport + tinfl inflate, +2007/-198).
> The §0 capability matrix, §3.1a/§3.1b (window and engine) and the new §9 (LAN transport)
> reflect it. Rows not
> marked `#124` were verified against the pre-#124 tree and are unaffected by that commit —
> it touched transport, inflate, and logging, not dispatch or TLV semantics. **`Firmware` is
> the reference implementation, so re-verify this file against it after every significant
> merge**, not only before a promotion.

Legacy `Firmware_NRF` is included only where it matters. Verified verdict: it is a **strict
subset** — no compression, no 0x76, no PIPE, no NFC, no LED_STOP, no CONFIG_CLEAR, no
READ_MSD handler, `ocrypto`-backed crypto, and its `CMD_DEEP_SLEEP` log string still says
"(0x0052)" (`Firmware_NRF/EPD/EPD_service.c:619-666`, `:658`). Dropping it loses no
protocol semantics.

## Reference implementation and direction of travel

**`Firmware` — specifically its ESP32 branch — models the feature set.** It is the most
actively developed, carries the widest capability coverage, and is where new protocol features
land first (WiFi/LAN transport and the tinfl inflate engine arrived in `#124`, 2026-07-25,
ESP32-only). When a behaviour is contested and no correctness argument decides it, `Firmware`'s
ESP32 shape is the default answer.

The other repos — `Firmware_NRF54`, `Firmware_Silabs`, `Firmware_NRF` — **should converge
toward it.** That is the default direction for anything they implement differently for no
reason beyond history.

Three qualifications, all load-bearing:

1. **"Reference" is about feature coverage, not correctness.** Where another repo is
   demonstrably right, it wins and `Firmware` changes — the resolution column below already
   does this repeatedly (NRF54's size-table TLV scan §2.2, its chunked-write validation §2.3,
   its mid-session plaintext rejection §1.5a). Modelling the feature set does not make
   `Firmware` the arbiter of every detail.
2. **Other repos will sometimes have *more*.** `CMD_NFC_ENDPOINT` is implemented on Silabs and
   NRF54 and *not* on `Firmware`; Channel Sounding is nRF54L15-only. Convergence is not
   one-way.
3. **Other repos will sometimes have *less*, permanently, and that is fine.** BG22 will never
   run PIPE or `0x76`; a gap is not a defect and not a backlog item. See SHARED_API_DESIGN.md
   § "Feature parity across targets is **not** a goal" for how the API expresses this.

**The canonical protocol headers are what bind them together.**
`opendisplay_protocol.h` and `opendisplay_structs.h` are the single contract every repo
implements a subset of: they own the wire values, the config TLV types, and the error codes.
A capability may be absent from a target, but its *opcode and encoding never differ* between
targets — a divergence in wire meaning is always a bug, whereas a divergence in coverage is a
design decision. That distinction is the organising rule for everything below.

### The opcode space is conservative

**Avoid introducing new opcodes.** Every opcode is permanent wire surface: it has to be
implemented or deliberately NACKed on four targets, taught to `py-opendisplay` and the HA
integration, and supported for the life of every deployed device. The cost is paid by
everything downstream, forever, and it is paid even by targets that will never implement it.

Before proposing one, exhaust the cheaper options in this order:

1. **An existing opcode with an existing field.** Most "new" needs are a value, not a verb.
2. **An existing opcode with a reserved or extension field.** Several responses already carry
   documented reserved bytes (the 4-byte ack's two trailing zeros, §1.4).
3. **A new error/status code in an existing namespace.** The "subsystem compiled out" case is
   exactly this — a target that lacks `0x76` or PIPE needs a *code* meaning "unsupported", not
   a new opcode (see §1 opcode coverage, where `OD_ERR_PIPE_START_BAD_HEADER` is currently
   misused for it).
4. **A new packet type in the config TLV** — cheaper than a new opcode *in principle*, but see
   the caveat below before relying on it. Prefer reserved bits, then reserved bytes, in an
   existing packet first (ARCHITECTURE.md § "The config binary is backward compatible").

   > **Caveat: unknown-type skip is only safe on NRF54 today.** `Firmware` and `Firmware_Silabs`
   > force skip-to-CRC on an unrecognised packet type, discarding everything after it —
   > including `0x27` security config if the new type is ordered ahead of it (§2.2). Adding a
   > packet type would therefore break deployed units on two of three targets. This option
   > re-opens once NRF54's size-table parser is the one in `shared/core`.
5. **A new opcode** — last resort.

**But do propose one when it genuinely simplifies the structure.** This is not a prohibition.
If a new opcode removes a class of special-casing, collapses several overloaded meanings of an
existing opcode, or replaces a fragile inference with an explicit signal, that is a real
simplification and worth the wire cost. Two live candidates from this document:

- **Capability / limits discovery.** ~~`MAX_CONFIG_SIZE` diverges (2048 on BG22, 4096
  elsewhere, §2.7) with no way for a host to learn the limit.~~ **This candidate lost its
  motivating case on 2026-07-25**: `MAX_CONFIG_SIZE` is now 4096 fleet-wide, so there is no
  per-device limit to discover and no oversized-config truncation to report. Limits discovery
  may still be wanted for the remaining values (max accepted `windowBits`, max frame size), but
  it must now be argued on those alone — which is a materially weaker case than the one that
  put it on this list. Whether it becomes a new opcode, an extension of
  `0x43 FIRMWARE_VERSION`, or a config TLV field remains the kind of trade-off to argue
  explicitly rather than settle by whoever implements first.
- **Explicit compression signalling.** Direct-write infers "compressed" from payload length
  rather than a flag (§3.1). It works, but it is inference where a bit would do.

A proposal must state: what it replaces, why options 1-4 cannot carry it, and the cost to every
target including those that will NACK it. Land the argument in these docs first — the canonical
headers are frozen, so no opcode can be added unilaterally regardless.

**This document is maintained, not a one-time survey.** Update it when a feature lands in any
repo, when a target adds or drops a capability, and when a subsystem is promoted to
`shared/core`. The capability matrix in § "Opcode coverage" is the quick answer to "what does
this repo support"; the behavioural tables are the detail.

## Reading the table

- **spec** = the canonical header (`shared/protocol/opendisplay_protocol.h`, 2.2+unreleased).
- "all three" = `Firmware` (ESP32/nRF52840), `Firmware_NRF54`, `Firmware_Silabs`.
- Where implementations unanimously contradict the spec, the fix is a **spec correction**
  (recommendation only — the header is frozen for this exercise), not three firmware changes.

## 0. Capability matrix — what each repo supports

The non-opcode half of "what does this repo do". Opcode-level coverage is in § "Opcode
coverage" below. `Firmware` is split where its two chip families differ, because they do.

| Capability | Firmware / ESP32 | Firmware / nRF52840 | NRF54 | Silabs | NRF legacy |
|---|---|---|---|---|---|
| BLE transport | NimBLE | Bluefruit | Zephyr BT host | Silabs BGAPI (`sl_bt_*`) | Nordic SoftDevice |
| WiFi / LAN TLS transport | **yes** (`#124`) | no | no | no | no |
| Inflate engine | **tinfl ROM** when `OPENDISPLAY_ENABLE_WIFI`, else uzlib (`#124`) | uzlib | uzlib | uzlib | **none** (uncompressed only) |
| Inflate RAM cost | **~11 KB** tinfl tables + 4 KB dict (9-bit window) vs ~2.5 KB uzlib | ~2.5 KB | ~2.5 KB | 1676 B measured | n/a |
| zlib window default | 512 B (32 KB on `esp32-s3-E1004` only) | 512 B | 512 B | 512 B | n/a |
| Panel backend | bb_epaper SPI + **FastEPD parallel** (S3) | bb_epaper SPI | bb_epaper SPI | bb_epaper SPI | direct driver |
| `MAX_CONFIG_SIZE` | 4096 | 4096 | 4096 | **2048** → 4096 (decided 2026-07-25, §2.7) | — |
| Crypto backend | mbedTLS CCM | mbedTLS CCM | PSA | PSA | `ocrypto` |
| PIPE sliding window `0x80`-`0x82` | **yes — only implementation** | no | no | no | no |
| Partial region `0x76` | yes | yes | yes | no (fail-fast NACK) | no |
| NFC endpoint (TNB132M) | no | no | **yes** | **yes** | no |
| Buzzer | yes | yes | yes | no (NACK) | no |
| Channel Sounding / ranging | no | no | **yes** (`device_flags` bit 5, default off) | no | no |
| Field OTA (host-driven) | **none** — `ENTER_DFU` only reboots | **yes** — Adafruit bootloader BLE DFU (`bledfu.begin()`, `CMD_ENTER_DFU` sets the bootloader magic), driven by `perform_nrf_dfu`; not wired into HA (verified 2026-07-25 — targets/nordic-zephyr/README.md § "Deployed nRF52840") | firmware-complete (MCUboot + SMP BT OTA DFU) but **undriveable** — `py-opendisplay` has no SMP/mcumgr client (targets/nordic-zephyr/README.md § Bootloader) | **yes** — `.gbl` via Silabs AppLoader, the only path wired into HA | library support exists (`perform_nrf_dfu`, legacy Nordic DFU `.zip`) but unused by HA |
| Kernel | Arduino loop / FreeRTOS | Arduino loop | Zephyr | **none** (superloop) | none |
| Config storage | LittleFS/NVS | NVS | Zephyr settings | NVM3 | flash |

Bold marks a capability unique to one column, in either direction — those are the cells that
show convergence is not one-way. `Firmware`'s ESP32 column is the widest, which is what
"models the feature set" means concretely; Silabs is the narrowest and defines the floor
`shared/core` must fit through.

Keep this table current as features land. A cell that changes without the table changing is
how the four repos drifted apart the first time.

## 1. Framing and dispatch

| # | Behaviour | spec | Firmware | NRF54 | Silabs | Resolution for `shared/core` |
|---|---|---|---|---|---|---|
| 1.1 | Auth-required reply | `[0xFE][echo]` (status byte 0xFE) | `{0x00, cmd_lo, 0xFE}` — 0xFE as *data* (`src/communication.cpp:630,637`) | same (`src/opendisplay_pipe.c:277`) | same (`opendisplay_pipe.c:227`) | Implementations are unanimous and clients parse the shipped shape. **Correct the spec** to `[0x00][echo][0xFE]`; do not change firmware. |
| 1.2 | Decrypt-failure reply | not specified | `{0x00, cmd_lo, 0xFF}` (`communication.cpp:668`) | same (`opendisplay_pipe.c:1349`) | same (`opendisplay_pipe.c:1244`) | Adopt as-is; document in spec. |
| 1.3 | Oversize inbound frame | reject via ATT 0x0D (declared GATT max) | GATT `max_len` = `OD_BLE_MAX_FRAME` rejects at ATT layer (`src/ble_init.cpp:277-286`) | app-level NACK `{0xFF, cmd_lo, 0xFE}` (`opendisplay_pipe.c:1333-1337`) | same app-level NACK (`opendisplay_pipe.c:1231-1235`) | Keep both belts: declare GATT max per target *and* keep the core-level length check (LAN has no ATT layer). |
| 1.4 | Ack frame width | 2-byte `[0x00][echo]` | config/LED/buzzer acks are **4-byte** `{status, echo, 0x00, 0x00}` (`communication.cpp:476-486`); direct-write acks 2-byte | same split (`opendisplay_pipe.c:811-812, 891-892, 743-755`) | same | Adopt the shipped shapes verbatim (clients depend on them); spec should document the two trailing zero bytes as reserved. |
| 1.5 | Plaintext exemptions when security on, pre-auth | AUTHENTICATE + FIRMWARE_VERSION always plaintext; Silabs also READ_MSD | exempts AUTH + FW_VERSION before the gate (`communication.cpp:613-621`); config write/chunk pass if `rewrite_allowed` flag, after secure erase (`:447-455, 508-517`) | same policy (`opendisplay_pipe.c:1187-1214`), plus rejects *plaintext* non-FW_VERSION commands **mid-session** (`:1356-1370`) | dispatch auth-gates everything except AUTHENTICATE (`opendisplay_pipe.c:1079-1084`); READ_MSD forced plaintext in `pipe_send` (`:532`) | **NRF54 is right** (most complete and matches spec). Shared dispatcher fixes both Silabs issues on adoption. |
| 1.5a | **Encrypted-frame detection when security on** | gate on `isEncryptionEnabled()` | length gate: decrypt only if `len ≥ BLE_CMD_HEADER+NONCE+TAG` else treat as auth-required (`communication.cpp:635`) | length gate `frame_len ≥ 31`, but **also** rejects short plaintext mid-session (`opendisplay_pipe.c:1342-1370`) — closes the hole | length gate `frame_len ≥ 31` with **no mid-session guard** (`opendisplay_pipe.c:1238-1251`) | **Security bug on Silabs**: once any client authenticates, a *short* (<31 B) plaintext command — REBOOT (2 B), DEEP_SLEEP, DIRECT_WRITE_END — bypasses CCM and executes, because `dispatch()` only blocks when there is *no* live session. NRF54's mid-session plaintext rejection is the correct model; shared dispatcher must gate on `sec_enabled()`, never on frame length. |
| 1.5b | NACK confidentiality | unspecified | NACKs (0xFF) sent unencrypted (`communication.cpp:218`) | same (`opendisplay_pipe.c:571`) | same — every NACK forced plaintext (`opendisplay_pipe.c:532`) | Unanimous; document. A NACK leaks only an opcode+error code, so plaintext is acceptable, but make it a deliberate rule in `shared/core`. |
| 1.6 | Origin-gated decrypt (LAN TLS bypasses CCM) | SECTION 9 rule 4 | implemented via `g_commandOrigin` (`communication.cpp:27-45, 623-679`) | n/a (no LAN) | n/a | Core takes `origin` as an explicit argument of frame RX (see SHARED_API_DESIGN.md); no global. |
| 1.7 | Execution context | unspecified | single `loop()` task; BLE cb enqueues to `commandQueue`, responses via 10-slot ring (`communication.cpp:89-131`) | BT RX thread copies into `K_MSGQ` (8 × 514 B ≈ 4.1 KB), main thread drains via `opendisplay_pipe_process()` (`opendisplay_pipe.c:70-84, 1375-1425`) | dispatch runs directly in the BGAPI event handler, i.e. superloop context (`opendisplay_pipe.c:1271-1298`) | Adopt the NRF54 shape as the core API: RX enqueue + `od_core_process()` pump. Silabs pumps inline (queue depth 1); queue depth is a target macro. |
| 1.8 | Long-write reassembly (ATT prepare/execute) | unspecified | not needed (NimBLE hands whole values) | rejects > 509 B at msgq bound (`opendisplay_pipe.c:75, 1380`) | explicit offset-based reassembly into `s_long_write_buf` (`opendisplay_pipe.c:1209-1227`) | Keep reassembly in the target BLE glue, not in core — it is a transport artifact. |
| 1.9 | Unknown opcode | no reply specified | logs, **no reply** (`communication.cpp:750-752`) | logs, no reply (`opendisplay_pipe.c:1316-1318`) | logs, no reply (`:1188-1190`) | Adopt; note clients must use timeouts for unknown commands. |

### Opcode coverage (implemented handler exists)

| Opcode | Firmware | NRF54 | Silabs | NRF legacy | Notes |
|---|---|---|---|---|---|
| 0x0F REBOOT | yes | yes | yes | yes | |
| 0x40/41/42 CONFIG R/W/CHUNK | yes | yes | yes | yes | |
| 0x43 FW_VERSION | yes | yes | yes | yes | all reply `[00][43][maj][min][sha_len][sha≤40]` — the `sha_len` byte is not in the spec's layout (`communication.cpp:382-392`, `opendisplay_pipe.c:587-608`) |
| 0x44 READ_MSD | yes | yes | yes | **no** (macro only) | |
| 0x45 CONFIG_CLEAR | yes | yes | **no case — silent drop** (grep: zero hits in `opendisplay_pipe.c`) | no | **spec error**: `@targets` for 0x45 lists Silabs. Either implement on Silabs or fix `@targets`. |
| 0x50 AUTHENTICATE | yes | yes | yes | yes | |
| 0x51 ENTER_DFU | yes | yes | yes | yes | |
| 0x52 POWER_OFF | yes (latch HW) | **no case — silent drop** (`opendisplay_pipe.c:1216-1319` switch) | NACK unsupported (`:1114-1123`) | no | spec says non-latch targets NACK `[FF][52][00]`. **NRF54 gap** — Silabs is the compliant model. |
| 0x53 DEEP_SLEEP | yes | recognized, deliberately **no reply** (`:1248-1254`) | ACKs then EM4, ignores payload (`:1124-1130`) | yes (opcode drift, see above) | spec permits both ("may instead stay silent"); shared handler should emit the proper NACK namespace and make silence unnecessary. |
| 0x70/71/72 DIRECT_WRITE | yes | yes | yes | yes (uncompressed only) | |
| 0x73/0x75 LED | yes | yes | yes | 0x73 only | |
| 0x76 PARTIAL | yes | yes | NACK `{FF,76,07,00}` fail-fast (`:1177-1186`) | no | Silabs fail-fast NACK is correct behaviour for an unsupporting target — adopt as the default for any compiled-out subsystem. |
| 0x77 BUZZER | yes | yes | NACK `{FF,77,07,00}` | no | |
| 0x80-0x82 PIPE | **yes — only implementation** | no (silent drop) | no (silent drop) | no | non-implementing targets should NACK the 0x80 START (`OD_ERR_PIPE_START_BAD_HEADER` is wrong; needs an "unsupported" code — spec gap) rather than silently drop. |
| 0x83 NFC | no (falls to default; `communication.cpp:684-685`) | yes | yes | no | |

## 2. Config TLV parsing

Three parsers (`Firmware/src/config_parser.cpp` 919 C++, `Firmware_NRF54/src/opendisplay_config_parser.c` 696 C, `Firmware_Silabs/opendisplay_config_parser.c` 529 C) share the same outer container: toolbox-outer CRC16-CCITT-FALSE with the two length bytes fed as zeros (`config_parser.cpp:252-268`, NRF54 `:70-95`, Silabs `:17-45` — Silabs alone uses the `OD_CONFIG_CRC_*` named constants).

| # | Behaviour | Firmware | NRF54 | Silabs | Resolution |
|---|---|---|---|---|---|
| 2.1 | Packet-type coverage | 14 types; **no 0x2A NFC** (`config_parser.cpp:329-614`) | size-table for all types, cross-checked against three sources (`:105-109`) | 15 types incl. 0x2A NFC; skips 0x26/0x28/0x29/0x2C as host-only (`:103-463`) | Shared parser covers the full `opendisplay_structs.h` set; per-target `#if` only for *applying* a packet, never for parsing it. **Firmware's missing 0x2A is a live defect, not a capability gap (established 2026-07-25).** `OD_PKT_NFC = 0x2A` is canonical schema, and Firmware forces skip-to-CRC on any unrecognised type (2.2) — so an ESP32 receiving a config that merely *mentions* NFC abandons the rest of the blob at that point, losing `0x2B flash_config` and `0x2C data_extended`. `0x27 security` survives only because it sorts ahead of `0x2A`; that is emission order, not a guarantee. ESP32 is entitled to not *support* NFC (`OD_NFC_ENABLE=0`); it is not entitled to lose the packets that follow. Fix on promotion via the NRF54 size table. |
| 2.2 | Unknown packet type | skip to CRC | **size-table skip**: known-size packets are stepped over; only genuinely unknown IDs force skip-to-CRC, plus a `rescan_security_packet` fallback so 0x27 after an unknown ID still loads (`:42-46, 621-623`) | skip handling per-type | **NRF54 is right** — its comment documents the exact bug the others have (a new packet type ahead of 0x27 silently drops security config). Take the size-table + ordered-scan design. |
| 2.3 | Chunked-write validation | does **not** validate declared total ≤ `MAX_CONFIG_SIZE` at START; commits when `receivedChunks ≥ expectedChunks` without checking `receivedSize == totalSize` (`communication.cpp:456-478, 527-537`) | validates total at START (`opendisplay_pipe.c:906`), rejects END with `received_size != total_size` (`:1004-1009`), binds the chunk context to a connection (`:901, 972`) | as NRF54 minus some checks | **NRF54 is right**; adopt all three tightenings. |
| 2.4 | Session invalidation after config save | `clearEncryptionSession()` in `reloadConfigAfterSave` (`communication.cpp:67`) | `clear_session()` on every save path (`opendisplay_pipe.c:939, 960, 1012`) | **never clears the session after a save** (`clear_session` call sites: `:118, 431, 621, 673, 1263` — none post-save) | **Security-relevant Silabs bug**: change the encryption key over an old session and the old session keeps working. Firmware/NRF54 behaviour wins. |
| 2.5 | Config-read scratch | shared 4 KB scratch, `getConfigScratch()` — deliberate stack-overflow avoidance (`communication.cpp:395-399`) | **own 4 KB static** `config_data[MAX_CONFIG_SIZE]` (`opendisplay_pipe.c:824`) on top of the 4 KB chunk buffer — ~20 KB of config buffers total across the file | shared scratch `opendisplay_config_buf()` (`opendisplay_pipe.c:725-727`) | One shared scratch (Firmware/Silabs pattern). On BG22 the NRF54 pattern would waste 4 KB of a 32 KB chip. |
| 2.6 | Read chunk cap | `(MAX_CONFIG_SIZE+93)/94` (`communication.cpp:405-408`) | same, with the derivation comment (`:826-832`) | `ceil(MAX_CONFIG_SIZE / (MAX_RESPONSE_DATA_SIZE-6))` — same value, cleaner form (`:729-735`) | Silabs form. |
| 2.7 | **`MAX_CONFIG_SIZE`** | — | **4096** (`config_parser.h:7`) | **4096** (`opendisplay_config_storage.h:18`) | **2048** (`opendisplay_config_storage.h:7`, NVM3 record 2064 B) | **RESOLVED 2026-07-25 — 4096 fleet-wide, BG22 raised to match.** A single product-wide constant, *not* a per-target macro: any device accepts up to 4096 bytes of config, so the divergence is removed rather than made discoverable. Three consequences. (1) `py-opendisplay` needs no change — it already hardcodes 4096 (`config_serializer.py:672-675`), so the host-side work item F5 opened is closed by the decision instead. (2) `MAX_CONFIG_SIZE` drops out of the capability-reporting reserved bytes (ARCHITECTURE.md § "The gap, and a proposed fix"), since there is no longer a per-device number to report. (3) **The cost is BG22-only and material** — +2048 B of NVM3 record and +2048 B of read scratch against a 10 576 B heap; the two mitigations in MEMORY_CONSTRAINTS.md item 3 (shared scratch, bitmap replay window) become required rather than optional, and NVM3 max-object-size and instance capacity must be verified before the Silabs swap. **4096 is the ABSOLUTE ceiling — storable and transferable (2026-07-25).** Only 4000 is reachable today (`MAX_CONFIG_CHUNKS` 20 × 200 B, enforced on all three targets), so `MAX_CONFIG_CHUNKS` must become **21** = `ceil(4096/200)` — a queued canonical-header change, blocked by the freeze. Until then hosts must cap at 4000, and deployed units enforce 20 until reflashed. See FOLLOWUPS.md § 3.1. |
| 2.8 | Outer CRC16 enforcement | advisory only — mismatch logs, config still applied (`config_parser.cpp:665-672` region) | advisory only (`opendisplay_config_parser.c:657-670`) | advisory only (`opendisplay_config_parser.c:490-498`) | **Unanimous** — the toolbox-outer CRC16 is never enforced anywhere; a second inner CRC32 in the storage record *is* enforced on load. So there is no divergence, but `shared/core` should decide deliberately whether to start enforcing CRC16 (it currently protects nothing). |

## 3. Direct-write 0x70/0x71/0x72

| # | Behaviour | Firmware | NRF54 | Silabs | Resolution |
|---|---|---|---|---|---|
| 3.1 | Compressed START detection | `len >= 4` ⇒ compressed, `[uncompressed_size:4 LE]` validated against computed panel geometry (`display_service.cpp:2138-2148`). **No flag byte** — length is the only signal | same wire contract via `opendisplay_display_direct_write_start` (`.cpp:771`) | same (`opendisplay_display.cpp:788`) | identical — promote as-is, but document that "compressed" is inferred from payload length, not a flag. |
| 3.1a | **zlib window (`OPENDISPLAY_ZLIB_WINDOW_BITS`)** | encoder must match | default **9 (512 B)**; only `env:esp32-s3-E1004` pins **15 (32 KB)** (`platformio.ini:152`); heap window on most envs | **9 static** (`zephyr/CMakeLists.txt:66` sets `USE_HEAP_WINDOW=0`) | **9 static** (`opendisplay-bg22.cmake:269-270`) | **The workspace-level "existing targets pin 32 KB for legacy-client compatibility" note is wrong for all but one board.** The real default is 512 B everywhere; exactly one ESP32-S3 board wants 32 KB. All targets *reject* a stream whose CMF declares a window larger than their limit (`od_zlib_stream.c:641-644`), so the encoder MUST cap `windowBits` at the smallest target it addresses — 9. `shared/compress` treats window as a per-target macro, floor 9; see MEMORY_CONSTRAINTS.md. |
| 3.1b | **Inflate engine (`#124`)** | not a wire property | **tinfl (ROM miniz)** when `OPENDISPLAY_ENABLE_WIFI`, via an *unconditional* compile-time remap of the `od_zlib_stream_*` call sites — so it serves direct-write, 0x76 **and** PIPE, not just LAN (`src/od_inflate_tinfl.h:12-19`). Costs **~11 KB** DRAM for Huffman tables + a 4 KB dict at the 9-bit window, vs ~2.5 KB for uzlib | uzlib bit-serial | uzlib bit-serial, 1676 B measured | Engine choice is invisible on the wire **provided the window contract holds** — both engines reject an over-wide CMF. Two consequences for `shared/compress`: the engine is a target-selected backend behind one streaming API (reset/push/poll/error/count), which the tinfl adapter already proves by reusing uzlib's status enum; and **tinfl can never be the shared default** — its ~11 KB working set alone exceeds the BG22's entire 10.3 KB heap. The `OPENDISPLAY_ENABLE_WIFI` gate is a proxy for "can spare the RAM", *not* a transport filter — do not carry that conflation into `shared/`. |
| 3.2 | Uncompressed auto-complete | when `bytes_written == total`, END runs implicitly without a 0x72 (`display_service.cpp:2331-2332`) | *unverified at line level* (delegated into `opendisplay_display.cpp`) | *unverified* | Spec does not document auto-complete. Keep it (clients rely on it for full-frame pushes) and document it; verify NRF54/Silabs parity during their subsystem swaps. |
| 3.3 | END ack ordering | END-ack **before** the blocking refresh, then 0x73/0x74 (`display_service.cpp:2382-2385` comment + code) | same, explicitly, with a 20 ms TX drain gap (`opendisplay_pipe.c:796-799`) | **refresh happens first** — `opendisplay_display_direct_write_end` blocks through the ≤60 s refresh (`opendisplay_display.cpp:854-894`) and only then are `[00][72]` + `[00][73/74]` sent (`opendisplay_pipe.c:707-722`) | Spec and two of three say ack-then-refresh. **Silabs diverges**: its END ack can arrive a minute late; clients that time out on the END ack see false failures. Fix on adoption. |
| 3.4 | Refresh-mode byte on END | partial session: 0 FULL / 1 FAST / else PARTIAL (`display_service.cpp:2362-2364`); full: 0/1 | same contract | `payload[0]==1` ⇒ FAST else FULL (`opendisplay_display.cpp:875-877`) — no partial mode exists | consistent given capability differences. |
| 3.5 | Stray legacy frames mid-PIPE | 0x71/0x72 silently discarded while `pipeState.active` (`display_service.cpp:2280-2285, 2339-2343`) | n/a | n/a | Part of the PIPE state machine; promote with it. |

## 4. Partial-region 0x76

Only `Firmware` and `Firmware_NRF54` implement it. The `Firmware` handler is the reference
(`display_service.cpp:2173-2278`): 17-byte **big-endian** header, flags-whitelist →
etag (`old != 0 && old == displayed && new != 0`) → 1bpp-only → rect OOB → x/w alignment
(mult. of 8), then a two-plane stream (`plane_size * 2`, old plane then new). Error codes
are the `OD_ERR_PARTIAL_*` namespace, and any geometry/etag NACK clears `displayed_etag`.
NRF54 delegates to `opendisplay_display_partial_write_start(payload, len, &err_code_out)`
(`opendisplay_pipe.c:727-739`) with the same NACK framing; its internal validation order was
**not line-verified** here — check parity during the subsystem swap.

Two spec gaps found: 0x76 has no `RESP_*` mirror constant, so the echo byte is a raw
literal in firmware (TODO recorded at `display_service.cpp:2231-2234`) — add
`RESP_PARTIAL_WRITE_START` to the canonical header. And the PIPE-partial twin packs the
same geometry **little**-endian (`display_service.cpp:2483-2494`), which the spec does
document but which any shared header-parser must treat as two distinct layouts.

## 5. PIPE sliding window 0x80-0x82 (Firmware only)

The one subsystem with a single implementation, so no divergence — but its constants and
invariants must be recorded before promotion because no other repo can cross-check them:

- **Reorder queue**: `PIPE_REORDER_SLOTS` 33 (17 with `PIPE_SMALL_DRAM_WINDOW`), slot =
  4 + 248 data bytes ⇒ **~8.3 KB .bss** (~4.3 KB small) (`src/structs.h:32-53, 99-104`,
  `display_service.cpp:573-577`). `seq % SLOTS` is collision-free because a live window
  spans ≤ W < SLOTS.
- **Negotiation**: min-rule on window/ack-cadence/frame (`display_service.cpp:2725-2731`),
  response `{00,80,ver,W,N,frame:2LE,resp_flags}` (`:2785-2787`).
- **SACK**: 7-byte `{00,81,highest_seen,mask:4 LE}`; mask bit *i* = chunk
  `highest_seen-1-i` received; the accepted-prefix depth is bounded by `received_count` to
  avoid mod-256 phantom acks in the first 32 chunks (`:2512-2546`).
- **NACKs are fatal**: 8-byte `{FF,81,err,hs,mask:4}`; state deliberately not reset so the
  NACK payload reflects it; further 0x81 discarded until next START (`:2562-2566`).
- **The replay-window coupling is a hidden invariant**: the AES-CCM nonce counter advances
  at decrypt time for *every* 0x81 frame, including ones later queued or discarded, and the
  design depends on `in-flight ≤ W ≤ 32 ≤ ±32 replay window`
  (`communication.cpp:740-745`, `encryption.cpp:128-136`). If `shared/core` ever raises
  `PIPE_MAX_W` above 32 it must raise the replay window in lock-step. Encode this as a
  `static_assert` in the shared implementation.
- Uncompressed full-frame transfers auto-complete like legacy; **partial transfers never
  auto-complete** (`display_service.cpp:2481-2494, 2848`).

`shared/core` must make PIPE fully compile-out (`OD_PIPE_ENABLE=0`): Silabs cannot spend
8.3 KB (or even 4.3 KB) of RAM on it, and the spec's own `@targets` says only Firmware has
it. A non-implementing target should NACK the 0x80 START instead of today's silent drop —
which needs an "unsupported" code in the `OD_ERR_PIPE_START_*` namespace (spec addition).

## 6. Session auth / AES-CCM

The good news: the KDF chain is **byte-identical in all four repos** — label
`"OpenDisplay session"`, CMAC over `label||0x00||device_id||client_nonce||server_nonce||0x00 0x80`,
ECB finalization, session_id = first 8 bytes of CMAC(session_key, client||server)
(`encryption.cpp:61-107`, NRF54 `opendisplay_pipe.c:218-265`, Silabs same,
`Firmware_NRF/encryption.c:96-126`). Also identical: 30 s challenge window, 10-attempts/60 s
rate limit, ±32 counter window + 64-entry seen-list, 3-strike integrity teardown
(`encryption.cpp:526-560`, NRF54 `:407-438, 129-141`).

**Superseded 2026-08-15 on `esp32-idf` and `nordic-zephyr`** by `shared/core/od_session.c`. Rows
6.5–6.9 below are the promotion's behaviour changes; `efr32bg22-slc` and `Firmware_NRF` still ship
everything described above, so this section stays written in the present tense for them. The one
paragraph that is now wrong for the two swapped targets is the replay window: they no longer use
the ±32 window with a 64-entry seen-list. Upstream `Firmware` had already replaced it with a
256-bit backward bitmap and **no forward bound** (`src/nonce_window.h`), and that file was ported
verbatim as `shared/core/od_nonce_window.h`. A forward cap is not merely unnecessary there but
harmful — it strands a session permanently once a gap exceeds it, and it bounds nothing an
attacker cares about.

| # | Behaviour | Firmware | NRF54 | Silabs | Resolution |
|---|---|---|---|---|---|
| 6.1 | STEP-2 success frame | **19 bytes**: `[00][50][00][server_proof:16]` where proof = CMAC(session_key, server_nonce‖client_nonce‖device_id) (`encryption.cpp:626-641`) | identical (`opendisplay_pipe.c:714-724`) | identical | Spec documents only `[0x00][0x50][AUTH_STATUS_SUCCESS]` — the 16-byte mutual-auth proof is **undocumented**. Spec correction. |
| 6.2 | Session timeout basis | absolute from `session_start_time` (`checkEncryptionSessionTimeout`) | absolute, with a comment explaining why idle-based is wrong (`opendisplay_pipe.c:163-171`) | **idle-based** on `last_activity_ms` (`opendisplay_pipe.c:117`) — a continuously active session never expires | Firmware/NRF54 win; the NRF54 comment is the rationale. Fix Silabs on adoption. **RESOLVED 2026-08-15** as absolute, in the MILLISECOND domain, expiring on `elapsed >= timeout_ms`. Three bases shipped and the promotion took one piece from each argument, so state it precisely: Firmware's `>=` boundary, NRF54's ms domain, and neither's idle basis. Firmware divided BOTH timestamps by 1000 before subtracting (`encryption.cpp:266-267`), which is not wrap-safe — across the 49.7-day `uint32_t` rollover the current time restarts near 0 while the divided start stays ~4.29e6, so the age wraps huge and a healthy session is torn down. Taking NRF54's ms domain fixes that; taking Firmware's `>=` shifts NRF54 by 1 ms and Firmware by up to ~999 ms (its seconds floor could expire early). **Silabs' idle basis is a genuine security divergence, not a rounding one** — a continuously active session there never expires at all — and adopting absolute will change its behaviour outright. |
| 6.3 | Crypto backend | mbedTLS CMAC/CCM or CC310 (`#ifdef` arms, `encryption.cpp:15`) | PSA for CMAC/ECB/RNG, but **CCM hand-rolled** RFC 3610 over ECB (`opendisplay_pipe.c:282-405`) | same hand-rolled CCM | **RESOLVED 2026-08-15 for Firmware + NRF54** (`shared/hal/od_hal_crypto.h`). Both now use NATIVE CCM: mbedTLS on esp32-idf, `psa_aead_*` on nordic-zephyr. The hand-rolled RFC 3610 was not a missing capability — `CONFIG_PSA_WANT_ALG_CCM` was simply never set, while the Oberon and CRACEN drivers both implement it; enabling it costs **+2,320 B flash, +0 B RAM**. That code is deleted from the target and preserved verbatim as `tests/host/session_ccm_reference.inc`, the differential reference. `OD_CRYPTO_SOFT_CCM` is **deferred**, not adopted: with both targets native it would be a `shared/` source nothing compiles. Silabs still hand-rolls and is unchanged. |
| 6.4 | Legacy backend | — | — | — | `Firmware_NRF` uses Nordic `ocrypto_*` (`encryption.c:7-9`) — SDK-locked; irrelevant once the repo is dropped. |
| 6.5 | **Bidirectional nonce reuse** | present | present | present | **NOT FIXED, and not fixable here — see FOLLOWUPS.md § 5.** Inbound and outbound share one `session_id` and both counters start at 0, so the same `session_id‖counter` nonce is used in BOTH directions under one key. Catastrophic for CCM if a host ever decrypts with the same key in both directions. `od_session` reproduces it faithfully because changing it changes the wire; it needs directional key separation or a nonce-domain bit in the next protocol revision. |
| 6.6 | Replay at `diff == 0` | **accepted** (`encryption.cpp:178` skips the seen-scan when the difference is zero) | accepted (`:461`) | accepted (`:383`) | **FIXED** on both swapped targets. In a bitmap `d == 0` is simply bit 0, and every forward accept sets it, so there is no special case left to regress. A resend of identical sealed bytes is now `OD_SESSION_OPEN_REPLAY`. Host-compatible: py-opendisplay re-seals on every transmission including PIPE retransmits (`device.py:758-770`, pinned by `test_pipe_write_sender.py:532`), so no client depends on the old acceptance. |
| 6.7 | Window advance vs tag check | advances BEFORE decrypt (`encryption.cpp:740`, decrypt at `:763`) | advances before decrypt (`opendisplay_pipe.c:500`, decrypt at `:505`) | already split | **FIXED** on both swapped targets: `od_nonce_check()` is pure and `od_nonce_commit()` runs only after the tag verifies, so a forged frame at a high counter can no longer move `rx_last` and lock out the legitimate lower-counter frames still in flight. Commit still precedes the inner-length check — that frame carried a valid tag, so it is authentic and leaving it uncommitted would leave it replayable. |
| 6.8 | Inner length byte | permissive `<=` (`encryption.cpp:767`) | permissive (`opendisplay_pipe.c:509`) | permissive | **TIGHTENED** to exact (`decrypted[0] == decrypted_len - 1`) on both swapped targets. The permissive form accepts authenticated trailing bytes the caller never sees — harmless today because no producer emits them, but it is slack in a length field on the pre-auth-adjacent path. If a device ever refuses a legitimate frame with `OD_SESSION_OPEN_BAD_LENGTH`, this is the line to look at first. |
| 6.9 | `encryption_enabled` test | `== 1` (`encryption.cpp:232`) | `!= 0` (`opendisplay_pipe.c:165`) | `!= 0` | **NRF54's `!= 0` adopted — the one place this promotion overrides the authority target**, so it is recorded rather than assumed. The wire contract defines only 0 and 1, and the two readings fail in opposite directions: under `== 1` a single corrupted byte silently turns encryption **off** on a device whose config says it is on; under `!= 0` a corrupt byte can only fail closed, and the client still authenticates normally because it holds the key. Fail-safe wins on a security gate. `od_session_security_enabled()` also folds in the zero-key rule, which Nordic and Silabs never applied. |

## 7. Advertisement / MSD

No semantic divergence — one 16-byte `struct MsdAdvertisement` (canonical
`opendisplay_structs.h`), company id `0x2446` little-endian, dynamic sensor bytes,
temperature byte, battery-voltage low byte + 9th bit in status, 4-bit loop counter:

- Firmware builds via the canonical struct and dedupes identical payloads before
  re-advertising (`display_service.cpp:1734-1821`).
- Silabs packs the same `msd_payload[2..15]` into the 0xFF AD record and moves the 16-bit
  service UUID `0x2446` to the scan response for space (`opendisplay_ble.c:1705-1754`,
  company id at `:22`).
- NRF54 uses the same company id and GATT UUID (`opendisplay_ble.c:33, 308-309`).

Resolution: the 16-byte payload build (sensor encode, voltage clamp to 511×10 mV,
temperature offset+40 ×2, status bits) is pure logic → `shared/core`. AD-record packing and
re-advertise mechanics stay per target (NimBLE vs Zephyr `bt_le_adv_update_data` vs
`sl_bt_legacy_advertiser_set_data` differ in whether the name fits the ADV PDU).

### 7a. BLE address derivation — a toolchain divergence, not a code one

**The one divergence in this document that no amount of reading the source could find.** It is
not in any repo's code; it is in a vendored binary framework's sdkconfig, and it decides the
device's identity to Home Assistant.

`CONFIG_<CHIP>_UNIVERSAL_MAC_ADDRESSES` sets how many interfaces get an IEEE-assigned MAC
derived from the factory base MAC, and therefore where Bluetooth lands:

| | STA | AP | BT | ETH |
|---|---|---|---|---|
| `FOUR` | base+0 | base+1 | **base+2** | base+3 |
| `TWO` | base+0 | local (U/L bit) | **base+1** | local |

HA's Bluetooth integration keys devices on the BLE address, so the offset *is* the fleet's
identity. What each build shipped:

| Chip | pioarduino (the fleet) | ESP-IDF default | Now pinned to |
|---|---|---|---|
| esp32s3 | **2** | 4 | **2** |
| esp32 | 4 | 4 | 4 |
| esp32c3 | 4 | 4 | 4 |
| esp32c6 | 4 | 4 | 4 |

**Discovered on hardware, 2026-08-03.** The S3 reappeared in Home Assistant as a brand-new
device after the ESP-IDF port. Base MAC `44:1b:f6:85:b1:b8`; the IDF build advertised
`...b1:ba` (base+2) against the fleet's `...b1:b9` (base+1). The device *name* was identical
throughout — `getChipIdHex()` reads the **base** MAC (`encryption.cpp:786`, `ESP.getEfuseMac()
>> 24`), which this setting does not touch — so name matched, address did not, and it read as a
stranger rather than as a fault.

**Why the S3 differs has no known rationale.** The one distinction that correlates — only the
classic ESP32 has an internal EMAC, so only it has a real fourth consumer — does not hold: the
C3 has the same profile as the S3 (SPI Ethernet only, whose controllers carry their own MACs)
and pioarduino gave it FOUR. Most likely an inconsistency in arduino-esp32's packaging.
Ruled out: IDF drift (`git log -p` on the chip `Kconfig.mac` files shows FOUR as the only
default ever set, so arduino's value is not a stale capture) and a smaller Espressif allocation
for the S3 (IDF's docs state every chip ships enough universal addresses for all internal
interfaces, and the knob is documented as being for *custom* MAC ranges — with a factory base
MAC either value is safe on any chip).

**Resolution: pin per chip to what shipped, in `sdkconfig.defaults.<chip>`, including where it
already equals IDF's default.** Fleet compatibility means matching observed behaviour, not a
rationale. Three of the four pins change nothing today and exist so an IDF upgrade cannot move
a fleet's addresses the way this one just did. The symbol cannot go in the common
`sdkconfig.defaults`: it is chip-prefixed, and the generic
`CONFIG_ESP_MAC_UNIVERSAL_MAC_ADDRESSES_*` is a bare bool with no prompt that the chip choice
`select`s.

### 7b. NimBLE host-stack log level — the same inherited-default trap

Second instance of 7a's mechanism, found the same week. The stack logs
`BLE_HS_LOG(INFO, ...)` from inside itself — `"GATT procedure initiated: notify"` plus
`att_handle=` on **every notify** (`ble_gattc.c:629`), and a five-line block per
advertise/stop-advertise (`ble_gap.c:3972`, `:4274`). A 414-chunk `PIPE_WRITE` emitted roughly
800 extra UART lines.

**It is not the NimBLE setting** — `CONFIG_BT_NIMBLE_LOG_LEVEL` is INFO in both builds. It is
the global ceiling:

| | pioarduino | ESP-IDF default | Ours before | Ours now |
|---|---|---|---|---|
| `CONFIG_LOG_DEFAULT_LEVEL` | 1 (ERROR) | 3 (INFO) | 3 | 3 |
| `CONFIG_LOG_MAXIMUM_LEVEL` | 1 (ERROR) | 3 (INFO) | 3 | 3 |
| `CONFIG_BT_NIMBLE_LOG_LEVEL` | 1 (INFO) | 1 (INFO) | 1 (INFO) | **2 (WARNING)** |

`MAXIMUM_LEVEL` is a compile-time ceiling, so at ERROR the preprocessor strips every `ESP_LOGI`
in the firmware — NimBLE's included. Arduino set it to ERROR; IDF defaults to INFO; this project
never named the symbol.

The cost that mattered was not noise. `od_log` takes a mutex and writes to UART0 while `ESP_LOG`
writes to the same UART with **no shared lock**, so records interleaved mid-line:

```
ERX 0x0040 (31 B): 00 40 C8 19 ... 27 B6 00I (137013) NimBLE: GATT procedure initiated: notify;
```

Every hex dump in a capture taken that way is suspect, which cost real debugging time during the
dark-panel investigation.

**Resolution: `CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y`, and deliberately NOT matching Arduino's
global ERROR ceiling.** Lowering the ceiling would also strip *our* useful `ESP_LOGI` lines —
`od_ble`'s resolved identity-address type and GATT `val_handle`, and the `bbep` BUSY-wait timeout
warning that made the dark-panel diagnosis possible. Silence the noisy component, not the whole
log. Verified by symbol inspection rather than config: the three NimBLE format strings are absent
from the linked image and the three of ours are present. 5 KB of flash reclaimed on the S3.

**The two-writers-on-one-UART problem is NOT fixed** and is now the remaining cause of corrupted
log lines. `od_log`'s mutex does not and cannot cover `ESP_LOG`. Either route both through one
sink or accept that any `ESP_LOG` at a visible level can splice a line. Worth doing before the
next hardware capture is trusted.

**The general lesson, which applies to the whole migration.** `docs/TOOLCHAINS.md`'s
PlatformIO-knob → sdkconfig translation table was built from `platformio.ini`'s `build_flags` —
i.e. from what the project *set*. This is a wire-visible behaviour the project silently
*inherited* from a precompiled framework sdkconfig it never saw, and now inherits differently.
Nothing in `Firmware`'s tree records that BT = base+1. The remaining ~1500 symbols in those
Arduino sdkconfigs are unaudited on the same basis; when the nRF52840 half moves to Zephyr,
expect the same class of surprise.

If a board ever needs FOUR *and* fleet-compatible BLE, `esp_iface_mac_addr_set(mac,
ESP_MAC_BT)` overrides one interface at runtime — but it must be called before `od_ble_init()`,
and it puts one wire fact in two places, so prefer the Kconfig pin.

## 8. Protocol-header state (report only — headers frozen)

Verified with `sync_protocol_header.py --check` and `git show`:

1. Canonical is 2.2 + an unreleased `OD_BLE_MAX_FRAME` addition. The vendored **protocol**
   copies in NRF54 / Silabs / NRF are one push behind (missing `OD_BLE_MAX_FRAME`);
   `Firmware` local and `Firmware_Unified/shared/protocol/` match canonical.
2. ~~**`upstream/main` of `Firmware` — the true ESP32 import source — is at 2.1**, missing
   all of 2.2 (SECTION 9 LAN) and the unreleased entries.~~ **WRONG — corrected 2026-07-25 at
   the Phase A import.** `upstream/main` is at **2.2**, and necessarily so: `#124`
   (`2e2131b`) *is* the LAN feature that SECTION 9 documents. Measured at import, its
   `include/opendisplay_protocol.h` differs from canonical by **ten lines, all inside a
   doc-only changelog entry** (the `0x43` trailing-patch-byte clarification), with no wire
   difference and no version difference. The import therefore did not land a stale header and
   nothing needed re-syncing. The migration risk built on this claim is void.
3. `upstream/main` hand-edited `include/opendisplay_structs.h` in PR #120 — three
   comment-only Seeed_GFX→FastEPD wording changes (PanelIC `@external` note, ids 3000/3001
   doc strings, `dc_pin` doc). No wire change. **Canonical still carries the old wording**,
   so the required sequence is: apply the FastEPD wording to the canonical structs header
   first, then `--push` — a bare `--push` today would *revert* upstream's edit.
4. `Firmware_Silabs` has a second, unmaintained `include/opendisplay_protocol.h` stale at
   2.1; nothing compiles it today only by include-path order. Do not carry it into
   `targets/efr32bg22-slc/` (see `targets/efr32bg22-slc/README.md`).
5. The sync tool's copy map still lacks `Firmware_Unified/shared/protocol/`.

Spec corrections accumulated from the matrix (all backward-looking documentation of shipped
behaviour, i.e. no-bump or MINOR under the header's own policy): 1.1 auth-required shape,
1.2 decrypt-failure shape, 1.4 reserved ack padding, 0x43's `sha_len` byte, 6.1 the STEP-2
server proof, 3.2 auto-complete, `RESP_PARTIAL_WRITE_START`, a PIPE-START "unsupported"
error code, the 0x45 `@targets` fix (or a Silabs implementation), and the **0x52 NACK-width
self-contradiction** — `@response` says `[FF][52][00][00]`, `@targets` says `[FF][52][00]`;
Silabs ships four bytes, so `@targets` is the line to correct (FOLLOWUPS.md § 2.1).

**One queued change is *not* backward-looking — it changes behaviour and must be sequenced
deliberately:** `MAX_CONFIG_CHUNKS` **20 → 21**, so that the 4096 `MAX_CONFIG_SIZE` ceiling is
actually transferable rather than capped at 4000 by the chunk count (decided 2026-07-25;
FOLLOWUPS.md § 3.1). It is a relaxation — a device accepts one additional chunk — so old hosts
are unaffected and it is a MINOR bump. But unlike everything above it, firmware must change to
match, and deployed units keep enforcing 20 until they are reflashed.

## 9. LAN / WiFi transport — `Firmware`/ESP32 only (`#124`)

New surface as of `2e2131b`. No other target has a second transport, so there is nothing to
diverge *from* yet — these rows exist to stop the ESP32's choices being adopted as universal by
default when `shared/core` gains an origin-aware dispatcher.

| # | Behaviour | Firmware / ESP32 | Others | Resolution for `shared/core` |
|---|---|---|---|---|
| 9.1 | Connection model | **The device is the server; the host connects to it.** It binds `WifiConfig.server_port` (default `OD_LAN_TCP_PORT` 2446 when 0) and listens | n/a — but **BLE is the same shape**: device is the peripheral, host is the central and initiates | **This is consistent across transports, not a LAN peculiarity.** On both BLE and LAN the device is passive and the host initiates, so `od_hal_radio` is uniformly passive: the transport delivers frames, the core replies on the same origin, and the core never initiates a connection on any transport. Do not model LAN as an exception. |
| 9.2 | `WifiConfig.server_host` | **DEPRECATED** (2026-07-25). Gates nothing and is not read. A leftover from an abandoned "tag pushes to an upload server" model that the device-as-server design never used; the name misleads, since `server_port` is the port *this device* binds | n/a | **Deprecated, not removed.** The 64 bytes stay in the `0x26` wire format at their current offset — deleting a field or shifting `server_port` would break the config-binary backward-compatibility rule (ARCHITECTURE.md) on shipped devices. Rules: firmware **must ignore** it; `py-opendisplay` and the HA config flow should stop writing it and must not surface it as a setting; new firmware must not repurpose the bytes (a deployed host may still send a non-zero value, so they are not free space — unlike `reserved[]`, which is contractually zero). Mark `@deprecated` in the canonical header when the freeze lifts. |
| 9.3 | TLS-PSK port | **derived, not configured**: `server_port + 1`, with no config entry (`OD_LAN_TLS_PORT` in the header agrees) | n/a | Derived ports are a wire contract as much as a stored one. `shared/core` must expose the derivation, not re-invent it per target. |
| 9.4 | PIPE over LAN | **not available** — "NO PIPE ON LAN" (`opendisplay_protocol.h`) | n/a | PIPE is BLE-only *by protocol*, not by capability. So `OD_PIPE_ENABLE` is necessary but not sufficient: the dispatcher must also refuse 0x80-0x82 on a LAN origin, on a target that supports both. |
| 9.5 | Credential logging | deliberately never logs SSID or password verbatim — presence and length only (`config_parser.cpp`, `#124`) | *unverified elsewhere* | **Now a rule**, not a convention — ARCHITECTURE.md § "Secrets are never logged verbatim". Applies to `shared/` at every log level and covers `0x26`, `0x27`, keys, nonces, MACs and raw TLV payloads. `Firmware`'s practice is the model; audit the other targets during promotion, since none is verified. |

**What generalises well:** the device is passive on every transport — BLE peripheral, LAN server
— and the host always initiates. `od_hal_radio` can therefore be uniformly passive with no
transport-specific initiation path, which is a simplification worth stating rather than
rediscovering.

**What generalises badly:** origin is already an explicit argument of frame RX in the proposed
core API (§1.6, SHARED_API_DESIGN.md) — good. But 9.4 shows origin also gates *which opcodes are
legal*, which the current design treats purely as a CCM-policy input. Extend it: origin selects
the reply transport, the encryption policy, **and** the permitted opcode set.
