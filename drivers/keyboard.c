#include "keyboard.h"
#include "ports.h"
#include "idt.h"
#include "pic.h"
#include "kapi.h"
#include "sched.h"
#include "uapi.h"
#include "gfx.h"
#include <stddef.h>

/* TTY line-discipline state.  Defaults: cooked mode with echo and terminal
 * signals enabled, matching a normal interactive terminal. */
static struct termios tty_termios = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = 0,
    .c_lflag = ISIG | ICANON | ECHO | ECHOE,
    .c_line  = 0,
    .c_cc = {
        [VINTR] = 3,     /* Ctrl-C */
        [VQUIT] = 28,    /* Ctrl-\ */
        [VERASE] = 0x7f, /* DEL */
        [VKILL] = 21,    /* Ctrl-U */
        [VEOF]  = 4,     /* Ctrl-D */
        [VSUSP] = 26,    /* Ctrl-Z */
        [VMIN]  = 1,
        [VTIME] = 0,
    },
};
static int tty_fg_pgrp = 0;

void tty_termios_get(struct termios *t) { *t = tty_termios; }
void tty_termios_set(const struct termios *t) { tty_termios = *t; }
int  tty_get_fg_pgrp(void) { return tty_fg_pgrp; }
void tty_set_fg_pgrp(int pgrp) { tty_fg_pgrp = pgrp; }

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

static volatile uint8_t keyboard_buffer[KB_BUF_SIZE];
static volatile uint16_t kb_read_idx = 0;
static volatile uint16_t kb_write_idx = 0;

static volatile char tty_buffer[TTY_BUF_SIZE];
static volatile uint16_t tty_read_idx = 0;
static volatile uint16_t tty_write_idx = 0;

/* Protects both ring buffers so the IRQ producer and a (possibly cross-CPU)
 * consumer never corrupt the indices. */
static spinlock_t kb_lock = SPINLOCK_INIT;

/* Readers blocked in tty_getc wait here; the keyboard/serial IRQ wakes them.
 * Lock order is always kb_lock -> sched_lock (wq_wake takes sched_lock), and
 * tty_getc never holds kb_lock while inside WAIT_EVENT, so there is no
 * inversion. */
static wait_queue_t tty_waitq = WAIT_QUEUE_INIT;

static const char scancode_to_ascii[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b',
    '\t',
    'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0,
    '*',
    0,
    ' ',
};

/* Push one ASCII char into the TTY ring buffer.  Caller must hold kb_lock. */
static void tty_push_locked(char c) {
    uint16_t tnext = (tty_write_idx + 1) % TTY_BUF_SIZE;
    if (tnext != tty_read_idx) {
        tty_buffer[tty_write_idx] = c;
        tty_write_idx = tnext;
    }
}

/* If ISIG is on and `c` is a terminal-signal control char, return the signal
 * to raise (and the char is consumed, not buffered); else 0. */
static int tty_signal_for(char c) {
    if (!(tty_termios.c_lflag & ISIG)) return 0;
    unsigned char uc = (unsigned char)c;
    if (uc == tty_termios.c_cc[VINTR]) return 2;   /* SIGINT  */
    if (uc == tty_termios.c_cc[VQUIT]) return 3;   /* SIGQUIT */
    if (uc == tty_termios.c_cc[VSUSP]) return 20;  /* SIGTSTP */
    return 0;
}

static void keyboard_callback(registers_t *r) {
    (void)r;
    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode & 0x80) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&kb_lock);

    uint16_t next = (kb_write_idx + 1) % KB_BUF_SIZE;
    if (next != kb_read_idx) {
        keyboard_buffer[kb_write_idx] = scancode;
        kb_write_idx = next;
    }

    /* Also push ASCII char to TTY buffer (unless it generates a signal). */
    int sig = 0;
    char ascii = 0;
    if (scancode < sizeof(scancode_to_ascii)) {
        char c = scancode_to_ascii[scancode];
        if (c) {
            ascii = c;
            sig = tty_signal_for(c);
            if (!sig) tty_push_locked(c);
        }
    }

    spin_unlock_irqrestore(&kb_lock, flags);
    /* Mirror printable/control keys into the unified /dev/input stream for the
     * GUI compositor (key-press only). */
    if (ascii && !sig) input_push(EV_KEY, (uint32_t)(unsigned char)ascii, 1);
    if (sig && tty_fg_pgrp) sched_signal_pgrp(tty_fg_pgrp, sig);
    wq_wake_all(&tty_waitq);
}

