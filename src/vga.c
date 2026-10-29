/* VGA mode 12h (640x480, 16 colours, planar) cell renderer for HANEDIT. */
#include <string.h>
#include <conio.h>
#include <i86.h>
#include "vga.h"
#include "font.h"

typedef struct {
    unsigned short ch;
    unsigned char  part;    /* 0 = narrow, 1 = left half of wide, 2 = right half */
    unsigned char  fg, bg, attr;
    unsigned char  hw;      /* 그릴 때 쓴 한글 글꼴: 1 = 넓은 한글, 0 = 좁은 한글 */
} Cell;

static Cell back[SCR_H][SCR_W];
static Cell front[SCR_H][SCR_W];

#define VMEM ((volatile unsigned char *)0xA0000)

/* fallback glyphs */
static const unsigned char box8[16] = {
    0x00,0x00,0x7E,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x7E,0x00,0x00,0x00 };
static const unsigned char blank16[32];

int font_hangul_wide = 1;       /* 지금 글자를 놓을 때 쓰는 한글 글꼴: 1 넓은(두 칸), 0 좁은(한 칸) */

static const unsigned char *find_glyph(const FontRange *rs, int n,
                                       const unsigned char *const *chunks,
                                       unsigned short ch, int *wide)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const FontRange *r = &rs[mid];
        if (ch < r->first) hi = mid - 1;
        else if (ch > r->last) lo = mid + 1;
        else {
            unsigned long off = r->offset + (unsigned long)(ch - r->first) * (r->wide ? 32 : 16);
            *wide = r->wide;
            return chunks[off >> 16] + (off & 0xFFFF);
        }
    }
    return 0;
}

const unsigned char *font_glyph_m(unsigned short ch, int hangul_wide, int *wide)
{
    if (hangul_wide && ch >= 0x3131) {
        const unsigned char *g = find_glyph(font_alt_ranges, font_alt_nranges,
                                            font_alt_chunks, ch, wide);
        if (g) return g;
    }
    return find_glyph(font_ranges, font_nranges, font_chunks, ch, wide);
}

const unsigned char *font_glyph(unsigned short ch, int *wide)
{
    return font_glyph_m(ch, font_hangul_wide, wide);
}

int uwidth_m(unsigned short ch, int hangul_wide)
{
    int wide;
    if (ch < 0x80) return 1;
    if (font_glyph_m(ch, hangul_wide, &wide)) return wide ? 2 : 1;
    /* not in font: guess from Unicode East Asian Wide ranges */
    if ((ch >= 0x1100 && ch <= 0x115F) || (ch >= 0x2E80 && ch <= 0xA4CF) ||
        (ch >= 0xAC00 && ch <= 0xD7A3) || (ch >= 0xF900 && ch <= 0xFAFF) ||
        (ch >= 0xFE30 && ch <= 0xFE4F) || (ch >= 0xFF00 && ch <= 0xFF60) ||
        (ch >= 0xFFE0 && ch <= 0xFFE6))
        return 2;
    return 1;
}

int uwidth(unsigned short ch)
{
    return uwidth_m(ch, font_hangul_wide);
}

void vga_init(void)
{
    union REGS r;
    r.w.ax = 0x0012;
    int386(0x10, &r, &r);
    outpw(0x3CE, 0x0005);   /* write mode 0 */
    outpw(0x3CE, 0x0001);   /* disable set/reset */
    outpw(0x3CE, 0x0003);   /* replace, no rotate */
    outpw(0x3CE, 0xFF08);   /* bit mask all */
    vga_textcolors(0);
    memset(front, 0xFF, sizeof(front));
    memset(back, 0, sizeof(back));
}

/* 16색 번호의 실제 색을 바꿈: 속성 팔레트가 가리키는 DAC 레지스터에 RGB를 씀 */
void vga_setrgb(int color, int r, int g, int b)
{
    union REGS rg;
    int dac;
    rg.w.ax = 0x1007;                /* 속성 팔레트 레지스터 읽기 */
    rg.h.bl = (unsigned char)color;
    int386(0x10, &rg, &rg);
    dac = rg.h.bh;
    outp(0x3C8, dac);
    outp(0x3C9, r >> 2);             /* VGA DAC은 6비트 */
    outp(0x3C9, g >> 2);
    outp(0x3C9, b >> 2);
}

/* 편집 영역의 바탕색·글자색. 두 색 번호의 실제 색만 바꾸므로 화면을 다시
   그리지 않아도 곧바로 바뀜 */
#define TEXT_DARK   0x0C        /* #0C0C0C */
#define TEXT_LIGHT  0xCC        /* #CCCCCC */
void vga_textcolors(int inverted)
{
    int bg = inverted ? TEXT_LIGHT : TEXT_DARK, fg = inverted ? TEXT_DARK : TEXT_LIGHT;
    vga_setrgb(C_TEXTBG, bg, bg, bg);
    vga_setrgb(C_TEXTFG, fg, fg, fg);
}

void vga_done(void)
{
    union REGS r;
    outpw(0x3C4, 0x0F02);
    r.w.ax = 0x0003;
    int386(0x10, &r, &r);
}

void scr_invalidate(void)
{
    memset(front, 0xFF, sizeof(front));
}

/* Break a wide character if one of its halves is being overwritten. */
static void unhook(int x, int y)
{
    Cell *c = &back[y][x];
    if (c->part == 1 && x + 1 < SCR_W) {
        back[y][x + 1].ch = ' '; back[y][x + 1].part = 0;
    } else if (c->part == 2 && x > 0) {
        back[y][x - 1].ch = ' '; back[y][x - 1].part = 0;
    }
}

