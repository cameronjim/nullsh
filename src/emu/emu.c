// The emu builtin: load a CHIP-8 rom and run it, on the terminal or headless.

#define _POSIX_C_SOURCE 200809L

#include "emu.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cpu.h"
#include "display.h"
#include "keypad.h"
#include "term.h"

#include "../alloc/alloc.h"
#include "../util/str.h"

#define ROM_MAX (CHIP8_MEM - CHIP8_ROM_BASE)
#define NS_PER_SEC 1000000000LL
#define STEPS_PER_SEC 700
#define TIMER_HZ 60
#define STEP_NS (NS_PER_SEC / STEPS_PER_SEC)
#define TIMER_NS (NS_PER_SEC / TIMER_HZ)
#define IDLE_NS 1000000L
// A press is held this long after its last byte, then a release is synthesized.
#define KEY_HOLD_NS 100000000LL
// A resume must not replay every cycle the stop swallowed.
#define CATCHUP_NS (NS_PER_SEC / 4)
#define KEY_ESC 0x1b
#define KEY_COUNT 16

// Set in a handler, consumed by the loop: a resume needs raw mode back.
static volatile sig_atomic_t g_resume;

static struct sigaction g_tstp_self;
static struct sigaction g_tstp_dfl;
static struct sigaction g_prev_tstp;
static struct sigaction g_prev_cont;
static sigset_t g_tstp_mask;

static void cont_handler(int sig) {
    (void)sig;
    g_resume = 1;
}

// The stop has to happen with the terminal already sane, so this handler does
// the restore itself, then re-raises under the default disposition. SIGTSTP is
// blocked while a handler runs, so the raise needs the explicit unblock.
static void tstp_handler(int sig) {
    (void)sig;
    term_emergency_restore();
    (void)sigaction(SIGTSTP, &g_tstp_dfl, NULL);
    (void)sigprocmask(SIG_UNBLOCK, &g_tstp_mask, NULL);
    (void)raise(SIGTSTP);
    (void)sigprocmask(SIG_BLOCK, &g_tstp_mask, NULL);
    (void)sigaction(SIGTSTP, &g_tstp_self, NULL);
    g_resume = 1;
}

static bool signals_install(void) {
    g_resume = 0;
    if (sigemptyset(&g_tstp_mask) != 0 || sigaddset(&g_tstp_mask, SIGTSTP) != 0) {
        return false;
    }
    memset(&g_tstp_self, 0, sizeof g_tstp_self);
    g_tstp_self.sa_handler = tstp_handler;
    memset(&g_tstp_dfl, 0, sizeof g_tstp_dfl);
    g_tstp_dfl.sa_handler = SIG_DFL;
    struct sigaction cont;
    memset(&cont, 0, sizeof cont);
    cont.sa_handler = cont_handler;
    cont.sa_flags = SA_RESTART;
    if (sigemptyset(&g_tstp_self.sa_mask) != 0 ||
        sigemptyset(&g_tstp_dfl.sa_mask) != 0 || sigemptyset(&cont.sa_mask) != 0) {
        return false;
    }
    if (sigaction(SIGTSTP, &g_tstp_self, &g_prev_tstp) != 0) {
        return false;
    }
    if (sigaction(SIGCONT, &cont, &g_prev_cont) != 0) {
        (void)sigaction(SIGTSTP, &g_prev_tstp, NULL);
        return false;
    }
    return true;
}

static void signals_restore(void) {
    (void)sigaction(SIGTSTP, &g_prev_tstp, NULL);
    (void)sigaction(SIGCONT, &g_prev_cont, NULL);
}

// On NSH_OK the caller nsh_frees *out; why always names the failure.
static NshError rom_read(const char *path, uint8_t **out, size_t *out_len,
                         const char **why) {
    *out = NULL;
    *out_len = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *why = strerror(errno);
        return NSH_ERR_IO;
    }

    struct stat st;
    NshError bad = NSH_ERR_INVALID;
    if (fstat(fd, &st) != 0) {
        *why = strerror(errno);
        bad = NSH_ERR_IO;
    } else if (!S_ISREG(st.st_mode)) {
        *why = "not a regular file";
    } else if (st.st_size == 0) {
        *why = "empty rom";
    } else if (st.st_size > (off_t)ROM_MAX) {
        *why = "rom is too large for chip-8 memory";
    } else {
        bad = NSH_OK;
    }
    if (bad != NSH_OK) {
        (void)close(fd);
        return bad;
    }

    size_t len = (size_t)st.st_size;
    uint8_t *buf = nsh_malloc(len);
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, buf + done, len - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            *why = (n < 0) ? strerror(errno) : "short read";
            nsh_free(buf);
            (void)close(fd);
            return NSH_ERR_IO;
        }
        done += (size_t)n;
    }
    (void)close(fd);
    *out = buf;
    *out_len = len;
    return NSH_OK;
}

static void report_step_error(const Chip8 *c, NshError err) {
    fprintf(stderr, "nullsh: emu: step failed at pc 0x%03X: %s\n",
            (unsigned)c->pc, nsh_error_str(err));
}

