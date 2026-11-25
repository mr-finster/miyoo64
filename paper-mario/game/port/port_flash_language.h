#ifndef PM64_PORT_FLASH_LANGUAGE_H
#define PM64_PORT_FLASH_LANGUAGE_H
#include <stdint.h>
#include <string.h>

/* fio_save_globals computes with crc1=0/crc2=-1; replacing those with sum/~sum
 * leaves the signed 32-bit total unchanged. */
static inline int port_flash_globals_language(const uint8_t raw[0x80]) {
    static const char magic[16] = "Mario Story 006";
    int32_t sum = 0, crc1, crc2;
    uint32_t language;
    if (memcmp(raw, magic, sizeof magic) != 0) return -1;
    for (unsigned i = 0; i < 0x80; i += 4) {
        int32_t word;
        memcpy(&word, raw + i, sizeof word);
        sum += word;
    }
    memcpy(&crc1, raw + 0x30, sizeof crc1);
    memcpy(&crc2, raw + 0x34, sizeof crc2);
    memcpy(&language, raw + 0x40, sizeof language);
    return crc1 == ~crc2 && sum == crc1 && language < 4 ? (int) language : -1;
}
#endif
