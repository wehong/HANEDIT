#ifndef VGA_H
#define VGA_H

/* 640x480x16 (VGA mode 12h) text-cell renderer: 80 x 30 cells of 8x16 px. */

#define SCR_W 80
#define SCR_H 30

/* cell attributes */
#define A_UNDER   0x01   /* underline (row 15) */
#define A_CURBAR  0x02   /* insert cursor: invert rows 13..15 */
#define A_CURBLK  0x04   /* overwrite cursor / composing block: invert all */

/* standard 16 colours */
enum { C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_LGRAY,
       C_DGRAY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE };

/* 편집 영역 색: 쓰지 않는 5번(자홍)·6번(갈색)을 DAC에서 다시 정의해 씀 */
#define C_TEXTBG  C_MAGENTA     /* #0C0C0C 거의 검정 */
#define C_TEXTFG  C_BROWN       /* #CCCCCC 밝은 회색 */

void vga_init(void);
void vga_setrgb(int color, int r, int g, int b);   /* 0..255 */
void vga_textcolors(int inverted);  /* 0: 어두운 바탕/밝은 글자, 1: 맞바꿈 */
void vga_done(void);

int  uwidth(unsigned short ch);                 /* 1 or 2 cells (지금 한글 글꼴 기준) */
int  uwidth_m(unsigned short ch, int hangul_wide);  /* 한글 글꼴을 정해서 */
void scr_put(int x, int y, unsigned short ch, int fg, int bg);
int  scr_puts(int x, int y, const char *utf8, int fg, int bg);   /* returns cells used */
int  scr_putw(int x, int y, const unsigned short *s, int n, int fg, int bg);
void scr_fill(int x, int y, int w, int h, unsigned short ch, int fg, int bg);
void scr_attr(int x, int y, int attr);          /* OR attribute into cell */
void scr_shadow(int x, int y, int w, int h);    /* darken cells (dialog shadow) */
void scr_box(int x, int y, int w, int h, int fg, int bg, int dbl);
void scr_flush(void);                           /* draw changed cells */
void scr_invalidate(void);

int  utf8_width(const char *s);
unsigned short utf8_next(const char **ps);

#endif
