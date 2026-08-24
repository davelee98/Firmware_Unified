# Per-target compile-time profile for shared/ — ONE definition, every consumer.
#
# These macros change the ABI of shared structures: OD_CONFIG_WITH_* #if-gate members of
# struct od_config, OD_CONFIG_MAX_SIZE sizes struct od_config_asm, and OD_TXQ_SLOTS and the
# OD_CAP_* switches size or remove state. A host archive built with a different set is a
# different layout, so a suite compiled that way validates something the firmware does not run —
# it passes, and proves nothing.
#
# So the firmware build and every host archive that claims to represent a target read the list
# from here. A consumer adds only definitions that are genuinely its own (a test-only knob, a
# board flag); it never restates one of these.
#
# Included by shared/sources.cmake, so anything already consuming the source list has these.

# EFR32BG22: 32 KB RAM, no kernel, no PIPE, no partial transfer, no RX ring, single chip select,
# no logging, and no touch/buzzer/Wi-Fi/extended-data hardware.
set(OD_PROFILE_SILABS
    OD_CONFIG_MAX_SIZE=2048u
    OD_TXQ_SLOTS=3u
    OD_CAP_PIPE=0
    OD_CAP_PARTIAL=0
    OD_CAP_RXQ=0
    OD_CAP_DUAL_CS=0
    OD_CAP_LOG=0
    OD_CONFIG_WITH_TOUCH=0
    # Product decision 2026-08-24: this target will never carry a buzzer.
    # DIVERGENCE_MATRIX 16; ratcheted by check.sh "silabs: BG22 has no buzzer runner".
    OD_CONFIG_WITH_BUZZER=0
    OD_CONFIG_WITH_WIFI=0
    OD_CONFIG_WITH_DATA_EXTENDED=0)
