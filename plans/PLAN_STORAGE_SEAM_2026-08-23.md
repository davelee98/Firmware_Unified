# Config-storage seam — design

**Status:** proposed, 2026-08-23. Step 3 of
[PLAN_DEDUP_OUTSTANDING_2026-08-22.md](PLAN_DEDUP_OUTSTANDING_2026-08-22.md) § 8 — the written
design that row requires before any code. No implementation is in this document.

**What it unblocks:** the config-storage promotion in that plan's § 4 (three `0xEDB88320` CRC-32
loops, three `0xDEADBEEF` record headers, ~450 lines). It does **not** unblock the sensor work,
which needs a different seam — see `PLAN_SENSOR_SEAM_2026-08-23.md`.

---

## 1. What is already common, and what is not

All three targets export the identical five-function API and the same record struct:

```c
bool     initConfigStorage(void);
bool     saveConfig(uint8_t *config_data, uint32_t len);
bool     loadConfig(uint8_t *config_data, uint32_t *len);
bool     clearStoredConfig(void);
uint32_t calculateConfigCRC(uint8_t *data, uint32_t len);

typedef struct { uint32_t magic, version, crc, data_len; uint8_t data[MAX_CONFIG_SIZE]; }
        opendisplay_config_storage_t;
```

`0xDEADBEEF` / version 1 / CRC-32 over the payload / 16-byte header, on every target. **The
framing is already uniform** — that is what makes this promotable at all.

What differs is entirely below it:

| | ESP32 | nordic-zephyr | efr32bg22-slc |
|---|---|---|---|
| Medium | `od_hal_nvs` over IDF NVS | Zephyr `settings` | NVM3 |
| Access | whole blob in, whole blob out | whole record, plus a RAM cache | **partial read at offset** (`nvm3_readPartialData`) |
| Staging | `static uint8_t blob[16 + 4096]` | `static opendisplay_config_storage_t rec` (~4 KB) | **none** — a union overlays the assembler |
| Extra | `od_hal_nvs_secure_erase()` | commit-cache-after-write ordering | header read separately from payload |

## 2. The constraint that decides the shape

`targets/esp32-idf/src/config_parser.cpp:110-117` already states it, and the promotion must obey
its own warning rather than re-discover it:

> The staging buffer is the cost of the blob interface. It is affordable here — an ESP32-S3 has
> 512 KB plus PSRAM — and it is NOT affordable on the EFR32BG22, whose whole heap is 10.3 KB.
> When this subsystem is promoted to `shared/core`, that target will need either a two-key record
> or a streaming write; do not carry this buffer across as if it were free.

BG22's answer today is sharper than either option that comment names: `od_config_work_t`
(`opendisplay_config_storage.c:33-41`) is a **union of `struct od_config_asm` and the storage
record**, statically asserted to put both byte arrays at offset 16 and to be the same size. During
a write the four assembler state words *become* the record header. That is why BG22 needs no
staging buffer at all, and it is a property of the target's memory budget — 2,064 bytes on a 32 KB
part — not an accident to normalise away.

**So a whole-blob seam is disqualified.** Any shared implementation that says "give me the whole
record" forces BG22 to find a second 2 KB, which it does not have.

## 3. Decisions

### S1 — The seam is partial, offsets are the interface, and the workspace is the caller's

```c
int od_hal_nvs_init (void);
int od_hal_nvs_size (uint32_t *len_out);                        /* stored size, or ENOENT */
int od_hal_nvs_read (uint32_t offset, void *buf, uint32_t len);
int od_hal_nvs_write(const void *record, uint32_t len);         /* ONE contiguous span */
int od_hal_nvs_erase(void);
```

Read takes an offset because BG22 reads the 16-byte header, validates it, and only then reads the
payload — a sequence that costs it nothing and that a whole-record read would force it to abandon.

**Write takes ONE span, not a header pointer plus a payload pointer.** The first draft split them,
which advertised an input BG22 cannot serve: its single `nvm3_writeData` works only because the
union puts header and payload adjacent, and two unrelated pointers would force exactly the copy
this design exists to avoid. So the contract is inverted — the **caller supplies a contiguous
workspace** with the header at offset 0 and the payload at offset 16, and `shared/` fills it in
place. BG22 points that at its union; ESP32 and Nordic point it at the staging buffer they already
keep. `_Static_assert` ties the workspace layout to the record.