// No terminal, no timers, no sleeps: the same rom always paints the same frame.
static int run_headless(Chip8 *c, unsigned long cycles) {
    int status = 0;
    for (unsigned long i = 0; i < cycles; i++) {
        NshError err = cpu_step(c);
        if (err != NSH_OK && status == 0) {
            report_step_error(c, err);
            status = 1;
        }
    }
    Str out;
    str_init(&out);
    display_render_ascii(c->fb, &out);
    fwrite(out.data, 1, out.len, stdout);
    fflush(stdout);
    str_free(&out);
    return status;
}

static long long now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * NS_PER_SEC + (long long)ts.tv_nsec;
}

static void idle(void) {
    struct timespec req = {0, IDLE_NS};
    (void)nanosleep(&req, NULL);
}

// The terminal reports presses only, so a key expires on its own clock.
static void release_stale_keys(Chip8 *c, long long *held, long long now) {
    for (int k = 0; k < KEY_COUNT; k++) {
        if (held[k] != 0 && now - held[k] >= KEY_HOLD_NS) {
            held[k] = 0;
            cpu_key_event(c, k, false);
        }
    }
}

// true when ESC arrived; every other byte either presses a key or is dropped.
static bool drain_input(Chip8 *c, long long *held, long long now) {
    int ch;
    while ((ch = term_read_key()) >= 0) {
        if (ch == KEY_ESC) {
            return true;
        }
        int key = keypad_map((unsigned char)ch);
        if (key >= 0) {
            held[key] = now;
            cpu_key_event(c, key, true);
        }
    }
    return false;
}

static void beep_once(const Chip8 *c, bool *sounding) {
    if (c->sound == 0) {
        *sounding = false;
        return;
    }
    if (!*sounding) {
        *sounding = true;
        fputc('\a', stdout);
        fflush(stdout);
    }
}

static void paint(Chip8 *c, Str *frame) {
    if (!c->fb_dirty) {
        return;
    }
    c->fb_dirty = false;
    display_render(c->fb, frame);
    fwrite(frame->data, 1, frame->len, stdout);
    fflush(stdout);
}

static int run_interactive(Chip8 *c) {
    if (term_enter_raw() != NSH_OK) {
        return 1;
    }

    Str frame;
    str_init(&frame);
    long long held[KEY_COUNT] = {0};
    long long prev = now_ns();
    long long step_acc = 0;
    long long timer_acc = 0;
    bool sounding = false;
    int status = 0;

    for (;;) {
        if (g_resume) {
            g_resume = 0;
            if (term_enter_raw() != NSH_OK) {
                status = 1;
                break;
            }
            c->fb_dirty = true;
            prev = now_ns();
        }

        long long now = now_ns();
        if (drain_input(c, held, now)) {
            break;
        }
        release_stale_keys(c, held, now);

        long long delta = now - prev;
        prev = now;
        if (delta < 0) {
            delta = 0;
        }
        if (delta > CATCHUP_NS) {
            delta = CATCHUP_NS;
        }
        step_acc += delta;
        timer_acc += delta;

        NshError err = NSH_OK;
        while (step_acc >= STEP_NS && err == NSH_OK) {
            step_acc -= STEP_NS;
            err = cpu_step(c);
        }
        if (err != NSH_OK) {
            term_exit_raw();
            report_step_error(c, err);
            status = 1;
            break;
        }
        while (timer_acc >= TIMER_NS) {
            timer_acc -= TIMER_NS;
            cpu_tick_timers(c);
        }

        beep_once(c, &sounding);
        paint(c, &frame);
        idle();
    }

    term_exit_raw();
    str_free(&frame);
    return status;
}

static void emu_usage(void) {
    fputs("nullsh: emu: usage: emu ROMFILE\n", stderr);
}

int emu_builtin(Shell *sh, int argc, char **argv) {
    if (argc != 2) {
        emu_usage();
        return 1;
    }

    const char *spec = getenv("NSH_EMU_HEADLESS");
    unsigned long cycles = 0;
    if (spec != NULL) {
        char *end = NULL;
        errno = 0;
        cycles = strtoul(spec, &end, 10);
        if (end == spec || *end != '\0' || errno != 0) {
            emu_usage();
            return 1;
        }
    } else if (!sh->interactive) {
        fputs("nullsh: emu: needs a terminal\n", stderr);
        return 1;
    }

    uint8_t *rom = NULL;
    size_t len = 0;
    const char *why = "cannot read";
    if (rom_read(argv[1], &rom, &len, &why) != NSH_OK) {
        fprintf(stderr, "nullsh: emu: %s: %s\n", argv[1], why);
        return 1;
    }

    Chip8 *c = nsh_malloc(sizeof *c);
    cpu_init(c);
    NshError err = cpu_load_rom(c, rom, len);
    nsh_free(rom);
    if (err != NSH_OK) {
        fprintf(stderr, "nullsh: emu: %s: rom does not fit\n", argv[1]);
        nsh_free(c);
        return 1;
    }

    int status;
    if (spec != NULL) {
        status = run_headless(c, cycles);
    } else if (!signals_install()) {
        fprintf(stderr, "nullsh: emu: sigaction: %s\n", strerror(errno));
        status = 1;
    } else {
        status = run_interactive(c);
        term_exit_raw();
        signals_restore();
    }
    nsh_free(c);
    return status;
}
