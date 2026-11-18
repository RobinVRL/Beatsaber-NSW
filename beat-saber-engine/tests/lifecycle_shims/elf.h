/* Host-only subset of public ELF64/AArch64 layout for lifecycle tests.
 * The Switch build uses devkitA64's full <elf.h>, not this header.
 */
#ifndef BS_LIFECYCLE_TEST_ELF_H
#define BS_LIFECYCLE_TEST_ELF_H
#include <stdint.h>

#define EI_NIDENT 16
#define EI_CLASS 4
#define ELFMAG "\177ELF"
#define SELFMAG 4
#define ELFCLASS64 2
#define EM_AARCH64 183
#define PT_LOAD 1

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;

typedef struct Elf64_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type, e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff, e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct Elf64_Phdr {
    Elf64_Word p_type, p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr, p_paddr;
    Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct Elf64_Shdr {
    Elf64_Word sh_name, sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link, sh_info;
    Elf64_Xword sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct Elf64_Sym {
    Elf64_Word st_name;
    unsigned char st_info, st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

typedef struct Elf64_Rela {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;
#endif