### S1a — Failure semantics are part of the seam, not left to each implementation

The first draft named return codes without saying what they mean, and the three targets already
disagree — Nordic ignores `settings_delete()`'s result, clears its cache and returns success
(`opendisplay_config_storage.c:102-105`), where the others report the medium's answer.

| Condition | Code | Contract |
|---|---|---|
| No stored record | `OD_HAL_NVS_ENOENT` | Not an error. An empty device boots on defaults |
| Caller's buffer too small | `OD_HAL_NVS_E2BIG` | Nothing copied; `len_out` still reports the stored size |
| Medium failure | `OD_HAL_NVS_EIO` | Read: nothing valid returned. Write: **the previous record must remain readable** |
| Erase failure | `OD_HAL_NVS_EIO` | The record may still be present, and any cache **must not** be cleared — reporting success while the medium still holds the old config is how a "cleared" device comes back configured |
| Corrupt record | not the HAL's | Magic, length and CRC are `shared/`'s (S2). The HAL returns bytes; it does not judge them |

Nordic's current erase behaviour is a **divergence to fix**, not to preserve: it is the one target
where a failed clear reports success. Record it in `DIVERGENCE_MATRIX` with the cutover.

### S2 — `shared/` owns framing and CRC; the HAL owns bytes

Magic, length bounds, CRC-32 computation and verification, and the ordering rules move to
`shared/core/od_config_store.c`. The HAL sees an opaque region and offsets. This is what
`targets/esp32-idf/hal/od_hal_nvs.h:8-10` already declares its intent to be — "The HAL stores an
OPAQUE BLOB. Record framing … is the core's business" — so the promotion is the repoint that
header was written for.

**`version` stays reserved and unchecked.** All three targets write 1; **none reads it back or
rejects anything** — verified across all three implementations. The first draft said version
verification moves into shared code, which would have introduced a *new* rejection: a device
carrying a record this firmware did not write would stop booting on its stored config. That is a
behaviour change smuggled in under a refactor.

So the shared core carries the field, writes 1, and ignores it on read — identical to today. If a
version ever needs to mean something, that is a deliberate change with a compatibility story and a
`DIVERGENCE_MATRIX` row, not a side effect of promotion. A host test pins the current behaviour:
a record with version 2 and a valid CRC still loads.

The CRC-32 becomes `od_config_store_crc32()` in `shared/`. It is bit-serial today on all three;
keep it bit-serial. A table costs 1 KB of flash on a part with 480 B of RAM headroom, and this
runs once per config write.

### S3 — The cache is the target's, not the core's

Nordic keeps a RAM copy and commits it **only after** a successful write, so a failed save keeps
reporting the last persisted config. That ordering is correct and must survive. But it is a
Zephyr-settings response to a slow backing store, not a property of the record — ESP32 and BG22
have no cache and want none.

So caching lives behind the HAL. `od_hal_nvs_read()` may serve from a cache; the core may not
assume one exists. The core's contract is: after `od_config_store_save()` returns true, a
subsequent load returns those bytes.

### S4 — Secure erase moves to its own ESP32 header

`od_hal_nvs_secure_erase()` has one implementation and one caller. It cannot stay declared in
`od_hal_nvs.h` once that header is canonical in `shared/hal/`: a shared header carrying a function
only one target defines is either a link error for the others or dead weight, and the "one target
keeps a private copy of the canonical header" arrangement is the thing the promotion removes.

So it moves to `targets/esp32-idf/hal/od_hal_nvs_secure.h`, declared and implemented there, called
only from ESP32 code. `shared/` never names it. If a second target needs it, that is the moment to
widen the shared seam.

### S5 — One size constant, not two

**Found while scoping this, and not in the survey.** `MAX_CONFIG_SIZE` is defined independently in
three target headers — `nordic-zephyr/src/opendisplay_config_storage.h:18` (4096),
`esp32-idf/src/config_parser.h:7` (4096), `efr32bg22-slc/opendisplay_config_storage.h:9` (2048) —
**alongside** `OD_CONFIG_MAX_SIZE`, which `shared/profiles.cmake` sets for the same purpose.

