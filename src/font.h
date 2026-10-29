#ifndef FONT_H
#define FONT_H

typedef struct {
    unsigned short first, last;
    unsigned char  wide;        /* 1 = 16x16, 0 = 8x16 */
    unsigned long  offset;      /* byte offset into font_data */
} FontRange;

extern const FontRange font_ranges[];
extern const int font_nranges;
extern const unsigned char *const font_chunks[];   /* 64K pieces */

/* 넓은 한글(갈무리14, 두 칸) 표. font_hangul_wide 는 지금 글자를 놓을 때 쓰는
   한글 글꼴 (1 넓은, 0 좁은). 메뉴·대화 상자는 늘 1, 본문은 글꼴 메뉴 설정을 따름 */
extern const FontRange font_alt_ranges[];
extern const int font_alt_nranges;
extern const unsigned char *const font_alt_chunks[];
extern int font_hangul_wide;
const unsigned char *font_glyph_m(unsigned short ch, int hangul_wide, int *wide);

/* Returns glyph bitmap (16 or 32 bytes) or NULL; *wide receives width flag. */
const unsigned char *font_glyph(unsigned short ch, int *wide);

#endif
