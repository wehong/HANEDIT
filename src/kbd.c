/* Keyboard handling for HANEDIT.
 *
 * Normal keys are read through BIOS INT 16h (extended functions).
 * The Han/Eng toggle is SHIFT-SPACE. The BIOS stores Shift-Space as a
 * plain space (39h/20h) in its key buffer, so an INT 9 (IRQ1) hook
 * watches the scan codes, tracks the Shift keys itself and counts every
 * Space make code (including typematic repeats) that arrives while a
 * Shift key is down. kbd_get() turns the matching buffered space into
 * K_HANTOGGLE, so the result does not depend on whether Shift is still
 * held when the key is read from the buffer.
 *
 * The RIGHT Alt key also toggles Han/Eng (the Han/Eng key of Korean
 * keyboards sends E0 38, or F2 on some models); both keys always work. The BIOS does not
 * tell left and right Alt apart, so the ISR watches E0 38 / E0 B8 and makes
 * a toggle when right Alt is pressed and released with no other key in
 * between (so right-Alt combinations still work).
 *
 * The Hanja key works the same way: right Ctrl (E0 1D / E0 9D) pressed and
 * released alone, or scan code F1 sent by the Hanja key of some Korean
 * keyboards, gives K_HANJA. Right-Ctrl combinations still work normally.
 *
 * Left Alt pressed and released alone gives K_MENU, which opens the menu as
 * in Hangul word processors.
 *
 * Shift + cursor keys (block selection): the BIOS reports Shift-Left etc.
 * exactly like Left (and the keypad turns Shift-arrows into digits), so the
 * ISR also records, for every cursor-key make code, whether Shift was down
 * at that moment. kbd_get() pairs each buffered cursor key with that record
 * and adds K_SHIFT to the returned code.
 */
#include <dos.h>
#include <conio.h>
#include <i86.h>
#include <bios.h>
#include "kbd.h"

static void (__interrupt __far *old_int9)(void);
static volatile int shsp = 0;           /* pending Shift-Space presses */
static volatile int e0 = 0, lsh = 0, rsh = 0;
static volatile int kb_toggle = 0;      /* pending right-Alt / Han-Eng key toggles */
static volatile int ralt = 0, other = 0;
static volatile int kb_hanja = 0;       /* pending Hanja key presses */
static volatile int rctl = 0, octl = 0;
static volatile int kb_menu = 0;        /* pending Alt taps (open menu) */
static volatile int lalt = 0, oalt = 0;
static int installed = 0;

/* cursor-key make codes with the Shift state at press time */
#define NAVQ 32
static volatile unsigned char navq_sc[NAVQ], navq_sh[NAVQ];
static volatile int navq_head = 0, navq_tail = 0;

/* Shift 상태를 기록해 둘 글쇠: 이동 글쇠, Ins, Del, Z (Ctrl-Shift-Z) */
static int is_nav(int sc)
{
    return sc == 0x47 || sc == 0x48 || sc == 0x49 || sc == 0x4B ||
           sc == 0x4D || sc == 0x4F || sc == 0x50 || sc == 0x51 ||
           sc == 0x52 || sc == 0x53 || sc == 0x2C;
}

static void __interrupt __far kbd_isr(void)
{
    unsigned char c = (unsigned char)inp(0x60);

    if (c == 0xE0) {
        e0 = 1;
    } else if (c == 0xF2) {             /* 한/영 키 (일부 한국어 자판, 떼는 코드 없음) */
        if (kb_toggle < 16) kb_toggle++;
        e0 = 0;
    } else if (c == 0xF1) {             /* 한자 키 (일부 한국어 자판) */
        if (kb_hanja < 16) kb_hanja++;
        e0 = 0;
    } else if (c == 0xE1 || c >= 0xF0) {
        e0 = 0;                         /* pause / ack / resend: ignore */
    } else {
        int brk = (c & 0x80) != 0, sc = c & 0x7F;
        if (!e0) {                      /* E0 2A / E0 36 are fake shifts */
            if (sc == 0x2A) lsh = !brk;
            else if (sc == 0x36) rsh = !brk;
            else if (sc == 0x39 && !brk && (lsh || rsh) && shsp < 16) shsp++;
        }
        if (e0 && sc == 0x38) {         /* 오른쪽 Alt */
            if (!brk) {
                if (!ralt) { ralt = 1; other = 0; }
                octl = 1; oalt = 1;
            } else {
                if (ralt && !other && kb_toggle < 16) kb_toggle++;   /* 혼자 눌렀다 떼면 한/영 전환 */
                ralt = 0;
            }
        } else if (!e0 && sc == 0x38) { /* 왼쪽 Alt: 혼자 눌렀다 떼면 메뉴 */
            if (!brk) {
                if (!lalt) { lalt = 1; oalt = 0; }
                other = 1; octl = 1;
            } else {
                if (lalt && !oalt && kb_menu < 4) kb_menu++;
                lalt = 0;
            }
        } else if (e0 && sc == 0x1D) {  /* 오른쪽 Ctrl = 한자 키 */
            if (!brk) {
                if (!rctl) { rctl = 1; octl = 0; }
                other = 1; oalt = 1;
            } else {
                if (rctl && !octl && kb_hanja < 16) kb_hanja++;
                rctl = 0;
            }
        } else if (!brk && !(e0 && (sc == 0x2A || sc == 0x36))) {
            other = 1;                  /* 다른 글쇠를 함께 누름: 전환 안 함 */
            octl = 1; oalt = 1;
        }
        if (!brk && is_nav(sc)) {
            int n = (navq_head + 1) % NAVQ;
            if (n != navq_tail) {
                navq_sc[navq_head] = (unsigned char)sc;
                navq_sh[navq_head] = (unsigned char)(lsh || rsh);
                navq_head = n;
            }
        }
        e0 = 0;
    }
    _chain_intr(old_int9);
}

