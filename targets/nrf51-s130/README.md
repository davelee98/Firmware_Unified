# nRF51822 + S130 slim target

Experimental firmware for the GreenTags LT213A price tag: nRF51822, S130 2.0.1 and the fixed
104 × 212 monochrome panel. It preserves the ordinary OpenDisplay command frames while limiting
notifications to the ATT-MTU-23 payload of 20 bytes. Commands larger than 20 bytes use standard
ATT Prepare/Execute Write.

This target supports config/version/MSD discovery and uncompressed full-frame direct writes. Its
configuration is immutable and capped at 1 KiB in the generated-source contract; the current blob
is 133 bytes. Authentication, pairing, config writes, compression, partial update, PIPE, NFC, DFU,
LED, buzzer and generic panel selection are intentionally absent.

The MSD temperature and liveness fields are live. Battery voltage currently reports the literal
wire value 0.00 V because the nRF51 VDD ADC path has not been qualified on this tag; the protocol
has no separate unavailable-voltage encoding.

## Build

Use GNU Arm Embedded and Nordic nRF5 SDK 12.3.0:

```sh
export NRF5_SDK_ROOT=/opt/nRF5_SDK_12.3.0_d7731ad
./targets/nrf51-s130/build.sh
```

The application HEX begins at `0x1B000`; flash it only after installing S130 2.0.1. To produce a
merged SWD image, set `S130_HEX` to Nordic's separately downloaded
`s130_nrf51_2.0.1_softdevice.hex`. The build refuses any file whose SHA-256 is not
`17ccba6573c51b386ccacae96d2eb428162737ef1cbe2f38059fd44e224078e7`.

The factory BLE address is retained. Clear the operating system and client GATT cache after
replacing the tag's original firmware; this preserves the existing device identity but changes its
attribute table.

No hardware gate has passed merely because this target builds. The NOBUF+AUTH queued-write path,
panel waveform/trace, reconnect behavior, watchdog timing, current and 100-upload soak all require
the exact tag.

## Provenance

`panel/lt213a_panel.c` derives from the monochrome OTP/full-refresh path in
`shirok1/greentags-lt213a-opendisplay` at commit
`28df8b6ce1506b82665809763dc92180b616e916`, retained under GPL-3.0-or-later. The import contains
only the fixed LT213A panel sequence; Nordic SDK 12.3.0 and S130 2.0.1 remain separately supplied
build inputs and are not redistributed here.