static void setcell(int x, int y, unsigned short ch, int part, int fg, int bg)
{
    Cell *c;
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) return;
    unhook(x, y);
    c = &back[y][x];
    c->ch = ch; c->part = (unsigned char)part;
    c->fg = (unsigned char)fg; c->bg = (unsigned char)bg; c->attr = 0;
    c->hw = (unsigned char)font_hangul_wide;
}

void scr_put(int x, int y, unsigned short ch, int fg, int bg)
{
    if (uwidth(ch) == 2) {
        if (x + 1 >= SCR_W) { setcell(x, y, ' ', 0, fg, bg); return; }
        setcell(x, y, ch, 1, fg, bg);
        setcell(x + 1, y, ch, 2, fg, bg);
    } else {
        setcell(x, y, ch, 0, fg, bg);
    }
}

unsigned short utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    unsigned c = *s;
    if (c < 0x80) { *ps += 1; return (unsigned short)c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *ps += 2; return (unsigned short)(((c & 0x1F) << 6) | (s[1] & 0x3F));
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *ps += 3;
        return (unsigned short)(((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F));
    }
    *ps += 1;
    return 0xFFFD;
}

int utf8_width(const char *s)
{
    int w = 0;
    while (*s) w += uwidth(utf8_next(&s));
    return w;
}

int scr_puts(int x, int y, const char *s, int fg, int bg)
{
    int x0 = x;
    while (*s && x < SCR_W) {
        unsigned short ch = utf8_next(&s);
        scr_put(x, y, ch, fg, bg);
        x += uwidth(ch);
    }
    return x - x0;
}

int scr_putw(int x, int y, const unsigned short *s, int n, int fg, int bg)
{
    int x0 = x, i;
    for (i = 0; i < n && x < SCR_W; i++) {
        scr_put(x, y, s[i], fg, bg);
        x += uwidth(s[i]);
    }
    return x - x0;
}

void scr_fill(int x, int y, int w, int h, unsigned short ch, int fg, int bg)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            setcell(i, j, ch, 0, fg, bg);
}

void scr_attr(int x, int y, int attr)
{
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) return;
    back[y][x].attr |= (unsigned char)attr;
}

void scr_shadow(int x, int y, int w, int h)
{
    int i, j;
    for (j = y; j < y + h && j < SCR_H; j++)
        for (i = x; i < x + w && i < SCR_W; i++) {
            if (i < 0 || j < 0) continue;
            back[j][i].bg = C_DGRAY;    /* #555555: 본문 바탕보다 밝고 메뉴보다 어두움 */
            back[j][i].fg = C_BLACK;
        }
}

void scr_box(int x, int y, int w, int h, int fg, int bg, int dbl)
{
    unsigned short hz = dbl ? 0x2550 : 0x2500, vt = dbl ? 0x2551 : 0x2502;
    int i;
    scr_fill(x, y, w, h, ' ', fg, bg);
    for (i = x + 1; i < x + w - 1; i++) { scr_put(i, y, hz, fg, bg); scr_put(i, y + h - 1, hz, fg, bg); }
    for (i = y + 1; i < y + h - 1; i++) { scr_put(x, i, vt, fg, bg); scr_put(x + w - 1, i, vt, fg, bg); }
    scr_put(x, y, dbl ? 0x2554 : 0x250C, fg, bg);
    scr_put(x + w - 1, y, dbl ? 0x2557 : 0x2510, fg, bg);
    scr_put(x, y + h - 1, dbl ? 0x255A : 0x2514, fg, bg);
    scr_put(x + w - 1, y + h - 1, dbl ? 0x255D : 0x2518, fg, bg);
}

static void draw_cell(int x, int y, const Cell *c)
{
    unsigned char rows[16];
    const unsigned char *g;
    int wide = 0, r, plane, fg = c->fg, bg = c->bg;
    volatile unsigned char *p;

    g = font_glyph_m(c->ch, c->hw, &wide);
    if (!g) { g = (c->part == 0) ? box8 : blank16; wide = (c->part != 0); }
    for (r = 0; r < 16; r++) {
        if (c->part == 0) rows[r] = wide ? g[r * 2] : g[r];
        else if (wide)    rows[r] = g[r * 2 + (c->part - 1)];
        else              rows[r] = (c->part == 1) ? g[r] : 0;
    }
    if (c->attr & A_UNDER)  rows[15] = 0xFF;
    if (c->attr & A_CURBAR) { rows[13] ^= 0xFF; rows[14] ^= 0xFF; rows[15] ^= 0xFF; }
    if (c->attr & A_CURBLK) for (r = 0; r < 16; r++) rows[r] ^= 0xFF;

    p = VMEM + (unsigned)y * 16 * 80 + x;
    for (plane = 0; plane < 4; plane++) {
        unsigned char fm = (unsigned char)((fg >> plane) & 1 ? 0xFF : 0);
        unsigned char bm = (unsigned char)((bg >> plane) & 1 ? 0xFF : 0);
        outpw(0x3C4, (unsigned short)(((1 << plane) << 8) | 0x02));
        for (r = 0; r < 16; r++)
            p[r * 80] = (unsigned char)((rows[r] & fm) | (~rows[r] & bm));
    }
}

void scr_flush(void)
{
    int x, y;
    for (y = 0; y < SCR_H; y++)
        for (x = 0; x < SCR_W; x++)
            if (memcmp(&back[y][x], &front[y][x], sizeof(Cell)) != 0) {
                draw_cell(x, y, &back[y][x]);
                front[y][x] = back[y][x];
            }
    outpw(0x3C4, 0x0F02);
}