void kbd_init(void)
{
    old_int9 = _dos_getvect(0x09);
    _dos_setvect(0x09, kbd_isr);
    installed = 1;
}

void kbd_done(void)
{
    if (installed) _dos_setvect(0x09, old_int9);
    installed = 0;
}


int kbd_hit(void)
{
    if (kb_toggle || kb_hanja || kb_menu) return 1;
    return _bios_keybrd(_NKEYBRD_READY) != 0;
}

/* BIOS scan code of Ctrl/Alt + cursor key -> raw make code */
static int nav_raw(int sc)
{
    switch (sc) {
    case 0x73: case 0x9B: return 0x4B;      /* Ctrl/Alt-Left */
    case 0x74: case 0x9D: return 0x4D;      /* Ctrl/Alt-Right */
    case 0x8D: case 0x98: return 0x48;      /* Ctrl/Alt-Up */
    case 0x91: case 0xA0: return 0x50;      /* Ctrl/Alt-Down */
    case 0x77: case 0x97: return 0x47;      /* Ctrl/Alt-Home */
    case 0x75: case 0x9F: return 0x4F;      /* Ctrl/Alt-End */
    case 0x84: case 0x99: return 0x49;      /* Ctrl/Alt-PgUp */
    case 0x76: case 0xA1: return 0x51;      /* Ctrl/Alt-PgDn */
    case 0x92: case 0xA2: return 0x52;      /* Ctrl/Alt-Ins */
    case 0x93: case 0xA3: return 0x53;      /* Ctrl/Alt-Del */
    }
    return is_nav(sc) ? sc : 0;
}

/* Shift state recorded by the ISR for this cursor key */
static int nav_shift(int raw)
{
    int sh = -1;
    _disable();
    while (navq_tail != navq_head) {
        int sc = navq_sc[navq_tail], s = navq_sh[navq_tail];
        navq_tail = (navq_tail + 1) % NAVQ;
        if (sc == raw) { sh = s; break; }   /* skip stale entries */
    }
    _enable();
    return sh >= 0 ? sh : (kbd_shift() != 0);
}

int kbd_get(void)
{
    unsigned k, sc, ch;
    int raw;
    for (;;) {
        if (kb_toggle) {
            _disable();
            if (kb_toggle) { kb_toggle--; _enable(); return K_HANTOGGLE; }
            _enable();
        }
        if (kb_hanja) {
            _disable();
            if (kb_hanja) { kb_hanja--; _enable(); return K_HANJA; }
            _enable();
        }
        if (kb_menu) {
            _disable();
            if (kb_menu) { kb_menu--; _enable(); return K_MENU; }
            _enable();
        }
        if (_bios_keybrd(_NKEYBRD_READY)) break;
        kbd_idle();
    }
    k = _bios_keybrd(_NKEYBRD_READ);
    sc = (k >> 8) & 0xFF;
    ch = k & 0xFF;
    if (sc == 0x39 && ch == ' ') {
        int t;
        _disable();
        t = shsp;
        if (t) shsp = t - 1;
        _enable();
        if (t) return K_HANTOGGLE;
    }
    raw = nav_raw(sc);
    if (raw) {
        int sh = nav_shift(raw);        /* 기록은 늘 꺼냄 (차례가 어긋나지 않게) */
        if (raw == 0x2C) {              /* Z */
            if (ch == 0x1A && sh) return 0x1A | K_SHIFT;  /* Ctrl-Shift-Z */
        } else if (sc >= 0x80 || ch == 0 || ch == 0xE0 ||
                   (ch >= '0' && ch <= '9') || ch == '.') {
            if (sh) return K_EXT(sc) | K_SHIFT;   /* 숫자판 숫자도 이동 글쇠로 */
            if ((ch >= '0' && ch <= '9') || ch == '.') return (int)ch;  /* Num Lock 숫자 */
            return K_EXT(sc);
        }
    }
    if ((ch == 0 || ch == 0xE0) && sc != 0) return K_EXT(sc);
    return (int)ch;
}

int kbd_shift(void)
{
    return (*(volatile unsigned char *)0x417) & 0x03;
}

unsigned long kbd_ticks(void)
{
    return *(volatile unsigned long *)0x46C;
}

void kbd_idle(void)
{
    union REGS r;
    r.w.ax = 0x1680;            /* release time slice (Windows/DOSBox/EMM friendly) */
    int386(0x2F, &r, &r);
}
