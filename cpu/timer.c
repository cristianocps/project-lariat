#include "timer.h"
#include "ports.h"
#include "idt.h"
#include "pic.h"
#include "sched.h"
#include "serial.h"

static volatile uint64_t tick_count = 0;

static void timer_callback(registers_t *r) {
    tick_count++;
    scheduler_tick(r);
}

void timer_init(uint32_t frequency) {
    register_interrupt_handler(32, timer_callback);

    uint32_t divisor = 1193182 / frequency;

    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

uint64_t timer_get_ticks(void) {
    return tick_count;
}

void timer_sleep(uint64_t ms) {
    uint64_t end = tick_count + ms;
    while (tick_count < end) {
        __asm__ __volatile__("hlt");
    }
}
