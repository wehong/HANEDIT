#ifndef KBD_H
#define KBD_H

/* Key codes: printable ASCII as-is (1..255), extended keys as 0x100 | scancode */
#define K_EXT(sc)   (0x100 | (sc))
#define K_UP        K_EXT(0x48)
#define K_DOWN      K_EXT(0x50)
#define K_LEFT      K_EXT(0x4B)
#define K_RIGHT     K_EXT(0x4D)
#define K_HOME      K_EXT(0x47)
#define K_END       K_EXT(0x4F)
#define K_PGUP      K_EXT(0x49)
#define K_PGDN      K_EXT(0x51)
#define K_INS       K_EXT(0x52)
#define K_DEL       K_EXT(0x53)
#define K_CLEFT     K_EXT(0x73)
#define K_CRIGHT    K_EXT(0x74)
#define K_CHOME     K_EXT(0x77)
#define K_CEND      K_EXT(0x75)
#define K_CPGUP     K_EXT(0x84)
#define K_CPGDN     K_EXT(0x76)
#define K_F(n)      K_EXT(0x3A + (n))      /* F1..F10 */
#define K_ALT_N     K_EXT(0x31)
#define K_ALT_O     K_EXT(0x18)
#define K_ALT_S     K_EXT(0x1F)
#define K_ALT_V     K_EXT(0x2F)
#define K_ALT_X     K_EXT(0x2D)
#define K_ENTER     13
#define K_BKSP      8
#define K_TAB       9
#define K_ESC       27
#define K_HANTOGGLE 0x1000                 /* Shift-Space */
#define K_SHIFT     0x2000                 /* flag: cursor key pressed with Shift */
#define K_HANJA     0x1001                 /* 한자 키: 오른쪽 Ctrl만 눌렀다 뗌, 또는 F1 코드 */
#define K_MENU      0x1002                 /* Alt만 눌렀다 뗌: 메뉴 열기 */
#define K_CINS      K_EXT(0x92)            /* Ctrl-Insert */

/* 한/영 전환 글쇠 */
#define HK_SHIFTSPACE 0                    /* Shift-Space */
#define HK_RALT       1                    /* 오른쪽 Alt (한국어 자판의 한/영 키) */

void kbd_init(void);
void kbd_set_hankey(int mode);
void kbd_done(void);
int  kbd_hit(void);           /* key waiting in BIOS buffer or han/eng toggle pending */
int  kbd_get(void);           /* blocking, no blink handling */
int  kbd_shift(void);         /* non-zero if a Shift key is down */
unsigned long kbd_ticks(void);/* BIOS 18.2Hz tick counter */
void kbd_idle(void);          /* give up time slice */

#endif
