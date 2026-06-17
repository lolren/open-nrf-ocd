/* test_elf.c - unit tests for the ELF parser. */
#include "elf.h"
#include "nrf_ocd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* Build a tiny ELF32 in memory with one PT_LOAD segment at 0x20000000. */
static size_t make_elf32(uint8_t *buf) {
    /* ELF32 header (52 bytes). */
    buf[0] = 0x7f; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1;     /* ELFCLASS32 */
    buf[5] = 1;     /* ELFDATA2LSB */
    buf[6] = 1;     /* EV_CURRENT */
    buf[7] = 0;
    /* e_type = ET_EXEC = 2 at offset 16. */
    buf[16] = 2;
    /* e_machine = EM_ARM = 40 at offset 18. */
    buf[18] = 40;
    /* e_version = 1 at offset 20. */
    buf[20] = 1;
    /* e_entry = 0x20000000 at offset 24. */
    buf[24] = 0; buf[25] = 0; buf[26] = 0; buf[27] = 0x20;
    /* e_phoff = 52 at offset 28. */
    buf[28] = 52;
    /* e_phentsize = 32 at offset 42. */
    buf[42] = 32;
    /* e_phnum = 1 at offset 44. */
    buf[44] = 1;

    /* Program header at offset 52. */
    /* p_type = PT_LOAD = 1 at offset 52. */
    buf[52] = 1;
    /* p_offset = 84 (data start) at offset 56. In LE, LSB first. */
    buf[56] = 84;  /* LSB */
    /* bytes 57, 58, 59 are 0 (already zeroed). */
    /* p_vaddr = 0x20000000 at offset 60. */
    buf[60] = 0; buf[61] = 0; buf[62] = 0; buf[63] = 0x20;
    /* p_paddr = 0x20000000 at offset 64. */
    buf[64] = 0; buf[65] = 0; buf[66] = 0; buf[67] = 0x20;
    /* p_filesz = 4 at offset 68. */
    buf[68] = 4;
    /* p_memsz = 4 at offset 72. */
    buf[72] = 4;

    /* Data at offset 84: 0xDE 0xAD 0xBE 0xEF. */
    buf[84] = 0xDE; buf[85] = 0xAD; buf[86] = 0xBE; buf[87] = 0xEF;
    return 88;
}

int main(void) {
    uint8_t buf[256] = {0};
    size_t sz = make_elf32(buf);
    char path[] = "/tmp/nrf_ocd_test_elfXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    write(fd, buf, sz);
    close(fd);

    hex_image_t img;
    hex_image_init(&img);
    elf_info_t info = {0};
    nrf_ocd_status_t st = elf_load(path, &img, &info);
    if (st != NRF_OCD_OK) {
        fprintf(stderr, "elf_load failed: %s\n", nrf_ocd_strerror(st));
        return 1;
    }
    ASSERT(st == NRF_OCD_OK);
    ASSERT(info.entry == 0x20000000);
    ASSERT(img.count == 1);
    if (img.count >= 1) {
        ASSERT(img.segments[0].address == 0x20000000);
        ASSERT(img.segments[0].size == 4);
        ASSERT(img.segments[0].data[0] == 0xDE);
        ASSERT(img.segments[0].data[1] == 0xAD);
    }
    hex_image_free(&img);
    unlink(path);

    if (failures) {
        fprintf(stderr, "%d ELF test(s) failed\n", failures);
        return 1;
    }
    printf("All ELF tests passed\n");
    return 0;
}
