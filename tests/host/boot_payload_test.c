#include "od_boot_payload.h"

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

    CHECK(od_boot_url_build(0x1234u, 0xabcdefu, key, true, 0x7856u, url, sizeof(url)));
    CHECK(strcmp(url, "https://opendisplay.org/l/?EjSrze8AESIzRFVmd4iZqrvM3e7_eFY") == 0);
    CHECK(!od_boot_url_build(0x1234u, 0xabcdefu, key, true, 0x7856u, line, 8u));

    if (failures != 0) {
        return 1;
    }
    puts("boot_payload: ok");
    return 0;
}
