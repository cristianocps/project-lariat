#include "idt.h"
#include "ports.h"
#include "serial.h"
#include "vga.h"
#include "lapic.h"
#include "smp.h"
#include "sched.h"

/* When non-zero, the 8259 PICs are disabled and all maskable interrupts are
 * delivered (and acknowledged) through the local APIC. */
static int apic_mode = 0;

void idt_set_apic_mode(int on) {
    apic_mode = on;
}

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_p;

static isr_t interrupt_handlers[IDT_ENTRIES];

extern void idt_flush(void);

static const char *exception_names[] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 FPU Error", "Alignment Check",
    "Machine Check", "SIMD FP Exception", "Virtualization Exception",
    "Control Protection Exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low  = base & 0xFFFF;
    idt[num].offset_mid  = (base >> 16) & 0xFFFF;
    idt[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].selector    = sel;
    idt[num].ist         = 0;
    idt[num].type_attr   = flags;
    idt[num].zero        = 0;
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

static void panic_dump(registers_t *r) {
    serial_printf(SERIAL_COM1, "\n[!!!] PANIC: %s (int %d, err 0x%x)\n",
                  exception_names[r->int_no], r->int_no, r->error_code);
    serial_printf(SERIAL_COM1, "  RAX=0x%llx RCX=0x%llx RDX=0x%llx RBX=0x%llx\n",
                  r->rax, r->rcx, r->rdx, r->rbx);
    /* For a ring-3 fault the CPU pushed user RSP/SS right after RFLAGS. */
    uint64_t *after = (uint64_t *)(r + 1);
    serial_printf(SERIAL_COM1, "  RBP=0x%llx RSI=0x%llx RDI=0x%llx uRSP=0x%llx\n",
                  r->rbp, r->rsi, r->rdi, after[0]);
    serial_printf(SERIAL_COM1, "  RIP=0x%llx CS=0x%x RFLAGS=0x%llx\n",
                  r->rip, r->cs, r->rflags);
    {
        struct thread *ct = current_thread();
        if (ct)
            serial_printf(SERIAL_COM1, "  cur tid=%d name=%s\n",
                          (int)ct->tid, ct->name);
    }
    uint64_t cr2, cr3;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    serial_printf(SERIAL_COM1, "  CR2=0x%llx CR3=0x%llx\n", cr2, cr3);

    vga_set_color(VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("\nKERNEL PANIC: ");
    vga_print(exception_names[r->int_no]);
    vga_print("\nSystem halted.\n");

    __asm__ __volatile__("cli; hlt");
}

void isr_handler(registers_t *r) {
    if (r->int_no < 32) {
        /* A CPU exception.  If it came from ring 3 (user mode), it is a fault
         * in a user process, not a kernel bug: terminate just that process with
         * the matching fatal signal (like Unix delivering an uncaught SIGSEGV/
         * SIGILL/SIGFPE) and reschedule, instead of taking down the kernel.
         * Only genuine ring-0 faults panic. */
        if ((r->cs & 0x3) == 3) {
            int sig;
            switch (r->int_no) {
                case 0:  sig = 8;  break;   /* #DE  -> SIGFPE  */
                case 6:  sig = 4;  break;   /* #UD  -> SIGILL  */
                case 16: case 19: sig = 8; break; /* x87/SIMD -> SIGFPE */
                case 13: sig = 11; break;   /* #GP  -> SIGSEGV */
                case 14: sig = 11; break;   /* #PF  -> SIGSEGV */
                default: sig = 11; break;   /* SIGSEGV */
            }
            uint64_t cr2;
            __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
            struct thread *ct = current_thread();
            serial_printf(SERIAL_COM1,
                "[fault] tid=%d (%s) %s err=0x%x rip=0x%llx cr2=0x%llx -> sig %d; killing process\n",
                ct ? (int)ct->tid : -1, ct ? ct->name : "?",
                exception_names[r->int_no], r->error_code, r->rip, cr2, sig);
            if (ct) ct->exit_code = 128 + sig;
            thread_exit();   /* does not return */
        }
        panic_dump(r);
    }

    if (apic_mode) {
        /* APIC mode: every device/IPI interrupt is acknowledged at the local
         * APIC.  The timer handler issues its own EOI before it may switch
         * contexts; for all other vectors we EOI here after dispatch. */
        if (interrupt_handlers[r->int_no]) {
            interrupt_handlers[r->int_no](r);
        }
        if (r->int_no >= 32 && r->int_no != VEC_LAPIC_TIMER) {
            lapic_eoi();
        }
        return;
    }

    /* Legacy PIC mode: IRQ vectors (32-47) get a PIC EOI. */
    if (r->int_no >= 32 && r->int_no < 48) {
        if (r->int_no >= 40) {
            outb(0xA0, 0x20);
        }
        outb(0x20, 0x20);
    }

    if (interrupt_handlers[r->int_no]) {
        interrupt_handlers[r->int_no](r);
    }
}

void idt_init(void) {
    idt_p.limit = sizeof(idt) - 1;
    idt_p.base  = (uint64_t)&idt;

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0x08, 0x8E);
        interrupt_handlers[i] = 0;
    }

    #define CS_KERNEL 0x08
    idt_set_gate(0,  (uint64_t)isr_0,  CS_KERNEL, 0x8E);
    idt_set_gate(1,  (uint64_t)isr_1,  CS_KERNEL, 0x8E);
    idt_set_gate(2,  (uint64_t)isr_2,  CS_KERNEL, 0x8E);
    idt_set_gate(3,  (uint64_t)isr_3,  CS_KERNEL, 0xEF);  /* Trap gate, DPL=3 for int3 from userspace */
    idt_set_gate(4,  (uint64_t)isr_4,  CS_KERNEL, 0x8E);
    idt_set_gate(5,  (uint64_t)isr_5,  CS_KERNEL, 0x8E);
    idt_set_gate(6,  (uint64_t)isr_6,  CS_KERNEL, 0x8E);
    idt_set_gate(7,  (uint64_t)isr_7,  CS_KERNEL, 0x8E);
    idt_set_gate(8,  (uint64_t)isr_8,  CS_KERNEL, 0x8E);
    idt_set_gate(9,  (uint64_t)isr_9,  CS_KERNEL, 0x8E);
    idt_set_gate(10, (uint64_t)isr_10, CS_KERNEL, 0x8E);
    idt_set_gate(11, (uint64_t)isr_11, CS_KERNEL, 0x8E);
    idt_set_gate(12, (uint64_t)isr_12, CS_KERNEL, 0x8E);
    idt_set_gate(13, (uint64_t)isr_13, CS_KERNEL, 0x8E);
    idt_set_gate(14, (uint64_t)isr_14, CS_KERNEL, 0x8E);
    idt_set_gate(15, (uint64_t)isr_15, CS_KERNEL, 0x8E);
    idt_set_gate(16, (uint64_t)isr_16, CS_KERNEL, 0x8E);
    idt_set_gate(17, (uint64_t)isr_17, CS_KERNEL, 0x8E);
    idt_set_gate(18, (uint64_t)isr_18, CS_KERNEL, 0x8E);
    idt_set_gate(19, (uint64_t)isr_19, CS_KERNEL, 0x8E);
    idt_set_gate(20, (uint64_t)isr_20, CS_KERNEL, 0x8E);
    idt_set_gate(21, (uint64_t)isr_21, CS_KERNEL, 0x8E);
    idt_set_gate(22, (uint64_t)isr_22, CS_KERNEL, 0x8E);
    idt_set_gate(23, (uint64_t)isr_23, CS_KERNEL, 0x8E);
    idt_set_gate(24, (uint64_t)isr_24, CS_KERNEL, 0x8E);
    idt_set_gate(25, (uint64_t)isr_25, CS_KERNEL, 0x8E);
    idt_set_gate(26, (uint64_t)isr_26, CS_KERNEL, 0x8E);
    idt_set_gate(27, (uint64_t)isr_27, CS_KERNEL, 0x8E);
    idt_set_gate(28, (uint64_t)isr_28, CS_KERNEL, 0x8E);
    idt_set_gate(29, (uint64_t)isr_29, CS_KERNEL, 0x8E);
    idt_set_gate(30, (uint64_t)isr_30, CS_KERNEL, 0x8E);
    idt_set_gate(31, (uint64_t)isr_31, CS_KERNEL, 0x8E);

    idt_set_gate(32, (uint64_t)irq_0,  CS_KERNEL, 0x8E);
    idt_set_gate(33, (uint64_t)irq_1,  CS_KERNEL, 0x8E);
    idt_set_gate(34, (uint64_t)irq_2,  CS_KERNEL, 0x8E);
    idt_set_gate(35, (uint64_t)irq_3,  CS_KERNEL, 0x8E);
    idt_set_gate(36, (uint64_t)irq_4,  CS_KERNEL, 0x8E);
    idt_set_gate(37, (uint64_t)irq_5,  CS_KERNEL, 0x8E);
    idt_set_gate(38, (uint64_t)irq_6,  CS_KERNEL, 0x8E);
    idt_set_gate(39, (uint64_t)irq_7,  CS_KERNEL, 0x8E);
    idt_set_gate(40, (uint64_t)irq_8,  CS_KERNEL, 0x8E);
    idt_set_gate(41, (uint64_t)irq_9,  CS_KERNEL, 0x8E);
    idt_set_gate(42, (uint64_t)irq_10, CS_KERNEL, 0x8E);
    idt_set_gate(43, (uint64_t)irq_11, CS_KERNEL, 0x8E);
    idt_set_gate(44, (uint64_t)irq_12, CS_KERNEL, 0x8E);
    idt_set_gate(45, (uint64_t)irq_13, CS_KERNEL, 0x8E);
    idt_set_gate(46, (uint64_t)irq_14, CS_KERNEL, 0x8E);
    idt_set_gate(47, (uint64_t)irq_15, CS_KERNEL, 0x8E);

    /* APIC / IPI vectors */
    extern void isr_240(void);
    extern void isr_253(void);
    extern void isr_255(void);
    idt_set_gate(240, (uint64_t)isr_240, CS_KERNEL, 0x8E);
    idt_set_gate(253, (uint64_t)isr_253, CS_KERNEL, 0x8E);
    idt_set_gate(255, (uint64_t)isr_255, CS_KERNEL, 0x8E);

    __asm__ __volatile__("lidt %0" :: "m"(idt_p));
}

/* Load the (shared) IDT on an application processor. */
void idt_load_ap(void) {
    __asm__ __volatile__("lidt %0" :: "m"(idt_p));
}