/* COM1 receive interrupt: drain the UART FIFO into the TTY buffer.  This is the
 * path that makes a piped/serial console reliable - bytes are captured the
 * instant they arrive rather than being overwritten in the 1-byte register. */
static void serial_callback(registers_t *r) {
    (void)r;
    int sig = 0;
    uint64_t flags = spin_lock_irqsave(&kb_lock);
    while (inb(0x3F8 + 5) & 0x01) {          /* LSR: data ready */
        char c = (char)inb(0x3F8);           /* RBR */
        int s = tty_signal_for(c);
        if (s) { sig = s; continue; }        /* consume, raise below */
        tty_push_locked(c);
    }
    spin_unlock_irqrestore(&kb_lock, flags);
    if (sig && tty_fg_pgrp) sched_signal_pgrp(tty_fg_pgrp, sig);
    wq_wake_all(&tty_waitq);
}

void keyboard_init(void) {
    register_interrupt_handler(33, keyboard_callback);
    pic_clear_mask(1);
}

/* Register the COM1 RX handler (vector 36 == IRQ4).  The caller is responsible
 * for routing GSI 4 to this vector in the IO-APIC and enabling the UART's RX
 * interrupt (serial_enable_rx_interrupt). */
void keyboard_register_serial(void) {
    register_interrupt_handler(36, serial_callback);
}

int keyboard_poll(void) {
    return kb_read_idx != kb_write_idx;
}

int keyboard_getc(void) {
    __asm__ __volatile__("sti");
    while (kb_read_idx == kb_write_idx) {
        /* Also poll serial port (COM1) for input */
        if (inb(0x3FD) & 0x01) {
            return inb(0x3F8);
        }
        __asm__ __volatile__("pause");
    }
    uint64_t flags = spin_lock_irqsave(&kb_lock);
    uint8_t scancode = keyboard_buffer[kb_read_idx];
    kb_read_idx = (kb_read_idx + 1) % KB_BUF_SIZE;
    spin_unlock_irqrestore(&kb_lock, flags);

    if (scancode < sizeof(scancode_to_ascii)) {
        return scancode_to_ascii[scancode];
    }
    return 0;
}

int tty_poll(void) {
    return tty_read_idx != tty_write_idx;
}

int tty_getc(void) {
    /* Block (yielding the CPU) until a char is available.  WAIT_EVENT checks the
     * ring under the scheduler lock so an IRQ producer cannot slip a wakeup
     * between the emptiness test and the sleep. */
    WAIT_EVENT(tty_waitq, tty_read_idx != tty_write_idx);

    uint64_t flags = spin_lock_irqsave(&kb_lock);
    char c = tty_buffer[tty_read_idx];
    tty_read_idx = (tty_read_idx + 1) % TTY_BUF_SIZE;
    spin_unlock_irqrestore(&kb_lock, flags);
    return c;
}

/* Block until a char is available OR the calling thread has a pending,
 * unblocked signal.  Returns the char, or -1 if interrupted by a signal. */
int tty_getc_intr(void) {
    struct thread *self = current_thread();
    for (;;) {
        uint64_t f = sched_lock_acquire();
        if (tty_read_idx != tty_write_idx) { sched_lock_release(f); break; }
        if (self && (self->sig_pending & ~self->sig_mask)) {
            sched_lock_release(f);
            return -1;
        }
        sched_wait_locked(&tty_waitq, f);
    }
    uint64_t flags = spin_lock_irqsave(&kb_lock);
    char c = tty_buffer[tty_read_idx];
    tty_read_idx = (tty_read_idx + 1) % TTY_BUF_SIZE;
    spin_unlock_irqrestore(&kb_lock, flags);
    return (unsigned char)c;
}

int tty_read(char *buf, size_t count) {
    size_t n = 0;
    uint64_t flags = spin_lock_irqsave(&kb_lock);
    while (n < count) {
        if (tty_read_idx == tty_write_idx) {
            break;
        }
        buf[n++] = tty_buffer[tty_read_idx];
        tty_read_idx = (tty_read_idx + 1) % TTY_BUF_SIZE;
    }
    spin_unlock_irqrestore(&kb_lock, flags);
    return (int)n;
}
