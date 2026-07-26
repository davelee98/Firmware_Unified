# compat/ — scaffolding with a scheduled demolition

This directory exists to be deleted.

`arduino_compat.h` (added in phase B) maps the Arduino surface the imported sources use onto
ESP-IDF — `String` onto a minimal `std::string` wrapper, `pinMode`/`digitalWrite`/`delay`/
`millis`/`delayMicroseconds` onto their IDF equivalents. The full census, with call-site counts,
is in [../../../docs/TOOLCHAINS.md](../../../docs/TOOLCHAINS.md) § "Arduino API census".

Its purpose is to get the target to **link and boot on hardware early**, so that every later
step is bisectable against a known-good baseline. It is not a portability layer, and nothing
new should ever be written against it.

## The ratchet

`ratchet.sh` counts the files that include the shim and compares that to `SHIM_BUDGET`. The
count may only go **down**. Run it locally before proposing a change here:

```bash
./targets/esp32-idf/compat/ratchet.sh
```

It fails in both directions on purpose:

| State | Result |
|---|---|
| count > budget | **fail** — the shim grew; route the dependency through `shared/hal` or write the IDF call |
| count < budget | **fail** — lower `SHIM_BUDGET` in the same commit; a stale budget is slack, and slack is how a ratchet stops ratcheting |
| count == budget | pass |

## Why it was built before the thing it polices

MIGRATION.md names the shim outliving its purpose as the biggest risk in the plan, and
prescribes "a mechanical ratchet rather than good intentions". DESIGN_REVIEW § "Likely pitfalls"
adds the prediction that matters: *if the ratchet is not built before phase B starts, it will be
built never* — because once the shim is load-bearing, adding a check that forbids using it is a
fight nobody wants to have. So it was built during phase A, with a budget of `0`, against a shim
that does not exist yet.

The practical consequence: the first phase-B commit that includes `arduino_compat.h` **will fail
CI** until `SHIM_BUDGET` is raised deliberately. That is the forcing function, not a defect —
it makes the shim's size a number someone has to type, look at, and justify.

## When phase C finishes

When `SHIM_BUDGET` reaches `0` and `arduino_compat.h` is gone, delete:

- this directory, and
- `.github/workflows/esp32-shim-ratchet.yml`

together, in one commit. A check that can no longer fail is noise, and noise is how the checks
that matter get ignored. **If the shim is still here when the last subsystem lands, the port is
not done** — that is the whole point of counting.
