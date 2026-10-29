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

static void pal_hide(void);
static void pal_show(void);
static int  pal_hidden;              /* 1: 첫 화면을 그리는 중이라 감춰 둠 */
static unsigned char dacidx[16];

void vga_init(void)
{
    union REGS r;
    r.w.ax = 0x0012;
    int386(0x10, &r, &r);
    outpw(0x3CE, 0x0005);   /* write mode 0 */
    outpw(0x3CE, 0x0001);   /* disable set/reset */
    outpw(0x3CE, 0x0003);   /* replace, no rotate */
    outpw(0x3CE, 0xFF08);   /* bit mask all */
    {   /* 16색 번호 → DAC 번호 (속성 팔레트). 화면을 감춘 동안 BIOS를 부르지 않으려고 미리 읽어 둠 */
        int i;
        for (i = 0; i < 16; i++) {
            r.w.ax = 0x1007;
            r.h.bl = (unsigned char)i;
            int386(0x10, &r, &r);
            dacidx[i] = r.h.bh;
        }
    }
    vga_textcolors(0);
    memset(front, 0xFF, sizeof(front));
    memset(back, 0, sizeof(back));
    pal_hide();                 /* 첫 화면은 다 그린 뒤에 한 번에 보임 */
}

/* 16색 번호의 실제 색을 바꿈: 속성 팔레트가 가리키는 DAC 레지스터에 RGB를 씀 */
void vga_setrgb(int color, int r, int g, int b)
{
    outp(0x3C8, dacidx[color & 15]);
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
    if (pal_hidden) pal_show();
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

/* 칸 하나의 16줄 그림 (글자 + 밑줄·커서). 모두 0이면(빈칸) 0 */
static const unsigned char *cell_rows(const Cell *c, unsigned char *tmp)
{
    const unsigned char *g;
    int wide = 0, r, step = 1;
    if (c->ch == ' ' && c->part == 0 && !c->attr) return 0;     /* 빈칸 (가장 흔함) */
    g = font_glyph_m(c->ch, c->hw, &wide);
    if (!g) { g = (c->part == 0) ? box8 : blank16; wide = (c->part != 0); }
    if (wide) { step = 2; if (c->part == 2) g++; }               /* 넓은 글자: 왼쪽/오른쪽 반 */
    else if (c->part == 2) g = blank16;
    if (step == 1 && !c->attr) return g;                        /* 그대로 씀 (복사 안 함) */
    for (r = 0; r < 16; r++, g += step) tmp[r] = *g;
    if (c->attr & A_UNDER)  tmp[15] = 0xFF;
    if (c->attr & A_CURBAR) { tmp[13] ^= 0xFF; tmp[14] ^= 0xFF; tmp[15] ^= 0xFF; }
    if (c->attr & A_CURBLK) for (r = 0; r < 16; r++) tmp[r] ^= 0xFF;
    return tmp;
}

/* 첫 화면은 VGA 출력을 꺼 둔 채 그리고, 다 그린 뒤 한 번에 보임 */
static void pal_hide(void) { inp(0x3DA); outp(0x3C0, 0x00); pal_hidden = 1; }  /* 속성 제어기 PAS=0: 화면 끔 */
static void pal_show(void) { inp(0x3DA); outp(0x3C0, 0x20); pal_hidden = 0; }  /* PAS=1: 화면 켬 */

/* 바뀐 칸만 그림. 빠르게 그리려고
   1) 바탕: 세트/리셋으로 네 평면을 한꺼번에, 같은 바탕색이 이어진 칸은 줄마다 memset 한 번
   2) 글자가 있는 칸만 평면마다 그림 (빈칸은 1로 끝) */
static unsigned char dirty[SCR_H][SCR_W], rowdirty[SCR_H];

void scr_flush(void)
{
    int x, y, x0, r;
    unsigned char rows[16];

    /* 0) 바뀐 칸 표시 */
    for (y = 0; y < SCR_H; y++) {
        int any = 0;
        for (x = 0; x < SCR_W; x++) {
            const Cell *b = &back[y][x], *f = &front[y][x];
            int d = b->ch != f->ch || b->fg != f->fg || b->bg != f->bg || b->attr != f->attr ||
                    b->part != f->part || b->hw != f->hw;
            dirty[y][x] = (unsigned char)d;
            any |= d;
        }
        rowdirty[y] = (unsigned char)any;
    }

    outpw(0x3C4, 0x0F02);               /* 네 평면 모두 */
    outpw(0x3CE, 0x0F01);               /* 세트/리셋 켬 */
    outpw(0x3CE, 0xFF08);               /* 비트 마스크 모두 */
    for (y = 0; y < SCR_H; y++) {
        volatile unsigned char *row0 = VMEM + (unsigned)y * 16 * 80;
        const unsigned char *dr = dirty[y];
        if (!rowdirty[y]) continue;
        /* 1) 바탕 */
        outpw(0x3CE, 0xFF08);
        for (x = 0; x < SCR_W; ) {
            int bg;
            if (!dr[x]) { x++; continue; }
            bg = back[y][x].bg;
            x0 = x;
            while (x < SCR_W && dr[x] && back[y][x].bg == bg) x++;
            outpw(0x3CE, (unsigned short)((bg << 8) | 0x00));
            for (r = 0; r < 16; r++)
                memset((void *)(row0 + r * 80 + x0), 0xFF, x - x0);
        }
        /* 2) 글자: 평면마다 (글자 점은 글자색, 나머지는 바탕색) */
        outpw(0x3CE, 0x0001);           /* 세트/리셋 끔 */
        for (x = 0; x < SCR_W; x++) {
            Cell *c = &back[y][x];
            volatile unsigned char *p = row0 + x, *q;
            const unsigned char *rw;
            unsigned char v, xm;
            int plane;
            if (!dr[x]) continue;
            front[y][x] = *c;
            if ((rw = cell_rows(c, rows)) == 0) continue;   /* 빈칸: 바탕만으로 끝 */
            /* 바탕은 이미 칠했으므로 글자색과 바탕색이 다른 평면만, 빈 줄은 건너뜀 */
            for (plane = 0; plane < 4; plane++) {
                int fb = (c->fg >> plane) & 1, bb = (c->bg >> plane) & 1;
                if (fb == bb) continue;
                outpw(0x3C4, (unsigned short)(((1 << plane) << 8) | 0x02));
                /* 글자색 1·바탕색 0 평면은 글자 모양 그대로, 반대면 뒤집어서.
                   (VGA 메모리에 쓴 뒤 그 자리에서 뒤집으면 읽기가 끼어 틀리므로 값을 먼저 만듦) */
                xm = fb ? 0 : 0xFF;
                q = p;
                for (r = 0; r < 16; r++, q += 80)
                    if (rw[r]) { v = (unsigned char)(rw[r] ^ xm); *q = v; }
            }
        }
        outpw(0x3C4, 0x0F02);
        outpw(0x3CE, 0x0F01);           /* 다음 줄 바탕을 위해 세트/리셋 켬 */
    }
    outpw(0x3CE, 0x0001);               /* 원래대로: 세트/리셋 끔 */
    outpw(0x3CE, 0x0000);
    outpw(0x3CE, 0xFF08);
    if (pal_hidden) pal_show();
}
