#ifndef FULMEN_ELF_H
#define FULMEN_ELF_H

#include "types.h"

#define ELF_MAGIC    0x464C457FU /* "\x7FELF" little-endian */
#define ELFCLASS64   2U
#define ELFDATA2LSB  1U
#define ET_EXEC      2U
#define ET_DYN       3U
#define EM_AARCH64   183U
#define PT_LOAD      1U

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
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

int elf_run_from_vfs(const char *path, int argc, char **argv, int *exit_code);
int el0_run_inline_demo(void);

#endif /* FULMEN_ELF_H */