Today they agree by maintenance, not by construction. Change the profile and BG22's static asserts
catch the divergence; ESP32 and Nordic have nothing that would. The promotion collapses them:
`OD_CONFIG_MAX_SIZE` is the constant, `MAX_CONFIG_SIZE` disappears, and the record struct derives
from it. A `_Static_assert` ties the record size to the assembler's, as BG22 already does.

### S6 — BG22's overlay is preserved, not normalised

The union stays, and stays statically asserted. What moves to `shared/` is the framing that writes
*into* it. The shared save path takes a caller-supplied header struct and a payload span; BG22
points both at its union, and the other two point them wherever they already do.

This is the same shape as the transfer promotions: shared state machine, target-owned buffer.

## 4. What the promotion must not change

- The **on-medium record**: `0xDEADBEEF`, version 1, 16-byte header, CRC-32 over the payload,
  little-endian. ESP32 units flashed since the NVS cutover carry it, and any BG22 written by a
  test image does — that target has never shipped, so this is format continuity for images, not a
  deployed fleet.
- **BG22 refuses rather than truncates** an over-size config (`OD_CONFIG_MAX_SIZE` = 2048 there).
  A cap a host cannot query has to be loud; that is `MEMORY_CONSTRAINTS.md` item 3.
- **Nordic's commit-after-write ordering** (S3).
- **ESP32's two-write staging** producing byte-identical bytes to the old single write.

## 5. Staging

1. **`od_hal_nvs.h` moves to `shared/hal/`** with the S1 signatures, and secure erase moves to
   `od_hal_nvs_secure.h` (S4). ESP32's existing implementation is repointed; Nordic and BG22 grow
   one. No shared caller yet — the build proves it **compiles**; an uncalled function is discarded
   by the linker, so "it links" would prove nothing about cost.
2. **`shared/core/od_config_store.{c,h}`** with framing, CRC and bounds. Dormant, both capability
   arms, host-tested against a fake medium.
3. **S5's constant collapse**, on its own, so a size regression is bisectable.
4. **Per-target cutover, one target per commit**, in the order ESP32 → Nordic → BG22. BG22 last
   because its overlay is the tightest constraint and the other two prove the seam first.
5. **Delete the three CRC loops and three headers** as their last callers disappear (X1).
6. **Ratchet**: no second CRC-32 table or loop outside `shared/`, and no target-side
   `CONFIG_STORAGE_MAGIC`.

## 6. Hardware gate

Per-target and mandatory; a config the device cannot read back is a bricked configuration.

- [ ] **ESP32:** write a config, reboot, confirm it reloads and the panel renders from it.
- [ ] **ESP32:** factory-provisioned config still loads at first boot (the flash-pointer path that
      avoids the 4 KB copy).
- [ ] **Nordic:** write, reload in place, reboot-persist — the three the 2026-08-19 run covered.
- [ ] **Nordic:** a failed write leaves the previous config readable (S3's ordering). Inducing a
      `settings` write failure on the bench is not obviously possible — if it cannot be, this
      belongs in a production-source fault test and the row says so rather than sitting open
      forever.
- [ ] **Each target:** a stored record with an unrecognised `version` and a valid CRC still loads
      (S2 — the behaviour that must NOT change).
- [ ] **BG22:** write, reload, reboot-persist, and an over-size declaration refused at the start
      frame with nothing stored.
- [ ] **BG22:** RAM unchanged — `heap_size` and `data + bss` against the pre-change image. The
      overlay existing is the reason this promotion is affordable at all; losing it would be
      invisible except here.
- [ ] **Each target:** a corrupted stored record (flip one payload byte out of band) fails CRC and
      the device boots on defaults rather than on garbage.

## 7. Open questions

1. **Does Nordic's `settings` backend support a partial read?** If not, its `od_hal_nvs_read()`
   serves offsets from the record it already caches, which is acceptable but means Nordic keeps a
   ~4 KB static the other two do not. Confirm before step 1 rather than after.
2. **Is `od_hal_nvs_size()` cheap on all three?** BG22 has `nvm3_getObjectInfo`; ESP32 NVS and
   Zephyr settings may require reading to discover length, which would make the header-first
   sequence pointless there. It is still correct — just not a saving.
3. **Two-key record for BG22?** Rejected in this design because the union already solves it, but
   if a future config exceeds what the overlay can carry, the split is the fallback and the record
   format changes — a wire-adjacent decision, not a refactor.
