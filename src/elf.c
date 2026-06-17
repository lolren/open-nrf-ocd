/* elf.c - minimal ELF32 / ELF64 reader for loadable segments. */
#include "elf.h"
#include "log.h"
#include "nrf_ocd.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EI_NIDENT 16
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFMAG "\x7f" "ELF"
#define ET_EXEC 2
#define PT_LOAD 1

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint16_t e_version;
    uint16_t e_pad;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

static nrf_ocd_status_t load_elf32(FILE *f, hex_image_t *img, elf_info_t *info) {
    Elf32_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, f) != 1) return NRF_OCD_ERR_FILE_FORMAT;
    info->entry = eh.e_entry;
    info->is_64 = false;
    if (eh.e_phoff == 0) return NRF_OCD_OK;
    if (fseek(f, (long)eh.e_phoff, SEEK_SET) != 0) return NRF_OCD_ERR_IO;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        Elf32_Phdr ph;
        if (fread(&ph, sizeof(ph), 1, f) != 1) return NRF_OCD_ERR_FILE_FORMAT;
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_filesz == 0) continue;
        if (fseek(f, (long)ph.p_offset, SEEK_SET) != 0) return NRF_OCD_ERR_IO;
        uint8_t *buf = (uint8_t *)malloc(ph.p_filesz);
        if (!buf) return NRF_OCD_ERR_NO_MEM;
        if (fread(buf, 1, ph.p_filesz, f) != ph.p_filesz) {
            free(buf);
            return NRF_OCD_ERR_IO;
        }
        nrf_ocd_status_t st = (nrf_ocd_status_t)0;
        /* Append the segment to the image. We push it as a single block. */
        size_t prev_count = img->count;
        st = hex_image_load_buffer_segment(img, ph.p_paddr, buf, ph.p_filesz);
        free(buf);
        if (st != NRF_OCD_OK) return st;
        (void)prev_count;
    }
    return NRF_OCD_OK;
}

static nrf_ocd_status_t load_elf64(FILE *f, hex_image_t *img, elf_info_t *info) {
    Elf64_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, f) != 1) return NRF_OCD_ERR_FILE_FORMAT;
    info->entry = (uint32_t)eh.e_entry;
    info->is_64 = true;
    if (eh.e_phoff == 0) return NRF_OCD_OK;
    if (fseek(f, (long)eh.e_phoff, SEEK_SET) != 0) return NRF_OCD_ERR_IO;
    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        if (fread(&ph, sizeof(ph), 1, f) != 1) return NRF_OCD_ERR_FILE_FORMAT;
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_filesz == 0) continue;
        if (ph.p_filesz > 0x1000000U) {
            LOG_ERROR("ELF segment too large: %llu", (unsigned long long)ph.p_filesz);
            return NRF_OCD_ERR_FILE_FORMAT;
        }
        if (fseek(f, (long)ph.p_offset, SEEK_SET) != 0) return NRF_OCD_ERR_IO;
        uint8_t *buf = (uint8_t *)malloc((size_t)ph.p_filesz);
        if (!buf) return NRF_OCD_ERR_NO_MEM;
        if (fread(buf, 1, (size_t)ph.p_filesz, f) != ph.p_filesz) {
            free(buf);
            return NRF_OCD_ERR_IO;
        }
        nrf_ocd_status_t st = hex_image_load_buffer_segment(img, (uint32_t)ph.p_paddr,
                                                            buf, (size_t)ph.p_filesz);
        free(buf);
        if (st != NRF_OCD_OK) return st;
    }
    return NRF_OCD_OK;
}

nrf_ocd_status_t elf_load(const char *path, hex_image_t *img, elf_info_t *info) {
    if (!path || !img) return NRF_OCD_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("Could not open %s: %s", path, strerror(errno));
        return NRF_OCD_ERR_FILE_OPEN;
    }
    uint8_t mag[4];
    if (fread(mag, 1, 4, f) != 4 || memcmp(mag, ELFMAG, 4) != 0) {
        fclose(f);
        LOG_ERROR("%s: not an ELF file", path);
        return NRF_OCD_ERR_FILE_FORMAT;
    }
    fseek(f, 4, SEEK_SET);
    int cls_i = fgetc(f);
    if (cls_i == EOF) { fclose(f); return NRF_OCD_ERR_FILE_FORMAT; }
    uint8_t cls = (uint8_t)cls_i;
    fclose(f);
    f = fopen(path, "rb");
    if (!f) return NRF_OCD_ERR_FILE_OPEN;
    nrf_ocd_status_t st;
    if (cls == ELFCLASS32) st = load_elf32(f, img, info);
    else if (cls == ELFCLASS64) st = load_elf64(f, img, info);
    else {
        fclose(f);
        return NRF_OCD_ERR_FILE_FORMAT;
    }
    fclose(f);
    return st;
}
