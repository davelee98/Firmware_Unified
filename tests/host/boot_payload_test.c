#include "od_boot_payload.h"

#include "opendisplay_structs.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

int main(void)
{
    static const uint8_t key[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    uint8_t zero[16] = {0};
    uint8_t payload[OD_BOOT_PAYLOAD_SIZE];
    char display[OD_BOOT_KEY_HEX_SIZE];
    char line[32];
    char url[OD_BOOT_URL_SIZE];

    od_boot_payload_build(0x1234u, 0xabcdefu, key, true, 0x7856u, payload);
    CHECK(payload[0] == 0x12u && payload[1] == 0x34u);
    CHECK(payload[2] == 0xabu && payload[3] == 0xcdu && payload[4] == 0xefu);
    CHECK(memcmp(&payload[5], key, sizeof(key)) == 0);
    CHECK(payload[21] == 0x78u && payload[22] == 0x56u);

    od_boot_payload_build(0x1234u, 0xabcdefu, key, false, 0x7856u, payload);
    CHECK(memcmp(&payload[5], zero, sizeof(zero)) == 0);

    od_boot_format_key_display(zero, true, display);
    CHECK(strcmp(display, "--------------------------------") == 0);
    od_boot_format_key_display(key, false, display);
    CHECK(strcmp(display, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX") == 0);
    od_boot_format_key_display(key, true, display);
    CHECK(strcmp(display, "00112233445566778899AABBCCDDEEFF") == 0);

    od_boot_format_key_line("KEY1:", key, 8u, false, true, line, sizeof(line));
    CHECK(strcmp(line, "KEY1: 0011223344556677") == 0);
    od_boot_format_key_line("KEY1:", key, 8u, false, false, line, sizeof(line));
    CHECK(strcmp(line, "KEY1: hidden") == 0);
    od_boot_format_key_line("KEY1:", key, 8u, true, true, line, sizeof(line));
    CHECK(strcmp(line, "KEY1: not set") == 0);

    /* The key-state tree, every input combination, no gaps. Two inputs decide whether a key is
     * in force and the third only chooses between HIDDEN and SHOWN, so the show flag must not
     * reach the answer while encryption is off or the key is absent. */
    {
        static const struct {
            uint8_t enabled;
            bool key_set;
            uint8_t flags;
            enum od_boot_key_state want;
        } cases[] = {
            {0u, false, 0u,                                   OD_BOOT_KEY_NOT_SET},
            {0u, false, OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_NOT_SET},
            {0u, true,  0u,                                   OD_BOOT_KEY_NOT_SET},
            {0u, true,  OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_NOT_SET},
            {1u, false, 0u,                                   OD_BOOT_KEY_NOT_SET},
            {1u, false, OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_NOT_SET},
            {1u, true,  0u,                                   OD_BOOT_KEY_HIDDEN},
            {1u, true,  OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN,   OD_BOOT_KEY_SHOWN},
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            struct SecurityConfig sec;
            memset(&sec, 0, sizeof(sec));
            sec.encryption_enabled = cases[i].enabled;
            sec.flags = cases[i].flags;
            if (cases[i].key_set) {
                memcpy(sec.encryption_key, key, sizeof(sec.encryption_key));
            }
            if (od_boot_key_state(&sec) != cases[i].want) {
                fprintf(stderr, "FAIL key_state case %u: enabled=%u key_set=%d flags=0x%02X\n",
                        i, cases[i].enabled, (int)cases[i].key_set, cases[i].flags);
                ++failures;
            }
        }
        /* An absent config is a device with nothing configured, not a crash. */
        CHECK(od_boot_key_state(NULL) == OD_BOOT_KEY_NOT_SET);
        /* One non-zero byte is a key. The zero test is over the whole 16, not a prefix. */
        {
            struct SecurityConfig sec;
            memset(&sec, 0, sizeof(sec));
            sec.encryption_enabled = 1u;
            sec.encryption_key[15] = 0x01u;
            CHECK(od_boot_key_state(&sec) == OD_BOOT_KEY_HIDDEN);
        }
    }

    CHECK(od_boot_url_build(0x1234u, 0xabcdefu, key, true, 0x7856u, url, sizeof(url)));
    CHECK(strcmp(url, "https://opendisplay.org/l/?EjSrze8AESIzRFVmd4iZqrvM3e7_eFY") == 0);
    CHECK(!od_boot_url_build(0x1234u, 0xabcdefu, key, true, 0x7856u, line, 8u));

    if (failures != 0) {
        return 1;
    }
    puts("boot_payload: ok");
    return 0;
}
