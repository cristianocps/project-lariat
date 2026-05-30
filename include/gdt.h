#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Segment selectors */
#define SEG_NULL       0x00
#define SEG_KCODE      0x08
#define SEG_KDATA      0x10
#define SEG_UCODE32    0x18   /* 32-bit compat, base for SYSRET calculations */
#define SEG_UDATA      0x20
#define SEG_UCODE64    0x28   /* 64-bit user code: UCODE32 + 16 */
#define SEG_TSS        0x30

/* x86_64 TSS layout (104 bytes) */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

void gdt_init(void);
void gdt_load_ap(void);
void tss_init(void);
void tss_init_cpu(uint32_t cpu);
void tss_set_rsp0(uint64_t rsp);

struct tss *tss_get_current(void);

#endif
