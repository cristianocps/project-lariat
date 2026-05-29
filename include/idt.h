#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags;
} __attribute__((packed)) registers_t;

typedef void (*isr_t)(registers_t *);

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void register_interrupt_handler(uint8_t n, isr_t handler);

extern void isr_0(void);   extern void isr_1(void);   extern void isr_2(void);   extern void isr_3(void);
extern void isr_4(void);   extern void isr_5(void);   extern void isr_6(void);   extern void isr_7(void);
extern void isr_8(void);   extern void isr_9(void);   extern void isr_10(void);  extern void isr_11(void);
extern void isr_12(void);  extern void isr_13(void);  extern void isr_14(void);  extern void isr_15(void);
extern void isr_16(void);  extern void isr_17(void);  extern void isr_18(void);  extern void isr_19(void);
extern void isr_20(void);  extern void isr_21(void);  extern void isr_22(void);  extern void isr_23(void);
extern void isr_24(void);  extern void isr_25(void);  extern void isr_26(void);  extern void isr_27(void);
extern void isr_28(void);  extern void isr_29(void);  extern void isr_30(void);  extern void isr_31(void);

extern void irq_0(void);   extern void irq_1(void);   extern void irq_2(void);   extern void irq_3(void);
extern void irq_4(void);   extern void irq_5(void);   extern void irq_6(void);   extern void irq_7(void);
extern void irq_8(void);   extern void irq_9(void);   extern void irq_10(void);  extern void irq_11(void);
extern void irq_12(void);  extern void irq_13(void);  extern void irq_14(void);  extern void irq_15(void);

#endif
