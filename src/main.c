/*
 * HANEDIT - VGA 그래픽 모드 한글 텍스트 편집기 (MS-DOS)
 *
 *  - 640x480x16 (VGA mode 12h), 80x30 글자 칸
 *  - 한글 입력: libhangul 오토마타 (두벌식 / 세벌식 최종)
 *  - 한/영 전환: Shift-Space, 오른쪽 Alt (한/영 키) 둘 다
 *  - 파일 형식: UTF-8 텍스트 (BOM 없음, 원래 줄바꿈 방식 유지)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <dos.h>
#include <direct.h>
#include "vga.h"
#include "font.h"
#include "kbd.h"
#include "hangul.h"

#define VERSION   "1.0.1"

#define MENU_Y    1             /* 메뉴 줄: 제목 줄 아래 */
#define TITLE_Y   0             /* 제목 줄: 맨 위 */
#define RULER_Y   2
#define EDIT_Y    3
#define EDIT_H    26
#define STATUS_Y  29            /* 맨 아래: 상태 줄 (알림도 여기에 잠깐 나옴) */

#define TEXT_FG   C_TEXTFG     /* 어두운 바탕에 밝은 글자 */
#define TEXT_BG   C_TEXTBG

typedef unsigned short wch;
typedef struct { wch *s; int len, cap; } Line;

/* ---------------------------------------------------------------- 문서 */
static Line *lines;
static int   nlines, caplines;
static int   cx, cy;            /* 커서: 줄 번호, 줄 안의 글자 위치 */
static int   top, left;         /* 화면 맨 위 줄, 가로 스크롤(표시 칸) */
static int   top_row;           /* 자동 줄 바꿈: top 줄의 몇째 화면 줄부터 보이나 */
static int   wrap_on = 0;       /* 자동 줄 바꿈 (설정) */
static int   goal = -1;         /* 위/아래 이동 시 유지할 표시 칸 */
static int   sel_on, sel_y, sel_x;  /* 블록: 기준점(sel_y, sel_x) ~ 커서(cy, cx) */
static int   sel_sticky;        /* 1: F3 블록 상태 — Shift 없이 옮겨도 블록이 넓어짐 */
static int   insmode = 1, hanmode = 0, modified = 0, crlf = 1;
static char  filename[80];

/* ---------------------------------------------------------- 한글 입력 */
static HangulInputContext *ic;
static int   kbdtype = 0;       /* 0 = 두벌식, 1 = 세벌식 최종 */
static wch   pre[8];            /* 조합 중인 글자 (preedit) */
static int   prelen;

/* ------------------------------------------------------------- 화면 */
static int   blink_on = 1;
static char  msg[200];          /* 한 번 보여 주는 알림 */
static void (*overlay)(void);   /* 대화 상자 그리기 */

static wch   findbuf[64];
static int   findlen;

/* ------------------------------------------------------------ 되돌리기 */
/* 편집 한 번 = "(y,x) 자리에서 del 을 지우고 ins 를 넣음". 줄바꿈은 '\n'. */
typedef struct {
    int   y, x;                 /* 바뀐 자리 (앞쪽) */
    wch  *del; long ndel;       /* 지운 글 */
    wch  *ins; long nins;       /* 넣은 글 */
    int   y0, x0;               /* 편집 전 커서 */
    int   kind;
} Undo;
enum { UK_TYPE = 1, UK_BKSP, UK_DEL, UK_OTHER };
#define UNDO_MAX 1000
static Undo *undo;
static int   nundo;             /* undo[0..nundo): 되돌릴 수 있는 기록 */
static int   nredo;             /* undo[nundo..nundo+nredo): 다시 실행할 수 있는 기록 */
static int   undo_seal = 1;     /* 1이면 다음 편집을 앞 기록에 합치지 않음 */
static int   save_point = 0;    /* 저장했을 때의 nundo (-1: 되돌아갈 수 없음) */

static void undo_clear(void)
{
    int i;
    for (i = 0; i < nundo + nredo; i++) { free(undo[i].del); free(undo[i].ins); }
    nundo = nredo = 0; save_point = 0; undo_seal = 1;
}

static void fatal(const char *s)
{
    kbd_done();
    vga_done();
    fprintf(stderr, "HANEDIT: %s\n", s);
    exit(1);
}

/* ------------------------------------------------------------ 설정 */
static int  text_wide = 1;      /* 본문 한글: 1 넓은 한글(두 칸, 기본), 0 좁은 한글(한 칸) */
static int  textinv = 0;        /* 1: 본문의 글자색과 바탕색을 맞바꾼 상태 */


/* 본문 글자의 칸 수 (본문 한글 글꼴 기준) */
static int tw(wch c) { return uwidth_m(c, text_wide); }

/* ============================================================ 줄 버퍼 */
static void line_reserve(Line *l, int n)
{
    if (n > l->cap) {
        int c = l->cap ? l->cap : 16;
        while (c < n) c *= 2;
        l->s = (wch *)realloc(l->s, c * sizeof(wch));
        if (!l->s) fatal("out of memory");
        l->cap = c;
    }
}

static void line_ins(Line *l, int pos, const wch *src, int n)
{
    line_reserve(l, l->len + n);
    memmove(l->s + pos + n, l->s + pos, (l->len - pos) * sizeof(wch));
    memcpy(l->s + pos, src, n * sizeof(wch));
    l->len += n;
}

static void line_del(Line *l, int pos, int n)
{
    if (pos + n > l->len) n = l->len - pos;
    if (n <= 0) return;
    memmove(l->s + pos, l->s + pos + n, (l->len - pos - n) * sizeof(wch));
    l->len -= n;
}

static void lines_insert(int idx)
{
    if (nlines == caplines) {
        caplines = caplines ? caplines * 2 : 256;
        lines = (Line *)realloc(lines, caplines * sizeof(Line));
        if (!lines) fatal("out of memory");
    }
    memmove(&lines[idx + 1], &lines[idx], (nlines - idx) * sizeof(Line));
    memset(&lines[idx], 0, sizeof(Line));
    nlines++;
}

static void lines_remove(int idx)
{
    free(lines[idx].s);
    memmove(&lines[idx], &lines[idx + 1], (nlines - idx - 1) * sizeof(Line));
    nlines--;
}

static void doc_clear(void)
{
    int i;
    for (i = 0; i < nlines; i++) free(lines[i].s);
    nlines = 0;
    lines_insert(0);
    undo_clear();
    cx = cy = top = left = top_row = 0;
    goal = -1;
    modified = 0;
    crlf = 1;
}

static int dcol(const Line *l, int n)
{
    int i, c = 0;
    for (i = 0; i < n && i < l->len; i++) c += tw(l->s[i]);
    return c;
}

static int col_to_idx(const Line *l, int col)
{
    int i, c = 0;
    for (i = 0; i < l->len; i++) {
        int w = tw(l->s[i]);
        if (c + w > col) break;
        c += w;
    }
    return i;
}

/* ======================================================== UTF-8 파일 */
static wch utf8_dec(const unsigned char **pp, const unsigned char *e, int *bad)
{
    const unsigned char *p = *pp;
    unsigned c = *p;
    if (c < 0x80) { *pp = p + 1; return (wch)c; }
    if ((c & 0xE0) == 0xC0 && c >= 0xC2 && p + 1 < e && (p[1] & 0xC0) == 0x80) {
        *pp = p + 2; return (wch)(((c & 0x1F) << 6) | (p[1] & 0x3F));
    }
    if ((c & 0xF0) == 0xE0 && p + 2 < e && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *pp = p + 3;
        return (wch)(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
    }
    if ((c & 0xF8) == 0xF0 && p + 3 < e && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *pp = p + 4; return 0xFFFD;            /* BMP 밖 문자 */
    }
    *pp = p + 1;
    (*bad)++;
    return 0xFFFD;
}

static int doc_load(const char *fn)
{
    FILE *f = fopen(fn, "rb");
    long size;
    unsigned char *buf;
    const unsigned char *p, *e;
    int col = 0, sawcrlf = 0, sawlf = 0, bad = 0;
    Line *l;

    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (unsigned char *)malloc(size + 1);
    if (!buf) { fclose(f); return -2; }
    if (size > 0 && fread(buf, 1, size, f) != (size_t)size) { free(buf); fclose(f); return -3; }
    fclose(f);

    doc_clear();
    l = &lines[0];
    p = buf; e = buf + size;
    if (size >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3;
    while (p < e) {
        wch ch;
        if (*p == '\r' || *p == '\n') {
            if (*p == '\r' && p + 1 < e && p[1] == '\n') { p += 2; sawcrlf = 1; }
            else { if (*p == '\n') sawlf = 1; p++; }
            lines_insert(nlines);
            l = &lines[nlines - 1];
            col = 0;
            continue;
        }
        ch = utf8_dec(&p, e, &bad);
        if (ch == '\t') {
            static const wch sp[8] = { ' ',' ',' ',' ',' ',' ',' ',' ' };
            int n = 8 - (col % 8);
            line_ins(l, l->len, sp, n);
            col += n;
        } else if (ch == 0x1A && p == e) {
            /* 끝의 Ctrl-Z(EOF) 무시 */
        } else {
            line_ins(l, l->len, &ch, 1);
            col += tw(ch);
        }
    }
    free(buf);
    crlf = sawcrlf || !sawlf;
    modified = 0;
    if (bad) sprintf(msg, "주의: UTF-8이 아닌 바이트 %d개를 '?'로 바꾸었습니다", bad);
    return 0;
}

static int put_utf8(FILE *f, wch c)
{
    if (c < 0x80) return fputc(c, f);
    if (c < 0x800) {
        fputc(0xC0 | (c >> 6), f);
        return fputc(0x80 | (c & 0x3F), f);
    }
    fputc(0xE0 | (c >> 12), f);
    fputc(0x80 | ((c >> 6) & 0x3F), f);
    return fputc(0x80 | (c & 0x3F), f);
}

static int doc_save(const char *fn)
{
    FILE *f = fopen(fn, "wb");
    int i, j, err;
    if (!f) return -1;
    for (i = 0; i < nlines; i++) {
        for (j = 0; j < lines[i].len; j++) put_utf8(f, lines[i].s[j]);
        if (i < nlines - 1) fputs(crlf ? "\r\n" : "\n", f);
    }
    err = ferror(f);
    if (fclose(f) != 0) err = 1;
    if (err) return -2;
    modified = 0;
    save_point = nundo;         /* 여기까지 되돌리면 제목 줄의 '*' 표시가 사라짐 */
    undo_seal = 1;
    return 0;
}

/* ========================================================== 한글 입력 */
typedef void (*InsFn)(void *ctx, wch c);

static void han_commit(const ucschar *s, InsFn fn, void *ctx)
{
    for (; s && *s; s++) fn(ctx, (wch)(*s <= 0xFFFF ? *s : 0xFFFD));
}

static void han_update_pre(void)
{
    const ucschar *p = hangul_ic_get_preedit_string(ic);
    prelen = 0;
    while (p && p[prelen] && prelen < 8) { pre[prelen] = (wch)p[prelen]; prelen++; }
}

static void han_flush(InsFn fn, void *ctx)
{
    if (!hangul_ic_is_empty(ic)) han_commit(hangul_ic_flush(ic), fn, ctx);
    prelen = 0;
}

/* 글쇠 하나 처리. 한글 모드면 libhangul 오토마타에 넘긴다. */
static void han_type(int c, InsFn fn, void *ctx)
{
    if (hanmode && c > ' ' && c < 127) {
        int k = c;
        if (isalpha(k)) k = kbd_shift() ? toupper(k) : tolower(k);  /* Caps Lock 무시 */
        if (hangul_ic_process(ic, k)) {
            han_commit(hangul_ic_get_commit_string(ic), fn, ctx);
            han_update_pre();
            return;
        }
        han_commit(hangul_ic_get_commit_string(ic), fn, ctx);
        han_flush(fn, ctx);
        fn(ctx, (wch)c);
    } else {
        han_flush(fn, ctx);
        fn(ctx, (wch)c);
    }
}

/* 조합 중이면 한 자모 지우고 1, 아니면 0 */
static int han_backspace(void)
{
    if (prelen && hangul_ic_backspace(ic)) {
        han_update_pre();
        return 1;
    }
    return 0;
}

static void set_keyboard(int t)
{
    hangul_ic_reset(ic);
    prelen = 0;
    kbdtype = t;
    hangul_ic_select_keyboard(ic, t ? "3f" : "2");
}

/* ============================================================ 블록 */
static long range_len(int y1, int x1, int y2, int x2);
/* 블록 범위를 앞(y1,x1) ~ 뒤(y2,x2) 순서로. 비어 있으면 0 */
static int sel_range(int *y1, int *x1, int *y2, int *x2)
{
    if (!sel_on) return 0;
    if (sel_y < cy || (sel_y == cy && sel_x < cx)) {
        *y1 = sel_y; *x1 = sel_x; *y2 = cy; *x2 = cx;
    } else {
        *y1 = cy; *x1 = cx; *y2 = sel_y; *x2 = sel_x;
    }
    if (*x1 > lines[*y1].len) *x1 = lines[*y1].len;
    if (*x2 > lines[*y2].len) *x2 = lines[*y2].len;
    return *y1 != *y2 || *x1 != *x2;
}

/* 블록 안의 글자 수 (줄바꿈은 1자로 셈) */
static long sel_count(void)
{
    int y1, x1, y2, x2;
    if (!sel_range(&y1, &x1, &y2, &x2)) return 0;
    return range_len(y1, x1, y2, x2);
}

/* ======================================================= 범위 편집 */
/* (y1,x1) ~ (y2,x2) 범위의 글자 수 (줄바꿈은 1자) */
static long range_len(int y1, int x1, int y2, int x2)
{
    long n;
    int y;
    if (y1 == y2) return x2 - x1;
    n = (lines[y1].len - x1) + 1 + x2;
    for (y = y1 + 1; y < y2; y++) n += lines[y].len + 1;
    return n;
}

static void range_get(int y1, int x1, int y2, int x2, wch *out)
{
    int y;
    long k = 0;
    for (y = y1; y <= y2; y++) {
        int a = (y == y1) ? x1 : 0, b = (y == y2) ? x2 : lines[y].len;
        memcpy(out + k, lines[y].s + a, (b - a) * sizeof(wch));
        k += b - a;
        if (y < y2) out[k++] = '\n';
    }
}

static void raw_delete(int y1, int x1, int y2, int x2)
{
    int y;
    if (y1 == y2) {
        line_del(&lines[y1], x1, x2 - x1);
    } else {
        Line *a = &lines[y1], *b = &lines[y2];
        a->len = x1;
        line_ins(a, x1, b->s + x2, b->len - x2);
        for (y = y2; y > y1; y--) lines_remove(y);
    }
}

/* (y,x) 자리에 글을 끼워 넣고 끝 자리를 (*ey,*ex)로 */
static void raw_insert(int y, int x, const wch *t, long n, int *ey, int *ex)
{
    long i = 0;
    while (i < n) {
        long j = i;
        while (j < n && t[j] != '\n') j++;
        line_ins(&lines[y], x, t + i, (int)(j - i));
        x += (int)(j - i);
        if (j < n) {                        /* 줄 나누기 */
            Line *l;
            lines_insert(y + 1);
            l = &lines[y];
            line_ins(&lines[y + 1], 0, l->s + x, l->len - x);
            l->len = x;
            y++; x = 0;
        }
        i = j + 1;
    }
    *ey = y; *ex = x;
}

static wch *wcat(wch *a, long na, const wch *b, long nb)   /* a 뒤에 b를 붙인 새 버퍼 */
{
    wch *r;
    if (na + nb == 0) { free(a); return 0; }
    r = (wch *)realloc(a, (na + nb) * sizeof(wch));
    if (!r) fatal("out of memory");
    memcpy(r + na, b, nb * sizeof(wch));
    return r;
}

/* 모든 문서 편집은 여기를 거침: 범위를 지우고 글을 넣은 뒤 되돌리기 기록을 남김 */
static void edit_replace(int y1, int x1, int y2, int x2, const wch *ins, long nins, int kind)
{
    long nd = range_len(y1, x1, y2, x2);
    wch *d = 0;
    int oy = cy, ox = cx, ey, ex;
    Undo *p;

    if (nd) {
        d = (wch *)malloc(nd * sizeof(wch));
        if (!d) fatal("out of memory");
        range_get(y1, x1, y2, x2, d);
    }
    raw_delete(y1, x1, y2, x2);
    raw_insert(y1, x1, ins, nins, &ey, &ex);
    cy = ey; cx = ex; goal = -1;
    modified = 1;

    if (nredo) {                        /* 새로 고치면 '다시 실행' 기록은 버림 */
        int i;
        for (i = nundo; i < nundo + nredo; i++) { free(undo[i].del); free(undo[i].ins); }
        if (save_point > nundo) save_point = -1;    /* 저장한 상태로는 못 돌아감 */
        nredo = 0;
        undo_seal = 1;
    }

    /* 이어지는 입력/지우기는 기록 하나로 합침 (낱말 단위로 되돌려짐) */
    p = (!undo_seal && nundo > 0) ? &undo[nundo - 1] : 0;
    if (p && p->kind == kind) {
        if (kind == UK_TYPE && y1 == y2 && p->y == y1 && p->x + p->nins == x1 &&
            !(p->nins && p->ins[p->nins - 1] == ' ' && ins[0] != ' ')) {
            p->ins = wcat(p->ins, p->nins, ins, nins); p->nins += nins;
            p->del = wcat(p->del, p->ndel, d, nd);     p->ndel += nd;
            free(d);
            return;
        }
        if (kind == UK_BKSP && y2 == p->y && x2 == p->x) {
            d = wcat(d, nd, p->del, p->ndel);
            free(p->del);
            p->del = d; p->ndel += nd;
            p->y = y1; p->x = x1;
            return;
        }
        if (kind == UK_DEL && y1 == p->y && x1 == p->x) {
            p->del = wcat(p->del, p->ndel, d, nd); p->ndel += nd;
            free(d);
            return;
        }
    }

    if (nundo == UNDO_MAX) {            /* 가장 오래된 기록을 버림 */
        free(undo[0].del); free(undo[0].ins);
        memmove(undo, undo + 1, (UNDO_MAX - 1) * sizeof(Undo));
        nundo--;
        save_point = save_point > 0 ? save_point - 1 : -1;
    }
    if (!undo) {
        undo = (Undo *)malloc(UNDO_MAX * sizeof(Undo));
        if (!undo) fatal("out of memory");
    }
    p = &undo[nundo++];
    p->y = y1; p->x = x1;
    p->del = d; p->ndel = nd;
    p->ins = 0; p->nins = nins;
    if (nins) {
        p->ins = (wch *)malloc(nins * sizeof(wch));
        if (!p->ins) fatal("out of memory");
        memcpy(p->ins, ins, nins * sizeof(wch));
    }
    p->y0 = oy; p->x0 = ox;
    p->kind = kind;
    undo_seal = (kind == UK_OTHER);
}

/* (y,x)에서 시작하는 글 t의 끝 자리 */
static void text_end(int y, int x, const wch *t, long n, int *ey, int *ex)
{
    long i;
    for (i = 0; i < n; i++)
        if (t[i] == '\n') { y++; x = 0; } else x++;
    *ey = y; *ex = x;
}

static void cursor_clamp(void)
{
    if (cy >= nlines) cy = nlines - 1;
    if (cx > lines[cy].len) cx = lines[cy].len;
}

/* Ctrl-Z: 마지막 편집을 거꾸로 (넣은 글을 지우고 지운 글을 되살림) */
static void undo_do(void)
{
    Undo *u;
    int ey, ex, a, b;
    if (!nundo) { strcpy(msg, "더 되돌릴 것이 없습니다."); return; }
    u = &undo[--nundo];
    nredo++;
    text_end(u->y, u->x, u->ins, u->nins, &ey, &ex);
    raw_delete(u->y, u->x, ey, ex);
    raw_insert(u->y, u->x, u->del, u->ndel, &a, &b);
    cy = u->y0; cx = u->x0;             /* 편집 전 커서 자리 */
    cursor_clamp();
    sel_on = 0; goal = -1; undo_seal = 1;
    modified = (nundo != save_point);
    if (!nundo) strcpy(msg, "처음 상태까지 되돌렸습니다. Ctrl-Y: 다시 실행");
}

/* Ctrl-Y: 되돌린 편집을 다시 (지운 글을 다시 지우고 넣은 글을 다시 넣음) */
static void redo_do(void)
{
    Undo *u;
    int ey, ex;
    if (!nredo) { strcpy(msg, "다시 실행할 것이 없습니다."); return; }
    u = &undo[nundo++];
    nredo--;
    text_end(u->y, u->x, u->del, u->ndel, &ey, &ex);
    raw_delete(u->y, u->x, ey, ex);
    raw_insert(u->y, u->x, u->ins, u->nins, &ey, &ex);
    cy = ey; cx = ex;                   /* 편집 뒤 커서 자리 */
    cursor_clamp();
    sel_on = 0; goal = -1; undo_seal = 1;
    modified = (nundo != save_point);
}

/* =========================================================== 편집 */
static void ed_insert(void *ctx, wch c)
{
    (void)ctx;
    if (!insmode && cx < lines[cy].len) edit_replace(cy, cx, cy, cx + 1, &c, 1, UK_TYPE);
    else edit_replace(cy, cx, cy, cx, &c, 1, UK_TYPE);
}

static void ed_commit(void) { han_flush(ed_insert, 0); }

static void ed_newline(void)
{
    static const wch nl = '\n';
    edit_replace(cy, cx, cy, cx, &nl, 1, UK_OTHER);
}

static void ed_backspace(void)
{
    if (han_backspace()) return;
    if (cx > 0) edit_replace(cy, cx - 1, cy, cx, 0, 0, UK_BKSP);
    else if (cy > 0) edit_replace(cy - 1, lines[cy - 1].len, cy, 0, 0, 0, UK_BKSP);
}

static void ed_delete(void)
{
    if (cx < lines[cy].len) edit_replace(cy, cx, cy, cx + 1, 0, 0, UK_DEL);
    else if (cy < nlines - 1) edit_replace(cy, cx, cy + 1, 0, 0, 0, UK_DEL);
}

/* 블록을 지우고 커서를 블록 앞으로. 지웠으면 1 */
static int sel_delete(void)
{
    int y1, x1, y2, x2;
    if (!sel_range(&y1, &x1, &y2, &x2)) { sel_on = 0; return 0; }
    sel_on = 0;
    edit_replace(y1, x1, y2, x2, 0, 0, UK_OTHER);
    return 1;
}

/* ============================================================ 오려두기 */
static wch  *clip;              /* 줄바꿈은 '\n' 으로 저장 */
static long  cliplen, clipcap;

static int clip_copy(void)
{
    int y1, x1, y2, x2;
    long n;
    if (!sel_range(&y1, &x1, &y2, &x2)) return 0;
    n = range_len(y1, x1, y2, x2);
    if (n > clipcap) {
        wch *p = (wch *)realloc(clip, n * sizeof(wch));
        if (!p) { strcpy(msg, "메모리가 모자라 복사하지 못했습니다."); return 0; }
        clip = p; clipcap = n;
    }
    range_get(y1, x1, y2, x2, clip);
    cliplen = n;
    return 1;
}

static void ed_copy(void)
{
    if (sel_count() == 0) { strcpy(msg, "복사할 블록이 없습니다. Shift+이동 글쇠로 블록을 잡으십시오."); return; }
    if (clip_copy()) {
        sprintf(msg, "블록 %ld자를 복사했습니다. Ctrl-V: 붙이기", cliplen);
        if (sel_sticky) sel_on = 0;     /* F3 블록은 복사하면 풂 */
    }
}

static void ed_cut(void)
{
    if (sel_count() == 0) { strcpy(msg, "오려 둘 블록이 없습니다. Shift+이동 글쇠로 블록을 잡으십시오."); return; }
    if (clip_copy()) {
        sel_delete();
        sprintf(msg, "블록 %ld자를 오려 두었습니다. Ctrl-V: 붙이기", cliplen);
    }
}

/* 붙이기: 블록이 있으면 블록 자리에 (되돌리기 한 번에 함께 되돌려짐).
   수정 상태여도 늘 끼워 넣기 */
static void ed_paste(void)
{
    int y1, x1, y2, x2;
    if (!cliplen) { strcpy(msg, "붙일 내용이 없습니다. 먼저 Ctrl-C로 복사하십시오."); return; }
    if (sel_range(&y1, &x1, &y2, &x2)) edit_replace(y1, x1, y2, x2, clip, cliplen, UK_OTHER);
    else edit_replace(cy, cx, cy, cx, clip, cliplen, UK_OTHER);
    sel_on = 0;
}

static void ed_vmove(int dy)
{
    if (goal < 0) goal = dcol(&lines[cy], cx);
    cy += dy;
    if (cy < 0) cy = 0;
    if (cy >= nlines) cy = nlines - 1;
    cx = col_to_idx(&lines[cy], goal);
}

static int is_word(wch c) { return c != ' ' && c != 0x3000; }

static void ed_word(int dir)
{
    Line *l = &lines[cy];
    goal = -1;
    if (dir > 0) {
        if (cx >= l->len) { if (cy < nlines - 1) { cy++; cx = 0; } return; }
        while (cx < l->len && is_word(l->s[cx])) cx++;
        while (cx < l->len && !is_word(l->s[cx])) cx++;
    } else {
        if (cx == 0) { if (cy > 0) { cy--; cx = lines[cy].len; } return; }
        while (cx > 0 && !is_word(l->s[cx - 1])) cx--;
        while (cx > 0 && is_word(l->s[cx - 1])) cx--;
    }
}


/* ========================================================== 화면 그리기 */
static wch  *tmpbuf;
static int   tmpcap;

/* ===================================================== 자동 줄 바꿈 */
/* 화면 줄: 파일의 한 줄을 SCR_W 칸에 맞게 나눈 조각. 띄어쓰기에서 나누고,
   한 줄보다 긴 낱말은 글자 단위로 나눔. 파일 내용은 바뀌지 않음 */

/* s[0..n)에서 st부터 시작하는 화면 줄의 끝 (= 다음 화면 줄의 시작, 마지막이면 n) */
static int wrap_next(const wch *s, int n, int st)
{
    int i = st, col = 0, brk = -1;
    while (i < n) {
        int w = tw(s[i]);
        if (col + w > SCR_W) break;
        col += w;
        if (s[i] == ' ') brk = i + 1;
        i++;
    }
    if (i >= n) return n;
    if (s[i] == ' ') return i + 1;      /* 넘치는 띄어쓰기는 이 줄 끝에 붙임 */
    if (brk > st) return brk;           /* 낱말 단위로 넘김 */
    return i > st ? i : st + 1;         /* 낱말이 한 줄보다 길면 글자 단위 */
}

static int wrap_nrows(const wch *s, int n)
{
    int st = 0, r = 1;
    for (;;) {
        int e = wrap_next(s, n, st);
        if (e >= n) return r;
        st = e; r++;
    }
}

/* x가 든 화면 줄 번호와 그 줄의 [시작, 끝) */
static int wrap_row_of(const wch *s, int n, int x, int *rs, int *re)
{
    int st = 0, r = 0;
    for (;;) {
        int e = wrap_next(s, n, st);
        if (x < e || e >= n) { *rs = st; *re = e; return r; }
        st = e; r++;
    }
}

/* r번째 화면 줄의 [시작, 끝) */
static void wrap_row_bounds(const wch *s, int n, int r, int *rs, int *re)
{
    int st = 0, e;
    for (;;) {
        e = wrap_next(s, n, st);
        if (!r || e >= n) break;
        st = e; r--;
    }
    *rs = st; *re = e;
}

/* 줄 li를 화면에 보이는 대로 (커서 줄이면 조합 중인 글자를 끼워) 돌려줌.
   ci: 커서 자리, hs~he: 조합 중인 글자 범위 (없으면 -1) */
static void disp_line(int li, int pl, const wch **ps, int *pn, int *ci, int *hs, int *he)
{
    const wch *s = lines[li].s;
    int n = lines[li].len;
    *ci = *hs = *he = -1;
    if (li == cy) {
        int need = n + pl + 1, rest;
        if (need > tmpcap) {
            tmpcap = need + 64;
            tmpbuf = (wch *)realloc(tmpbuf, tmpcap * sizeof(wch));
            if (!tmpbuf) fatal("out of memory");
        }
        memcpy(tmpbuf, s, cx * sizeof(wch));
        memcpy(tmpbuf + cx, pre, pl * sizeof(wch));
        rest = cx + ((pl && !insmode && cx < n) ? 1 : 0);
        memcpy(tmpbuf + cx + pl, s + rest, (n - rest) * sizeof(wch));
        n = cx + pl + (n - rest);
        s = tmpbuf;
        if (pl) { *hs = cx; *he = cx + pl; }
        else *ci = cx;
    }
    *ps = s; *pn = n;
}

static int line_nrows(int li)
{
    const wch *s; int n, a, b, c;
    disp_line(li, prelen, &s, &n, &a, &b, &c);
    return wrap_nrows(s, n);
}

static int cursor_row(int *rs, int *re)  /* 커서가 든 화면 줄 (줄 cy 안에서) */
{
    const wch *s; int n, a, b, c;
    disp_line(cy, prelen, &s, &n, &a, &b, &c);
    return wrap_row_of(s, n, cx, rs, re);
}

/* 문서 자리 (y, x)의 화면 좌표. 화면 밖이면 0 */
static int screen_pos(int y, int x, int *sx, int *sy)
{
    if (!wrap_on) {
        *sy = EDIT_Y + (y - top);
        *sx = dcol(&lines[y], x) - left;
    } else {
        int li = top, r = top_row, cnt = 0, rs, re, row;
        row = wrap_row_of(lines[y].s, lines[y].len, x, &rs, &re);
        if (y < top) return 0;
        while (li < y && cnt <= EDIT_H) { cnt += line_nrows(li) - r; r = 0; li++; }
        cnt += row - r;
        *sy = EDIT_Y + cnt;
        *sx = dcol(&lines[y], x) - dcol(&lines[y], rs);
    }
    return *sy >= EDIT_Y && *sy < EDIT_Y + EDIT_H;
}

static void ensure_visible(void)
{
    int rs, re, crow, li, r, cnt, k;
    if (cx > lines[cy].len) cx = lines[cy].len;
    if (!wrap_on) {
        int dc, cw;
        Line *l = &lines[cy];
        top_row = 0;
        if (cy < top) top = cy;
        if (cy >= top + EDIT_H) top = cy - EDIT_H + 1;
        dc = dcol(l, cx);
        cw = prelen ? tw(pre[0]) : (cx < l->len ? tw(l->s[cx]) : 1);
        while (dc < left) left -= 20;
        if (left < 0) left = 0;
        while (dc + cw > left + SCR_W) left += 20;
        return;
    }
    left = 0;
    if (top >= nlines) top = nlines - 1;
    if (top_row >= line_nrows(top)) top_row = line_nrows(top) - 1;
    crow = cursor_row(&rs, &re);
    if (cy < top || (cy == top && crow < top_row)) { top = cy; top_row = crow; return; }
    /* 위 화면 줄부터 커서까지 몇 줄인지 (화면 높이를 넘으면 그만 셈) */
    li = top; r = top_row; cnt = 0;
    while (li < cy && cnt < EDIT_H) { cnt += line_nrows(li) - r; r = 0; li++; }
    if (li == cy) cnt += crow - r;
    if (cnt < EDIT_H) return;
    /* 커서가 아래로 벗어남: 커서 줄이 맨 아래가 되게 거슬러 올라감 */
    li = cy; r = crow; k = EDIT_H - 1;
    while (k > 0) {
        if (r >= k) { r -= k; k = 0; }
        else {
            k -= r + 1;
            if (li == 0) { r = 0; break; }
            li--;
            r = line_nrows(li) - 1;
        }
    }
    top = li; top_row = r;
}

/* 자동 줄 바꿈일 때 ↑↓: 화면 줄 단위로 옮김 */
static void wrap_vmove(int d)
{
    Line *l = &lines[cy];
    int rs, re, r, i, col, n = d < 0 ? -d : d;
    r = wrap_row_of(l->s, l->len, cx, &rs, &re);
    if (goal < 0) goal = dcol(l, cx) - dcol(l, rs);
    while (n--) {
        if (d < 0) {
            if (r > 0) r--;
            else if (cy > 0) { cy--; l = &lines[cy]; r = wrap_nrows(l->s, l->len) - 1; }
            else break;
        } else {
            if (re < l->len) r++;
            else if (cy < nlines - 1) { cy++; l = &lines[cy]; r = 0; }
            else break;
        }
        wrap_row_bounds(l->s, l->len, r, &rs, &re);
    }
    wrap_row_bounds(l->s, l->len, r, &rs, &re);
    for (i = rs, col = 0; i < re && i < l->len && col + tw(l->s[i]) <= goal; i++) col += tw(l->s[i]);
    if (re < l->len && i >= re) i = re - 1;     /* 다음 화면 줄로 넘어가지 않게 */
    cx = i;
}

static void draw_menubar(int active);
static const char *menu_titles[] = { "파일", "편집", "찾기", "입력", "글꼴", "도움말" };
static const char  menu_hot[]    = { 'F', 'E', 'Q', 'D', 'J', 'H' };
/* Alt+글자 (BIOS 스캔 코드): F E Q D J H — 아래아 한글의 Alt 명령(S 저장, G 찾아가기,
   I 상용구 …)과 겹치지 않게 고름 */
static const int   menu_alt[]    = { 0x21, 0x12, 0x10, 0x20, 0x24, 0x23 };

static int alt_menu(int k)             /* Alt+글자면 메뉴 번호, 아니면 -1 */
{
    int i;
    for (i = 0; i < 6; i++) if (k == K_EXT(menu_alt[i])) return i;
    return -1;
}
#define NMENU 6
static int menu_x[NMENU];

static void draw_menubar(int active)
{
    int i, x = 1;
    scr_fill(0, MENU_Y, SCR_W, 1, ' ', C_BLACK, C_LGRAY);
    for (i = 0; i < NMENU; i++) {
        int fg = (i == active) ? C_WHITE : C_BLACK;
        int bg = (i == active) ? C_BLUE : C_LGRAY;
        char hk[4];
        menu_x[i] = x;
        scr_put(x, MENU_Y, ' ', fg, bg);
        x += 1;
        x += scr_puts(x, MENU_Y, menu_titles[i], fg, bg);
        hk[0] = '('; hk[1] = menu_hot[i]; hk[2] = ')'; hk[3] = 0;
        scr_put(x, MENU_Y, '(', fg, bg);
        scr_put(x + 1, MENU_Y, (wch)menu_hot[i], (i == active) ? C_YELLOW : C_RED, bg);
        scr_put(x + 2, MENU_Y, ')', fg, bg);
        x += 3;
        scr_put(x, MENU_Y, ' ', fg, bg);
        x += 2;
    }
    scr_puts(SCR_W - 1 - utf8_width("Esc/F10: 메뉴"), MENU_Y, "Esc/F10: 메뉴", C_DGRAY, C_LGRAY);
}

static void draw_title(void)
{
    char buf[120];
    const char *name = filename[0] ? filename : "(새 문서)";
    int w;
    scr_fill(0, TITLE_Y, SCR_W, 1, ' ', C_WHITE, C_BLUE);
    scr_puts(1, TITLE_Y, "한글편집기 HANEDIT", C_LCYAN, C_BLUE);   /* 버전은 '이 프로그램은'에만 */
    sprintf(buf, "%s%s", name, modified ? " *" : "");
    w = utf8_width(buf);
    scr_puts((SCR_W - w) / 2, TITLE_Y, buf, C_WHITE, C_BLUE);
}

static void draw_ruler(void)
{
    int i, curc = dcol(&lines[cy], cx) - left;
    if (wrap_on) {                      /* 자동 줄 바꿈: 커서가 든 화면 줄 안의 칸 */
        int rs, re;
        wrap_row_of(lines[cy].s, lines[cy].len, cx, &rs, &re);
        curc = dcol(&lines[cy], cx) - dcol(&lines[cy], rs);
    }
    for (i = 0; i < SCR_W; i++) {
        int c = left + i + 1;
        wch ch;
        if (c % 10 == 0) ch = (wch)('0' + (c / 10) % 10);
        else if (c % 5 == 0) ch = '+';
        else ch = 0x00B7;               /* 가운뎃점 */
        if (i == curc) scr_put(i, RULER_Y, 0x25BC, C_RED, C_CYAN);   /* ▼ */
        else scr_put(i, RULER_Y, ch, C_BLACK, C_CYAN);
    }
}

/* 한 화면 줄 그리기: s[st..en) (en == n 이면 줄 끝 칸도). xoff: 첫 글자의 화면 칸 */
static void draw_seg(int y, const wch *s, int n, int st, int en, int xoff,
                     int ss, int se, int ci, int hs, int he)
{
    int i, col = xoff, last = (en >= n) ? n : en - 1;
    for (i = st; i <= last; i++) {
        int w = (i < n) ? uwidth(s[i]) : 1, x = col, k;
        if (x >= SCR_W) break;
        if (x >= 0) {
            int insel = (i >= ss && i < se);
            int fg = insel ? C_WHITE : TEXT_FG, bg = insel ? C_BLUE : TEXT_BG;
            if (i < n) {
                if (x + w > SCR_W) scr_put(x, y, ' ', fg, bg);
                else scr_put(x, y, s[i], fg, bg);
            } else if (insel) {
                scr_put(x, y, ' ', fg, bg);  /* 줄 끝(줄바꿈) 표시 */
            }
            for (k = 0; k < w && x + k < SCR_W; k++) {
                if (i >= hs && i < he) scr_attr(x + k, y, A_CURBLK);
                else if (i == ci && blink_on && !overlay)
                    scr_attr(x + k, y, insmode ? A_CURBAR : A_CURBLK);
            }
        }
        col += w;
    }
}

static void sel_span(int li, int n, int *ss, int *se)   /* 줄 li 에서 블록 범위 */
{
    int y1, x1, y2, x2;
    *ss = *se = -1;
    if (sel_on && sel_range(&y1, &x1, &y2, &x2) && li >= y1 && li <= y2) {
        *ss = (li == y1) ? x1 : 0;
        *se = (li == y2) ? x2 : n + 1;  /* n+1: 줄바꿈도 블록에 듦 */
    }
}

static void draw_edit(void)
{
    int r, pl = overlay ? 0 : prelen;   /* 대화 상자 입력 중이면 본문에 조합 글자 안 그림 */
    font_hangul_wide = text_wide;       /* 본문은 글꼴 메뉴에서 고른 한글로 */
    for (r = 0; r < EDIT_H; r++) scr_fill(0, EDIT_Y + r, SCR_W, 1, ' ', TEXT_FG, TEXT_BG);
    if (!wrap_on) {
        for (r = 0; r < EDIT_H && top + r < nlines; r++) {
            const wch *s; int n, ci, hs, he, ss, se;
            disp_line(top + r, pl, &s, &n, &ci, &hs, &he);
            if (pl) sel_span(-1, n, &ss, &se); else sel_span(top + r, n, &ss, &se);
            draw_seg(EDIT_Y + r, s, n, 0, n, -left, ss, se, ci, hs, he);
        }
    } else {
        int li = top, r0 = top_row, row = 0;
        while (row < EDIT_H && li < nlines) {
            const wch *s; int n, ci, hs, he, ss, se, st = 0, rr = 0;
            disp_line(li, pl, &s, &n, &ci, &hs, &he);
            if (pl) sel_span(-1, n, &ss, &se); else sel_span(li, n, &ss, &se);
            for (;;) {
                int e = wrap_next(s, n, st);
                if (rr >= r0) {
                    draw_seg(EDIT_Y + row, s, n, st, e, 0, ss, se, ci, hs, he);
                    if (++row >= EDIT_H) break;
                }
                if (e >= n) break;
                st = e; rr++;
            }
            r0 = 0; li++;
        }
    }
    font_hangul_wide = 1;               /* 메뉴·대화 상자는 늘 넓은 한글 */
}

static int seg(int x, int y, const char *s, int fg, int bg)
{
    return scr_puts(x, y, s, fg, bg);
}

#define HANIND_W 6             /* 오른쪽 끝 한글/영문 표시 폭 */

static void draw_hanind(void)   /* 상태 줄 맨 오른쪽: 빨간 '한글' / 파란 '영문' */
{
    if (hanmode) seg(SCR_W - HANIND_W, STATUS_Y, " 한글 ", C_WHITE, C_GREEN);
    else         seg(SCR_W - HANIND_W, STATUS_Y, " 영문 ", C_WHITE, C_BLUE);
}

static void draw_status(void)
{
    char buf[80];
    int x = 0;
    scr_fill(0, STATUS_Y, SCR_W, 1, ' ', C_BLACK, C_LGRAY);
    sprintf(buf, " 줄 %d/%d  칸 %d ", cy + 1, nlines, dcol(&lines[cy], cx) + 1);
    x += seg(x, STATUS_Y, buf, C_BLACK, C_LGRAY);
    x = 24;
    scr_put(x++, STATUS_Y, 0x2502, C_DGRAY, C_LGRAY);
    x++;
    x += seg(x, STATUS_Y, insmode ? "삽입" : "수정", C_BLACK, C_LGRAY);
    x++;
    scr_put(x++, STATUS_Y, 0x2502, C_DGRAY, C_LGRAY);
    x++;
    x += seg(x, STATUS_Y, kbdtype ? "세벌식 최종" : "두벌식", C_BLACK, C_LGRAY);
    if (!crlf) {                        /* 기본(CR/LF)이 아닐 때만 줄바꿈 방식 표시 */
        x++;
        scr_put(x++, STATUS_Y, 0x2502, C_DGRAY, C_LGRAY);
        x++;
        x += seg(x, STATUS_Y, "LF", C_BLACK, C_LGRAY);
    }
    if (sel_on && sel_count() > 0) {
        sprintf(buf, " 블록 %ld자 ", sel_count());
        x += 2;
        seg(x, STATUS_Y, buf, C_WHITE, C_BLUE);
    }
    draw_hanind();
}

/* 알림(msg)은 다음 글쇠를 누를 때까지 상태 줄 왼쪽에 겹쳐 보임 */
static void draw_msg(void)
{
    const char *p = msg;
    int x = 1, lim = SCR_W - HANIND_W - 1;
    if (!msg[0]) return;
    scr_fill(0, STATUS_Y, lim, 1, ' ', C_YELLOW, C_RED);
    while (*p) {
        wch c = utf8_next(&p);
        if (x + uwidth(c) > lim) break;
        scr_put(x, STATUS_Y, c, C_YELLOW, C_RED);
        x += uwidth(c);
    }
    draw_hanind();
}

static void render(void)
{
    ensure_visible();
    draw_menubar(-1);
    draw_title();
    draw_ruler();
    draw_edit();
    draw_status();
    draw_msg();
    if (overlay) overlay();
    scr_flush();
}

/* 키 기다리기 (커서 깜빡임 포함) */
static int getkey(void)
{
    unsigned long t = kbd_ticks();
    for (;;) {
        if (kbd_hit()) {
            blink_on = 1;
            return kbd_get();
        }
        if (kbd_ticks() - t >= 5) {
            t = kbd_ticks();
            blink_on = !blink_on;
            render();
        }
        kbd_idle();
    }
}

/* ============================================================ 대화 상자 */
typedef struct {
    wch  buf[64];
    int  len, cur, max;
    int  hangul;                /* 한글 입력 허용 */
} Field;

static Field *fld_cur;

static void fld_insert(void *ctx, wch c)
{
    Field *f = (Field *)ctx;
    if (f->len >= f->max) return;
    memmove(f->buf + f->cur + 1, f->buf + f->cur, (f->len - f->cur) * sizeof(wch));
    f->buf[f->cur++] = c;
    f->len++;
}

static void fld_set(Field *f, const char *s)
{
    f->len = 0;
    while (*s && f->len < f->max) f->buf[f->len++] = (wch)(unsigned char)*s++;
    f->cur = f->len;
}

static void fld_get(Field *f, char *out, int n)
{
    int i, j = 0;
    for (i = 0; i < f->len && j < n - 1; i++)
        if (f->buf[i] < 0x80) out[j++] = (char)f->buf[i];
    out[j] = 0;
}

static void fld_draw(Field *f, int x, int y, int w)
{
    int i, col = 0, cur_x = -1;
    scr_fill(x, y, w, 1, ' ', C_WHITE, C_BLUE);
    for (i = 0; i <= f->len; i++) {
        if (i == f->cur) {
            if (prelen && f->hangul) {
                int k;
                for (k = 0; k < prelen && col < w - 1; k++) {
                    int j, cw = uwidth(pre[k]);
                    scr_put(x + col, y, pre[k], C_WHITE, C_BLUE);
                    for (j = 0; j < cw; j++) scr_attr(x + col + j, y, A_CURBLK);
                    col += cw;
                }
            } else cur_x = x + col;
        }
        if (i == f->len) break;
        if (col + uwidth(f->buf[i]) > w - 1) break;
        scr_put(x + col, y, f->buf[i], C_WHITE, C_BLUE);
        col += uwidth(f->buf[i]);
    }
    if (cur_x >= 0 && blink_on) scr_attr(cur_x, y, A_CURBAR);
}

/* 입력란 글쇠 처리: 1 = Enter, -1 = Esc, 0 = 계속 */
static int fld_key(Field *f, int k)
{
    if (k == K_HANTOGGLE) {
        han_flush(fld_insert, f);
        hanmode = !hanmode;
        return 0;
    }
    if (k == K_ENTER) { han_flush(fld_insert, f); return 1; }
    if (k == K_ESC)   { hangul_ic_reset(ic); prelen = 0; return -1; }
    if (k == K_BKSP) {
        if (han_backspace()) return 0;
        if (f->cur > 0) {
            memmove(f->buf + f->cur - 1, f->buf + f->cur, (f->len - f->cur) * sizeof(wch));
            f->cur--; f->len--;
        }
        return 0;
    }
    if (k >= ' ' && k < 127) {
        if (f->hangul) han_type(k, fld_insert, f);
        else fld_insert(f, (wch)k);
        return 0;
    }
    han_flush(fld_insert, f);
    switch (k) {
    case K_LEFT:  if (f->cur > 0) f->cur--; break;
    case K_RIGHT: if (f->cur < f->len) f->cur++; break;
    case K_HOME:  f->cur = 0; break;
    case K_END:   f->cur = f->len; break;
    case K_DEL:
        if (f->cur < f->len) {
            memmove(f->buf + f->cur, f->buf + f->cur + 1, (f->len - f->cur - 1) * sizeof(wch));
            f->len--;
        }
        break;
    }
    return 0;
}

/* --- 한 줄 입력 대화 상자 --- */
static const char *dlg_title, *dlg_prompt;
static int dlg_w;

static void dlg_frame(int x, int y, int w, int h, const char *title)
{
    int tw = utf8_width(title);
    scr_shadow(x + 2, y + 1, w, h);
    scr_box(x, y, w, h, C_BLACK, C_LGRAY, 1);
    scr_put(x + (w - tw) / 2 - 1, y, ' ', C_BLUE, C_LGRAY);
    scr_puts(x + (w - tw) / 2, y, title, C_BLUE, C_LGRAY);
    scr_put(x + (w - tw) / 2 + tw, y, ' ', C_BLUE, C_LGRAY);
}

static void input_overlay(void)
{
    int x = (SCR_W - dlg_w) / 2, y = 11;
    dlg_frame(x, y, dlg_w, 7, dlg_title);
    scr_puts(x + 3, y + 2, dlg_prompt, C_BLACK, C_LGRAY);
    fld_draw(fld_cur, x + 3, y + 3, dlg_w - 6);
    scr_puts(x + 3, y + 5, "Enter: 확인   Esc: 취소", C_DGRAY, C_LGRAY);
}

static int input_dialog(const char *title, const char *prompt, Field *f)
{
    int r;
    dlg_title = title; dlg_prompt = prompt; dlg_w = 56;
    fld_cur = f;
    overlay = input_overlay;
    for (;;) {
        render();
        r = fld_key(f, getkey());
        if (r) break;
    }
    overlay = 0;
    return r > 0;
}

/* --- 알림/질문 상자 --- */
static const char *mb_lines[6];
static int mb_n;

static void msgbox_overlay(void)
{
    int i, w = 0, x, y, h = mb_n + 4;
    for (i = 0; i < mb_n; i++) if (utf8_width(mb_lines[i]) > w) w = utf8_width(mb_lines[i]);
    w += 8;
    if (w < 30) w = 30;
    x = (SCR_W - w) / 2; y = (SCR_H - h) / 2;
    dlg_frame(x, y, w, h, dlg_title);
    for (i = 0; i < mb_n; i++) scr_puts(x + 4, y + 2 + i, mb_lines[i], C_BLACK, C_LGRAY);
}

/* keys: 받아들일 글쇠 목록 (대문자). 돌려주는 값: 누른 글쇠(대문자) 또는 K_ESC */
static int msgbox(const char *title, const char *l1, const char *l2, const char *keys)
{
    int k;
    dlg_title = title;
    mb_lines[0] = l1; mb_n = 1;
    if (l2) { mb_lines[1] = ""; mb_lines[2] = l2; mb_n = 3; }
    overlay = msgbox_overlay;
    for (;;) {
        render();
        k = getkey();
        if (k == K_HANTOGGLE) continue;
        if (k == K_ESC) break;
        if (k < 128 && strchr(keys, toupper(k))) { k = toupper(k); break; }
        if (!keys[0]) break;
        if (k == K_ENTER && strchr(keys, '\r')) break;
    }
    overlay = 0;
    return k;
}

/* --- 도움말 --- */
static const char *help_text[] = {   /* "글쇠\t하는 일", "-" = 가로줄 */
    "글쇠\t하는 일",
    "-",
    "Shift-Space\t한글/영문 전환",
    "오른쪽 Alt\t한글/영문 전환 (한/영 키도 됨)",
    "F9 / 한자 키\t한자로 바꾸기",
    "화살표 Home End\t커서 이동",
    "PgUp PgDn\t한 화면 위/아래",
    "Ctrl-Home Ctrl-End\t문서 처음/끝",
    "Ctrl-←  Ctrl-→\t낱말 단위 이동",
    "Ins\t삽입/수정 상태 전환",
    "Del  BackSpace\t글자 지우기 / 블록 지우기",
    "Shift + 이동 글쇠\t블록 잡기 (Esc: 블록 풀기)",
    "F3 / Ctrl-A\t블록 설정 / 모두 선택",
    "Ctrl-X / C / V\t오려두기 / 복사하기 / 붙이기",
    "Ctrl-Z / Ctrl-Y\t되돌리기 / 다시 실행",
    "Alt-B\t배경색 변경 (글자색과 맞바꿈)",
    "Alt-S / Alt-V\t저장하기 / 다른 이름으로",
    "Alt-O / Alt-N\t불러오기 / 새 문서",
    "F2 / Ctrl-L\t찾기 / 다시 찾기",
    "Alt-G\t줄 찾아가기",
    "F10 / 왼쪽 Alt\t메뉴 (Alt+괄호 속 글자: 그 메뉴)",
    "Alt-X\t끝내기",
    0
};

static void help_overlay(void)
{
    int i, x = 11, y = 1, w = 58, h = 27;
    dlg_frame(x, y, w, h, "도움말");
    for (i = 0; help_text[i]; i++) {
        const char *t = strchr(help_text[i], '\t');
        int fg = i < 2 ? C_BLUE : C_BLACK;
        if (help_text[i][0] == '-') {               /* 가로줄 */
            int j;
            for (j = 2; j < w - 4; j++) scr_put(x + j, y + 2 + i, 0x2500, C_BLACK, C_LGRAY);
            continue;
        }
        if (t) {                                    /* 두 칸: 글쇠 / 하는 일 */
            char key[48];
            int n = (int)(t - help_text[i]);
            memcpy(key, help_text[i], n); key[n] = 0;
            scr_puts(x + 4, y + 2 + i, key, fg, C_LGRAY);
            scr_puts(x + 24, y + 2 + i, t + 1, fg, C_LGRAY);
        }
    }
    scr_puts(x + 2, y + h - 2, "  아무 글쇠나 누르면 돌아갑니다.", C_DGRAY, C_LGRAY);
}

static void show_help(void)
{
    int k;
    overlay = help_overlay;
    do { render(); k = getkey(); } while (k == K_HANTOGGLE);
    overlay = 0;
}

static void show_about(void)
{
    msgbox("이 프로그램은", "HANEDIT " VERSION " - VGA 한글 텍스트 편집기",
           "입력: libhangul (LGPL)  글꼴: 갈무리14, GNU Unifont", "");
}

/* --- 파일 선택 (불러오기) --- */
#define MAXFILES 512
typedef struct { char name[13]; int dir; } FEnt;
static FEnt *files;
static int nfiles, fsel, ftop;
static Field ffield;
static char cwd[80];

static int fent_cmp(const void *a, const void *b)
{
    const FEnt *x = (const FEnt *)a, *y = (const FEnt *)b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcmp(x->name, y->name);
}

static void scan_dir(void)
{
    struct find_t ft;
    unsigned rc;
    nfiles = 0; fsel = 0; ftop = 0;
    if (!files) files = (FEnt *)malloc(MAXFILES * sizeof(FEnt));
    if (!files) return;
    rc = _dos_findfirst("*.*", _A_NORMAL | _A_RDONLY | _A_ARCH | _A_SUBDIR, &ft);
    while (rc == 0 && nfiles < MAXFILES) {
        if (strcmp(ft.name, ".") != 0) {
            strcpy(files[nfiles].name, ft.name);
            files[nfiles].dir = (ft.attrib & _A_SUBDIR) != 0;
            nfiles++;
        }
        rc = _dos_findnext(&ft);
    }
    qsort(files, nfiles, sizeof(FEnt), fent_cmp);
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
}

#define FL_ROWS 12
static void open_overlay(void)
{
    int x = 14, y = 4, w = 52, h = 22, i;
    char b[40];
    dlg_frame(x, y, w, h, "불러오기");
    scr_puts(x + 3, y + 2, "파일 이름:", C_BLACK, C_LGRAY);
    fld_draw(&ffield, x + 14, y + 2, w - 17);
    scr_puts(x + 3, y + 4, "디렉터리:", C_BLACK, C_LGRAY);
    scr_puts(x + 14, y + 4, cwd, C_BLUE, C_LGRAY);
    scr_box(x + 3, y + 5, w - 6, FL_ROWS + 2, C_BLACK, C_LGRAY, 0);
    for (i = 0; i < FL_ROWS && ftop + i < nfiles; i++) {
        FEnt *e = &files[ftop + i];
        int sel = (ftop + i == fsel);
        if (e->dir) sprintf(b, " [%s]", e->name); else sprintf(b, " %s", e->name);
        scr_fill(x + 4, y + 6 + i, w - 8, 1, ' ', sel ? C_WHITE : C_BLACK, sel ? C_BLUE : C_LGRAY);
        scr_puts(x + 4, y + 6 + i, b, sel ? C_WHITE : (e->dir ? C_BLUE : C_BLACK), sel ? C_BLUE : C_LGRAY);
    }
    scr_puts(x + 3, y + h - 2, "↑↓: 고르기  Enter: 불러오기  Esc: 취소", C_DGRAY, C_LGRAY);
}

static void sel_to_field(void)
{
    if (fsel < nfiles && !files[fsel].dir) fld_set(&ffield, files[fsel].name);
    else ffield.len = ffield.cur = 0;
}

static int is_dir(const char *name)
{
    struct find_t ft;
    if (_dos_findfirst(name, _A_SUBDIR | _A_RDONLY | _A_ARCH, &ft) != 0) return 0;
    return (ft.attrib & _A_SUBDIR) != 0;
}

static int open_dialog(char *out)
{
    int k, r = 0;
    ffield.max = 63; ffield.hangul = 0;
    ffield.len = ffield.cur = 0;
    scan_dir();
    overlay = open_overlay;
    for (;;) {
        render();
        k = getkey();
        if (k == K_UP || k == K_DOWN || k == K_PGUP || k == K_PGDN) {
            if (k == K_UP) fsel--;
            if (k == K_DOWN) fsel++;
            if (k == K_PGUP) fsel -= FL_ROWS;
            if (k == K_PGDN) fsel += FL_ROWS;
            if (fsel >= nfiles) fsel = nfiles - 1;
            if (fsel < 0) fsel = 0;
            if (fsel < ftop) ftop = fsel;
            if (fsel >= ftop + FL_ROWS) ftop = fsel - FL_ROWS + 1;
            sel_to_field();
            continue;
        }
        if (k == K_ENTER) {
            char name[64];
            fld_get(&ffield, name, sizeof(name));
            if (!name[0] && fsel < nfiles && files[fsel].dir) strcpy(name, files[fsel].name);
            if (!name[0]) continue;
            if (is_dir(name) || strcmp(name, "..") == 0) {
                if (chdir(name) == 0) { scan_dir(); ffield.len = ffield.cur = 0; }
                continue;
            }
            strcpy(out, name);
            r = 1;
            break;
        }
        if (fld_key(&ffield, k) < 0) break;
    }
    overlay = 0;
    return r;
}

/* =========================================================== 명령 */
enum { CMD_NONE, CMD_NEW, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_QUIT,
       CMD_INSMODE, CMD_TOP, CMD_BOTTOM,
       CMD_FIND, CMD_FINDNEXT, CMD_GOTO,
       CMD_KBD2, CMD_KBD3, CMD_HELP, CMD_ABOUT,
       CMD_CUT, CMD_COPY, CMD_PASTE, CMD_UNDO, CMD_REDO, CMD_COLORS,
       CMD_FONT_NARROW, CMD_FONT_WIDE, CMD_HANJA,
       CMD_BLOCK, CMD_SELALL, CMD_WRAP };


static int do_saveas(void);

static int do_save(void)
{
    char b[120];
    if (!filename[0]) return do_saveas();
    if (doc_save(filename) != 0) {
        sprintf(b, "%s 파일에 저장하지 못했습니다.", filename);
        msgbox("오류", b, "아무 글쇠나 누르십시오.", "");
        return 0;
    }
    sprintf(msg, "%s 에 저장했습니다 (UTF-8, %d줄)", filename, nlines);
    return 1;
}

static int do_saveas(void)
{
    Field f;
    char name[64];
    f.max = 63; f.hangul = 0;
    fld_set(&f, filename);
    if (!input_dialog("다른 이름으로 저장", "저장할 파일 이름:", &f)) return 0;
    fld_get(&f, name, sizeof(name));
    if (!name[0]) return 0;
    if (strcmp(name, filename) != 0) {
        FILE *t = fopen(name, "rb");
        if (t) {
            fclose(t);
            if (msgbox("다른 이름으로 저장", "같은 이름의 파일이 있습니다. 덮어쓸까요?",
                       "Y: 덮어쓰기   N: 취소", "YN") != 'Y') return 0;
        }
    }
    strcpy(filename, name);
    return do_save();
}

/* 수정된 문서를 버려도 되는지 확인. 1 = 계속 진행 */
static int confirm_discard(void)
{
    int k;
    if (!modified) return 1;
    k = msgbox("확인", "문서가 고쳐졌습니다. 저장할까요?",
               "Y: 저장   N: 저장 안 함   Esc: 취소", "YN");
    if (k == 'Y') return do_save();
    return k == 'N';
}

static void do_open(void)
{
    char name[64], b[120];
    int r;
    if (!confirm_discard()) return;
    if (!open_dialog(name)) return;
    r = doc_load(name);
    if (r == -1) {
        sprintf(b, "%s 파일을 열 수 없습니다.", name);
        msgbox("오류", b, "아무 글쇠나 누르십시오.", "");
        return;
    }
    if (r < 0) { msgbox("오류", "파일을 읽지 못했습니다.", 0, ""); return; }
    strcpy(filename, name);
    if (!msg[0]) sprintf(msg, "%s 을(를) 불러왔습니다 (%d줄)", filename, nlines);
}

static int wmatch(const Line *l, int pos)
{
    return pos + findlen <= l->len && memcmp(l->s + pos, findbuf, findlen * sizeof(wch)) == 0;
}

static void do_findnext(void)
{
    int i, y = cy, x = cx + 1, n;
    if (!findlen) return;
    for (n = 0; n <= nlines; n++) {
        Line *l = &lines[y];
        for (i = x; i + findlen <= l->len; i++)
            if (wmatch(l, i)) { cy = y; cx = i; goal = -1; return; }
        y = (y + 1) % nlines;
        x = 0;
    }
    strcpy(msg, "찾는 낱말이 없습니다.");
}

static void do_find(void)
{
    Field f;
    f.max = 60; f.hangul = 1;
    memcpy(f.buf, findbuf, findlen * sizeof(wch));
    f.len = f.cur = findlen;
    {
        static char prompt[64];
        strcpy(prompt, "찾을 낱말 (Shift-Space, 오른쪽 Alt: 한/영):");
        if (!input_dialog("찾기", prompt, &f)) return;
    }
    if (!f.len) return;
    memcpy(findbuf, f.buf, f.len * sizeof(wch));
    findlen = f.len;
    {
        /* 현재 위치부터 찾기 */
        int ox = cx;
        cx--;
        do_findnext();
        if (msg[0]) cx = ox;
    }
}

static void do_goto(void)
{
    Field f;
    char b[16];
    int n;
    f.max = 7; f.hangul = 0;
    f.len = f.cur = 0;
    if (!input_dialog("줄 찾아가기", "찾아갈 줄 번호:", &f)) return;
    fld_get(&f, b, sizeof(b));
    n = atoi(b);
    if (n < 1) return;
    if (n > nlines) n = nlines;
    cy = n - 1; cx = 0; goal = -1;
    top = cy - EDIT_H / 2;
    if (top < 0) top = 0;
    top_row = 0;
}

static int running = 1;

/* ============================================================ 블록 설정 (F3) */
/* 아래아 한글처럼: F3 한 번 = 블록 시작(이동 글쇠로 범위를 넓힘), 두 번 = 낱말,
   세 번 = 줄(문단), 네 번 = 문서 전체. Esc로 풂 */
static int f3_level, last_key;

static void do_block(void)
{
    Line *l = &lines[cy];
    f3_level = (last_key == K_F(3) && sel_on) ? f3_level + 1 : 1;
    if (f3_level > 4) f3_level = 4;
    sel_on = 1; sel_sticky = 1; goal = -1;
    switch (f3_level) {
    case 1:
        sel_y = cy; sel_x = cx;
        strcpy(msg, "블록 설정: 이동 글쇠로 범위를 정하십시오. F3 두 번 낱말, 세 번 줄, 네 번 문서 전체");
        break;
    case 2: {                                   /* 낱말 */
            int a = cx, b = cx;
            while (a > 0 && is_word(l->s[a - 1])) a--;
            while (b < l->len && is_word(l->s[b])) b++;
            sel_y = cy; sel_x = a; cx = b;
        }
        break;
    case 3:                                     /* 줄 (줄바꿈까지) */
        sel_y = cy; sel_x = 0;
        if (cy < nlines - 1) { cy++; cx = 0; } else cx = l->len;
        break;
    case 4:                                     /* 문서 전체 */
        sel_y = 0; sel_x = 0;
        cy = nlines - 1; cx = lines[cy].len;
        break;
    }
}

/* ============================================================ 설정 파일 */
/* HANEDIT.CFG (EXE와 같은 디렉터리). 줄마다 "이름=값", ';' 뒤는 주석.
     font   = wide | narrow     본문 한글 글꼴
     colors = dark | light      본문 색 (light: 글자색과 바탕색을 맞바꾼 상태)
     wrap   = off | on          자동 줄 바꿈
   (예전 판의 hankey 항목은 읽을 때 무시함. 한/영 전환은 Shift-Space와 오른쪽 Alt 둘 다 됨) */
static char exedir[160];         /* EXE가 있는 디렉터리 ('\\'로 끝남) */
static char cfgpath[176];

static void cfg_path(const char *argv0)
{
    const char *p = strrchr(argv0, '\\'), *q = strrchr(argv0, ':');
    int n;
    if (q && (!p || q > p)) p = q;
    if (p) {                            /* EXE가 있는 디렉터리 */
        n = (int)(p - argv0 + 1);
        if (n > (int)sizeof(exedir) - 1) n = 0;
        memcpy(exedir, argv0, n);
        exedir[n] = 0;
    } else if (getcwd(exedir, sizeof(exedir) - 1)) {
        n = (int)strlen(exedir);
        if (n && exedir[n - 1] != '\\') strcat(exedir, "\\");
    } else exedir[0] = 0;
    sprintf(cfgpath, "%sHANEDIT.CFG", exedir);
}

static void cfg_load(void)
{
    char line[128];
    FILE *f = fopen(cfgpath, "r");
    if (!f) return;                     /* 없으면 기본값 (넓은 한글, 어두운 바탕) */
    while (fgets(line, sizeof(line), f)) {
        char *k = line, *v, *e;
        if ((e = strchr(line, ';')) != 0) *e = 0;
        if (!(v = strchr(line, '='))) continue;
        *v++ = 0;
        while (*k == ' ' || *k == '\t') k++;
        for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' ||
                                          e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
        if (!stricmp(k, "font")) {
            if (!stricmp(v, "wide")) text_wide = 1;
            else if (!stricmp(v, "narrow")) text_wide = 0;
        } else if (!stricmp(k, "colors")) {
            if (!stricmp(v, "dark")) textinv = 0;
            else if (!stricmp(v, "light")) textinv = 1;
        } else if (!stricmp(k, "wrap")) {
            if (!stricmp(v, "on")) wrap_on = 1;
            else if (!stricmp(v, "off")) wrap_on = 0;
        } else if (!stricmp(k, "hankey")) {
            /* 예전 판의 설정: 이제 두 글쇠 모두 쓰므로 무시 */
        }
    }
    fclose(f);
}

static void cfg_save(void)
{
    FILE *f = fopen(cfgpath, "w");
    int err;
    if (!f) { sprintf(msg, "설정 파일 %s 을(를) 저장하지 못했습니다.", cfgpath); return; }
    fputs("; HANEDIT settings (saved automatically)\n", f);
    fputs("; font   = wide | narrow\n", f);
    fputs("; colors = dark | light\n", f);
    fputs("; wrap   = off | on\n", f);
    fprintf(f, "font=%s\n", text_wide ? "wide" : "narrow");
    fprintf(f, "colors=%s\n", textinv ? "light" : "dark");
    fprintf(f, "wrap=%s\n", wrap_on ? "on" : "off");
    err = ferror(f);
    if (fclose(f) != 0 || err) sprintf(msg, "설정 파일 %s 을(를) 저장하지 못했습니다.", cfgpath);
}

/* ============================================================ 한자 */
/* HANJA.DIC (EXE와 같은 디렉터리, tools/genhanja.py 로 만듦)
   머리: "HJD2", u32 음절부 위치, u32 음절부 길이, u32 낱말 색인 위치, u32 낱말 수, u32 0
   음절부 (메모리에 읽음): "HJD1", u16 음절 수, u16 0,
       음절 수 x { u16 한글 음절, u16 후보 수, u32 위치 }, 후보마다 { u16 한자, u8 길이, 뜻 }
   낱말부 (디스크에서 바로 찾음): 낱말 수 x u32 기록 위치 (낱말 순으로 정렬),
       기록: u8 글자 수 n, n x u16 한글, u8 후보 수, 후보마다 n x u16 한자            */
static unsigned char *hdic;             /* 음절부 */
static long hdic_len;
static unsigned long hw_idx, hw_n;      /* 낱말 색인 위치, 낱말 수 */

#define HMAXW 12                        /* 바꿀 수 있는 낱말의 최대 글자 수 */
typedef struct {
    wch  hz[HMAXW];
    char m[100];                        /* 뜻 (UTF-8) */
} HCand;
static HCand *hc;
static int nhc, hccap, hsel, hlen;      /* hlen: 바꿀 글자 수 */
static wch hkey[HMAXW];
static int hbox_x, hbox_y, hbox_w;
#define HPAGE 9

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long rd32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int is_syllable(wch c) { return c >= 0xAC00 && c <= 0xD7A3; }

static void wch_utf8(wch c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; out[1] = 0; }
    else if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); out[2] = 0; }
    else {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F)); out[3] = 0;
    }
}

static void hanja_path(char *path) { sprintf(path, "%sHANJA.DIC", exedir); }

static int hanja_load(void)
{
    char path[180];
    unsigned char hd[24];
    unsigned long so, sl;
    FILE *f;
    if (hdic) return 1;
    hanja_path(path);
    f = fopen(path, "rb");
    if (!f) { sprintf(msg, "한자 사전 %s 을(를) 찾을 수 없습니다.", path); return 0; }
    if (fread(hd, 1, 24, f) != 24 || memcmp(hd, "HJD2", 4) != 0) goto bad;
    so = rd32(hd + 4); sl = rd32(hd + 8);
    hw_idx = rd32(hd + 12); hw_n = rd32(hd + 16);
    hdic = (unsigned char *)malloc(sl ? sl : 1);
    if (!hdic || fseek(f, so, SEEK_SET) != 0 || fread(hdic, 1, sl, f) != sl ||
        sl < 8 || memcmp(hdic, "HJD1", 4) != 0 || 8 + 8L * rd16(hdic + 4) > (long)sl)
        goto bad;
    hdic_len = sl;
    fclose(f);
    return 1;
bad:
    free(hdic); hdic = 0;
    fclose(f);
    sprintf(msg, "한자 사전 %s 이(가) 올바르지 않습니다. 새 HANJA.DIC 을 쓰십시오.", path);
    return 0;
}

static int hc_reserve(int n)
{
    if (n > hccap) {
        HCand *t = (HCand *)realloc(hc, n * sizeof(HCand));
        if (!t) return 0;
        hc = t; hccap = n;
    }
    return 1;
}

/* 음절의 후보 목록 위치. 없으면 0 */
static const unsigned char *syl_entry(wch syl, int *cnt)
{
    int n = rd16(hdic + 4), lo = 0, hi = n - 1;
    const unsigned char *idx = hdic + 8, *data = hdic + 8 + 8L * n;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned k = rd16(idx + 8 * mid);
        if (syl < k) hi = mid - 1;
        else if (syl > k) lo = mid + 1;
        else { *cnt = rd16(idx + 8 * mid + 2); return data + rd32(idx + 8 * mid + 4); }
    }
    return 0;
}

/* 한자 한 글자의 뜻 ("집 가"). 없으면 빈 문자열 */
static void syl_meaning(wch syl, wch hz, char *out, int max)
{
    int cnt, i;
    const unsigned char *p = syl_entry(syl, &cnt);
    out[0] = 0;
    for (i = 0; p && i < cnt && p + 3 <= hdic + hdic_len; i++) {
        if (rd16(p) == hz) {
            int n = p[2] < max - 1 ? p[2] : max - 1;
            memcpy(out, p + 3, n); out[n] = 0;
            return;
        }
        p += 3 + p[2];
    }
}

/* 한 글자: 후보를 hc[]에 채움 */
static int hanja_lookup_syl(wch syl)
{
    int cnt, i;
    const unsigned char *p = syl_entry(syl, &cnt);
    nhc = 0;
    if (!p || !hc_reserve(cnt)) return 0;
    for (i = 0; i < cnt && p + 3 <= hdic + hdic_len; i++) {
        int n = p[2] < (int)sizeof(hc[i].m) - 1 ? p[2] : (int)sizeof(hc[i].m) - 1;
        hc[i].hz[0] = (wch)rd16(p);
        memcpy(hc[i].m, p + 3, n); hc[i].m[n] = 0;
        p += 3 + p[2];
    }
    return nhc = i;
}

/* 낱말: 사전 파일에서 바로 찾아 후보를 hc[]에 채움 (글자마다 뜻을 이어 붙임) */
static int hanja_lookup_word(FILE *f, const wch *key, int n)
{
    long lo = 0, hi = (long)hw_n - 1;
    unsigned char b[4 + 2 * HMAXW];
    nhc = 0;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        int kl, i, c = 0;
        if (fseek(f, hw_idx + 4 * mid, SEEK_SET) || fread(b, 1, 4, f) != 4) return 0;
        if (fseek(f, rd32(b), SEEK_SET) || fread(b, 1, 1, f) != 1) return 0;
        kl = b[0];
        if (kl > HMAXW || fread(b + 1, 2, kl, f) != (size_t)kl) return 0;
        for (i = 0; i < kl && i < n && !c; i++) {
            unsigned k = rd16(b + 1 + 2 * i);
            c = key[i] < k ? -1 : key[i] > k ? 1 : 0;
        }
        if (!c) c = n < kl ? -1 : n > kl ? 1 : 0;
        if (c < 0) hi = mid - 1;
        else if (c > 0) lo = mid + 1;
        else {
            int cnt, j;
            if (fread(b, 1, 1, f) != 1) return 0;
            cnt = b[0];
            if (!hc_reserve(cnt)) return 0;
            for (j = 0; j < cnt; j++) {
                char one[40];
                if (fread(b, 2, n, f) != (size_t)n) break;
                hc[j].m[0] = 0;
                for (i = 0; i < n; i++) {
                    hc[j].hz[i] = (wch)rd16(b + 2 * i);
                    syl_meaning(key[i], hc[j].hz[i], one, sizeof(one));
                    if (strchr(one, ',')) *strchr(one, ',') = 0;  /* 뜻은 첫째 것만 */
                    if (!one[0]) wch_utf8(key[i], one);
                    if (strlen(hc[j].m) + strlen(one) + 3 < sizeof(hc[j].m)) {
                        if (i) strcat(hc[j].m, "·");
                        strcat(hc[j].m, one);
                    }
                }
            }
            return nhc = j;
        }
    }
    return 0;
}

static void hanja_overlay(void)
{
    int page = hsel / HPAGE, first = page * HPAGE, pages = (nhc + HPAGE - 1) / HPAGE;
    int i, j, x = hbox_x, y = hbox_y, w = hbox_w, h = HPAGE + 4, tw0 = 0;
    char title[80], ch[4], num[4];
    strcpy(title, "한자: ");
    for (i = 0; i < hlen; i++) { wch_utf8(hkey[i], ch); strcat(title, ch); }
    sprintf(title + strlen(title), " (%d/%d)", page + 1, pages);
    dlg_frame(x, y, w, h, title);
    for (i = 0; i < hlen; i++) tw0 += uwidth(hc[0].hz[i]);
    for (i = 0; i < HPAGE; i++) {
        int k = first + i, yy = y + 1 + i, sel = (k == hsel), cx2;
        int fg = sel ? C_WHITE : C_BLACK, bg = sel ? C_BLUE : C_LGRAY;
        const char *mp;
        if (k >= nhc) continue;
        scr_fill(x + 1, yy, w - 2, 1, ' ', fg, bg);
        sprintf(num, "%d", i + 1);
        scr_puts(x + 2, yy, num, sel ? C_YELLOW : C_RED, bg);
        cx2 = x + 4;
        for (j = 0; j < hlen; j++) { scr_put(cx2, yy, hc[k].hz[j], fg, bg); cx2 += uwidth(hc[k].hz[j]); }
        cx2 = x + 4 + tw0 + 2;
        mp = hc[k].m;
        while (*mp) {                   /* 뜻: 상자 폭에 맞게 자름 */
            wch c = utf8_next(&mp);
            if (cx2 + uwidth(c) > x + w - 2) break;
            scr_put(cx2, yy, c, sel ? C_WHITE : C_DGRAY, bg);
            cx2 += uwidth(c);
        }
    }
    scr_puts(x + 2, y + h - 2, "1-9 고르기 PgDn 다음 Esc 취소", C_DGRAY, C_LGRAY);
}

/* 한자 바꾸기 (F9 / 한자 키)
   - 블록이 있으면: 블록(한 줄 안의 한글)을 낱말로, 한 글자면 한 글자로 바꿈
   - 없으면: 커서 바로 앞에 이어진 한글 중 사전에 있는 가장 긴 낱말을 바꿈.
     낱말이 없으면 커서 앞(없으면 커서 자리)의 한 글자를 바꿈
   바꾼 것은 되돌리기(Ctrl-Z)로 되돌릴 수 있음 */
static void do_hanja(void)
{
    int y, x = 0, y1, x1, y2, x2, k, r = 0, sy, sx, i, n = 0, found = 0;
    Line *l = &lines[cy];
    char path[180];
    FILE *f;

    if (!hanja_load()) return;
    hanja_path(path);
    f = fopen(path, "rb");

    if (sel_range(&y1, &x1, &y2, &x2)) {
        n = x2 - x1;
        if (y1 != y2 || n > HMAXW) {
            strcpy(msg, "한자로 바꿀 블록은 한 줄 안의 한글 12자 이하여야 합니다.");
            goto out;
        }
        for (i = 0; i < n; i++)
            if (!is_syllable(lines[y1].s[x1 + i])) {
                strcpy(msg, "한자로 바꿀 블록에는 한글만 있어야 합니다.");
                goto out;
            }
        y = y1; x = x1;
        memcpy(hkey, lines[y].s + x, n * sizeof(wch));
        found = (n == 1) ? hanja_lookup_syl(hkey[0]) : (f && hanja_lookup_word(f, hkey, n));
    } else {
        int run = 0;
        y = cy;
        while (run < HMAXW && cx - run > 0 && is_syllable(l->s[cx - run - 1])) run++;
        for (n = run; n >= 2 && !found; n--) {      /* 가장 긴 낱말부터 */
            memcpy(hkey, l->s + cx - n, n * sizeof(wch));
            if (f && hanja_lookup_word(f, hkey, n)) { found = 1; x = cx - n; break; }
        }
        if (!found) {                               /* 한 글자 */
            n = 1;
            if (cx > 0 && is_syllable(l->s[cx - 1])) x = cx - 1;
            else if (cx < l->len && is_syllable(l->s[cx])) x = cx;
            else {
                strcpy(msg, "한자로 바꿀 한글 바로 뒤에 커서를 두거나 블록을 잡고 F9(또는 한자 키)를 누르십시오.");
                goto out;
            }
            hkey[0] = l->s[x];
            found = hanja_lookup_syl(hkey[0]);
        }
    }
    hlen = n;
    if (!found) {
        char s8[60];
        s8[0] = 0;
        for (i = 0; i < n && i < HMAXW; i++) { char c3[4]; wch_utf8(hkey[i], c3); strcat(s8, c3); }
        sprintf(msg, "'%s'에 해당하는 한자가 사전에 없습니다.", s8);
        goto out;
    }

    /* 상자 자리: 글자 바로 아래 (모자라면 위) */
    hbox_w = (hlen == 1) ? 32 : 46;
    screen_pos(y, x, &sx, &sy);
    hbox_y = (sy + 1 + HPAGE + 4 <= STATUS_Y) ? sy + 1 : sy - (HPAGE + 4);
    if (hbox_y < 1) hbox_y = 1;
    hbox_x = sx;
    if (hbox_x > SCR_W - hbox_w - 2) hbox_x = SCR_W - hbox_w - 2;
    if (hbox_x < 0) hbox_x = 0;

    hsel = 0;
    overlay = hanja_overlay;
    for (;;) {
        int page = hsel / HPAGE;
        render();
        k = getkey() & ~K_SHIFT;
        if (k == K_ESC || k == K_HANJA || k == K_F(9)) break;
        if (k == K_ENTER) { r = 1; break; }
        if (k >= '1' && k <= '9') {
            int j = page * HPAGE + (k - '1');
            if (j < nhc) { hsel = j; r = 1; break; }
            continue;
        }
        if (k == K_UP && hsel > 0) hsel--;
        else if (k == K_DOWN && hsel < nhc - 1) hsel++;
        else if (k == K_PGDN || k == K_RIGHT || k == ' ') {
            if ((page + 1) * HPAGE < nhc) hsel = (page + 1) * HPAGE;
        } else if (k == K_PGUP || k == K_LEFT) {
            if (page > 0) hsel = (page - 1) * HPAGE;
        } else if (k == K_HOME) hsel = 0;
        else if (k == K_END) hsel = nhc - 1;
    }
    overlay = 0;
    if (r) {
        sel_on = 0;
        edit_replace(y, x, y, x + hlen, hc[hsel].hz, hlen, UK_OTHER);
    }
out:
    if (f) fclose(f);
}

static void exec_cmd(int c)
{
    switch (c) {        /* 커서를 옮기거나 문서를 바꾸는 명령은 블록을 풂 */
    case CMD_NEW: case CMD_OPEN: case CMD_REDO: case CMD_TOP: case CMD_BOTTOM:
    case CMD_FIND: case CMD_FINDNEXT: case CMD_GOTO: case CMD_UNDO:
        sel_on = 0;
        break;
    }
    switch (c) {
    case CMD_NEW:
        if (confirm_discard()) { doc_clear(); filename[0] = 0; }
        break;
    case CMD_OPEN:    do_open(); break;
    case CMD_SAVE:    do_save(); break;
    case CMD_SAVEAS:  do_saveas(); break;
    case CMD_QUIT:    if (confirm_discard()) running = 0; break;
    case CMD_INSMODE: insmode = !insmode; break;
    case CMD_TOP:     cy = 0; cx = 0; goal = -1; break;
    case CMD_BOTTOM:  cy = nlines - 1; cx = lines[cy].len; goal = -1; break;
    case CMD_FIND:    do_find(); break;
    case CMD_FINDNEXT:
        if (findlen) do_findnext(); else do_find();
        break;
    case CMD_GOTO:    do_goto(); break;
    case CMD_KBD2:    set_keyboard(0); break;
    case CMD_KBD3:    set_keyboard(1); break;
    case CMD_HANJA:   do_hanja(); break;
    case CMD_BLOCK:   do_block(); break;
    case CMD_WRAP:                      /* 자동 줄 바꿈 켜기/끄기 */
        wrap_on = !wrap_on;
        left = 0; top_row = 0; goal = -1;
        strcpy(msg, wrap_on ? "자동 줄 바꿈을 켰습니다 (파일 내용은 바뀌지 않음)." : "자동 줄 바꿈을 껐습니다.");
        cfg_save();
        break;
    case CMD_SELALL:                    /* 모두 선택: 문서 처음부터 끝까지 블록 */
        sel_on = 1; sel_sticky = 0;
        sel_y = 0; sel_x = 0;
        cy = nlines - 1; cx = lines[cy].len; goal = -1;
        break;
    case CMD_HELP:    show_help(); break;
    case CMD_ABOUT:   show_about(); break;
    case CMD_CUT:     ed_cut(); break;
    case CMD_COPY:    ed_copy(); break;
    case CMD_PASTE:   ed_paste(); break;
    case CMD_UNDO:    undo_do(); break;
    case CMD_REDO:    redo_do(); break;
    case CMD_FONT_NARROW:
    case CMD_FONT_WIDE:
        text_wide = (c == CMD_FONT_WIDE);
        goal = -1;
        cfg_save();
        break;
    case CMD_COLORS:
        textinv = !textinv;
        vga_textcolors(textinv);
        cfg_save();
        break;
    }
}

/* ============================================================ 메뉴 */
typedef struct { const char *label, *key; int cmd; } MItem;

static const MItem m_file[] = {
    { "새 문서",            "Alt-N",  CMD_NEW },
    { "불러오기...",        "Alt-O",  CMD_OPEN },
    { "저장하기",           "Alt-S",  CMD_SAVE },
    { "다른 이름으로...",   "Alt-V",  CMD_SAVEAS },
    { 0, 0, 0 },
    { "배경색 변경",        "Alt-B",  CMD_COLORS },
    { 0, 0, 0 },
    { "끝",                 "Alt-X",  CMD_QUIT },
};
static const MItem m_edit[] = {
    { "되돌리기",           "Ctrl-Z", CMD_UNDO },
    { "다시 실행",          "Ctrl-Y", CMD_REDO },
    { 0, 0, 0 },
    { "블록 설정",          "F3",     CMD_BLOCK },
    { "모두 선택",          "Ctrl-A", CMD_SELALL },
    { "오려두기",           "Ctrl-X", CMD_CUT },
    { "복사하기",           "Ctrl-C", CMD_COPY },
    { "붙이기",             "Ctrl-V", CMD_PASTE },
    { 0, 0, 0 },
    { "삽입/수정 전환",     "Ins",    CMD_INSMODE },
    { 0, 0, 0 },
    { "문서 처음으로",      "C-Home", CMD_TOP },
    { "문서 끝으로",        "C-End",  CMD_BOTTOM },
};
static const MItem m_find[] = {
    { "찾기...",            "F2",     CMD_FIND },
    { "다시 찾기",          "Ctrl-L", CMD_FINDNEXT },
    { "줄 찾아가기...",     "Alt-G",  CMD_GOTO },
};
static const MItem m_input[] = {
    { "두벌식 자판",        "",       CMD_KBD2 },
    { "세벌식 최종 자판",   "",       CMD_KBD3 },
};
static const MItem m_font[] = {
    { "본문: 넓은 한글 (영문의 두 배 폭)",  "", CMD_FONT_WIDE },
    { "본문: 좁은 한글 (영문과 같은 폭)",   "", CMD_FONT_NARROW },
    { 0, 0, 0 },
    { "자동 줄 바꿈",                       "", CMD_WRAP },
};
static const MItem m_help[] = {
    { "글쇠 도움말",        "F1",     CMD_HELP },
    { "이 프로그램은...",   "",       CMD_ABOUT },
};
static const MItem *menus[NMENU] = { m_file, m_edit, m_find, m_input, m_font, m_help };
static const int menu_n[NMENU] = { 8, 13, 3, 2, 4, 2 };

static int mcur, msel;

static void menu_overlay(void)
{
    const MItem *m = menus[mcur];
    int n = menu_n[mcur], i, w = 0, x, y = MENU_Y + 1;
    for (i = 0; i < n; i++)
        if (m[i].label) {
            int t = utf8_width(m[i].label) + utf8_width(m[i].key) + 6;
            if (t > w) w = t;
        }
    x = menu_x[mcur];
    if (x + w > SCR_W) x = SCR_W - w;
    draw_menubar(mcur);
    scr_shadow(x + 2, y + 1, w, n + 2);
    scr_box(x, y, w, n + 2, C_BLACK, C_LGRAY, 0);
    for (i = 0; i < n; i++) {
        int yy = y + 1 + i, sel = (i == msel);
        int fg = sel ? C_WHITE : C_BLACK, bg = sel ? C_BLUE : C_LGRAY;
        if (!m[i].label) {
            int j;
            for (j = 1; j < w - 1; j++) scr_put(x + j, yy, 0x2500, C_BLACK, C_LGRAY);
            scr_put(x, yy, 0x251C, C_BLACK, C_LGRAY);
            scr_put(x + w - 1, yy, 0x2524, C_BLACK, C_LGRAY);
            continue;
        }
        scr_fill(x + 1, yy, w - 2, 1, ' ', fg, bg);
        scr_puts(x + 2, yy, m[i].label, fg, bg);
        scr_puts(x + w - 2 - utf8_width(m[i].key), yy, m[i].key, sel ? C_YELLOW : C_DGRAY, bg);
        if ((m[i].cmd == CMD_KBD2 && kbdtype == 0) || (m[i].cmd == CMD_KBD3 && kbdtype == 1) ||
            (m[i].cmd == CMD_INSMODE && !insmode) ||
            (m[i].cmd == CMD_FONT_NARROW && !text_wide) ||
            (m[i].cmd == CMD_FONT_WIDE && text_wide) ||
            (m[i].cmd == CMD_WRAP && wrap_on))
            scr_put(x + 1, yy, 0x221A, fg, bg);   /* √ */
    }
}

static void menu_step(int d)
{
    int n = menu_n[mcur];
    do { msel = (msel + d + n) % n; } while (!menus[mcur][msel].label);
}

static int menu_run(int start)
{
    int k, i, cmd = CMD_NONE;
    mcur = start; msel = 0;
    overlay = menu_overlay;
    for (;;) {
        render();
        k = getkey();
        if (k == K_ESC || k == K_F(10) || k == K_MENU) break;
        if (alt_menu(k) >= 0) { mcur = alt_menu(k); msel = 0; continue; }
        if (k == K_LEFT)  { mcur = (mcur + NMENU - 1) % NMENU; msel = 0; continue; }
        if (k == K_RIGHT) { mcur = (mcur + 1) % NMENU; msel = 0; continue; }
        if (k == K_UP)    { menu_step(-1); continue; }
        if (k == K_DOWN)  { menu_step(1); continue; }
        if (k == K_ENTER) { cmd = menus[mcur][msel].cmd; break; }
        if (k < 128) {
            for (i = 0; i < NMENU; i++)
                if (toupper(k) == menu_hot[i]) { mcur = i; msel = 0; }
        }
    }
    overlay = 0;
    return cmd;
}

/* ============================================================ 주 고리 */
static int is_move_key(int k)
{
    switch (k) {
    case K_LEFT: case K_RIGHT: case K_UP: case K_DOWN: case K_PGUP: case K_PGDN:
    case K_HOME: case K_END: case K_CLEFT: case K_CRIGHT:
    case K_CHOME: case K_CEND: case K_CPGUP: case K_CPGDN:
        return 1;
    }
    return 0;
}

/* 커서 이동 글쇠면 옮기고 1 */
static int do_move(int k)
{
    switch (k) {
    case K_LEFT:
        if (cx > 0) cx--; else if (cy > 0) { cy--; cx = lines[cy].len; }
        goal = -1; break;
    case K_RIGHT:
        if (cx < lines[cy].len) cx++; else if (cy < nlines - 1) { cy++; cx = 0; }
        goal = -1; break;
    case K_UP:    if (wrap_on) wrap_vmove(-1); else ed_vmove(-1); break;
    case K_DOWN:  if (wrap_on) wrap_vmove(1); else ed_vmove(1); break;
    case K_PGUP:
        if (wrap_on) { wrap_vmove(-(EDIT_H - 1)); break; }
        top -= EDIT_H - 1; if (top < 0) top = 0; ed_vmove(-(EDIT_H - 1)); break;
    case K_PGDN:
        if (wrap_on) { wrap_vmove(EDIT_H - 1); break; }
        top += EDIT_H - 1;
        if (top > nlines - 1) top = nlines - 1;
        ed_vmove(EDIT_H - 1); break;
    case K_HOME:
    case K_END:
        goal = -1;
        if (wrap_on) {                  /* 자동 줄 바꿈: 화면 줄의 처음 / 끝 */
            int rs, re;
            wrap_row_of(lines[cy].s, lines[cy].len, cx, &rs, &re);
            if (k == K_HOME) cx = rs;
            else cx = (re >= lines[cy].len) ? lines[cy].len : re - 1;
        } else cx = (k == K_HOME) ? 0 : lines[cy].len;
        break;
    case K_CLEFT: ed_word(-1); break;
    case K_CRIGHT: ed_word(1); break;
    case K_CHOME: case K_CPGUP: cy = 0; cx = 0; goal = -1; break;
    case K_CEND:  case K_CPGDN: cy = nlines - 1; cx = lines[cy].len; goal = -1; break;
    default: return 0;
    }
    return 1;
}

static int q_pending;           /* Ctrl-Q 다음 글쇠를 기다림 (Ctrl-Q,F = 찾기) */

static void handle_key_(int k);

static void handle_key(int k)
{
    handle_key_(k);
    last_key = k & ~K_SHIFT;
}

static void handle_key_(int k)
{
    int shifted = (k & K_SHIFT) != 0;
    k &= ~K_SHIFT;

    if (!sel_on) sel_sticky = 0;

    if (q_pending) {                    /* Ctrl-Q,F 찾기 / Ctrl-Q,A 찾아 바꾸기 */
        q_pending = 0;
        if (k == 'f' || k == 'F' || k == 6) { ed_commit(); exec_cmd(CMD_FIND); return; }
        if (k == 'a' || k == 'A' || k == 1) { strcpy(msg, "찾아 바꾸기는 아직 없습니다."); return; }
    }

    if (k == K_HANTOGGLE) { ed_commit(); hanmode = !hanmode; return; }

    /* 이동 글쇠: Shift와 함께 또는 F3 블록 상태면 블록을 넓히고, 아니면 블록을 풂 */
    if (is_move_key(k)) {
        int oy, ox;
        ed_commit();                    /* 조합 중인 글자를 먼저 확정 */
        undo_seal = 1;                  /* 커서를 옮기면 입력 묶음이 끝남 */
        oy = cy; ox = cx;
        do_move(k);
        if (shifted || (sel_sticky && sel_on)) {
            if (!sel_on) { sel_on = 1; sel_y = oy; sel_x = ox; }
        } else sel_on = 0;
        return;
    }

    /* 블록이 있을 때 글자를 치면 블록을 지우고 그 자리에 입력 */
    if ((k >= ' ' && k < 127) || (k >= 0x80 && k < 0x100) || k == K_ENTER || k == K_TAB)
        sel_delete();

    if (k >= ' ' && k < 127) { han_type(k, ed_insert, 0); return; }
    if (k >= 0x80 && k < 0x100) { ed_insert(0, (wch)k); return; }

    if (k == K_BKSP) { if (!sel_delete()) ed_backspace(); return; }

    ed_commit();
    /* 아래아 한글식 오려두기/복사/붙이기: Shift-Del, Ctrl-Ins, Shift-Ins */
    if (k == K_DEL && shifted) { exec_cmd(CMD_CUT); return; }
    if (k == K_INS && shifted) { exec_cmd(CMD_PASTE); return; }
    if (k == K_CINS)           { exec_cmd(CMD_COPY); return; }
    if (k == K_DEL)  { if (!sel_delete()) ed_delete(); return; }
    undo_seal = 1;                      /* 다른 명령도 입력 묶음을 끝냄 */
    if (k == K_ESC && sel_on) { sel_on = 0; return; }

    if (alt_menu(k) >= 0) { exec_cmd(menu_run(alt_menu(k))); return; }

    switch (k) {
    case K_ENTER: ed_newline(); break;
    case K_TAB: {
            static const wch sp[8] = { ' ',' ',' ',' ',' ',' ',' ',' ' };
            int i, n = 8 - (dcol(&lines[cy], cx) % 8);
            for (i = 0; i < n; i++) ed_insert(0, sp[i]);
        }
        break;
    case K_INS:   insmode = !insmode; break;
    case 26:                                    /* Ctrl-Z / Ctrl-Shift-Z */
        exec_cmd(shifted ? CMD_REDO : CMD_UNDO); break;
    case 25:      exec_cmd(CMD_REDO); break;    /* Ctrl-Y */
    case 24:      exec_cmd(CMD_CUT); break;     /* Ctrl-X */
    case 3:       exec_cmd(CMD_COPY); break;    /* Ctrl-C */
    case 22:      exec_cmd(CMD_PASTE); break;   /* Ctrl-V */
    case K_F(9):  case K_HANJA: exec_cmd(CMD_HANJA); break;
    case K_EXT(0x30): exec_cmd(CMD_COLORS); break;  /* Alt-B */
    case K_F(3):  exec_cmd(CMD_BLOCK); break;   /* 블록 설정 */
    case 1:       exec_cmd(CMD_SELALL); break;  /* Ctrl-A: 모두 선택 */
    case K_EXT(0x22): case 7: exec_cmd(CMD_GOTO); break;   /* Alt-G (Ctrl-G도) */
    case K_F(1):  exec_cmd(CMD_HELP); break;
    case K_ALT_S: case 19: exec_cmd(CMD_SAVE); break;      /* Alt-S, Ctrl-S */
    case K_ALT_O: exec_cmd(CMD_OPEN); break;
    case K_ALT_V: exec_cmd(CMD_SAVEAS); break;
    case K_ALT_N: exec_cmd(CMD_NEW); break;
    case K_ALT_X: exec_cmd(CMD_QUIT); break;
    case K_F(2):  case 6: exec_cmd(CMD_FIND); break;       /* F2, Ctrl-F */
    case 12:      exec_cmd(CMD_FINDNEXT); break;           /* Ctrl-L */
    case 17:                                               /* Ctrl-Q */
        q_pending = 1;
        strcpy(msg, "Ctrl-Q: F 찾기");
        break;
    case K_ESC: case K_F(10): case K_MENU:
        exec_cmd(menu_run(0)); break;
    }
}

int main(int argc, char **argv)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGBREAK, SIG_IGN);

    ic = hangul_ic_new("2");
    if (!ic) { fprintf(stderr, "HANEDIT: libhangul init failed\n"); return 1; }

    doc_clear();
    if (argc > 1) {
        strncpy(filename, argv[1], sizeof(filename) - 1);
        strupr(filename);
        if (doc_load(filename) == -1) sprintf(msg, "새 파일: %s", filename);
        else if (!msg[0]) sprintf(msg, "%s 을(를) 불러왔습니다 (%d줄)", filename, nlines);
    }

    cfg_path(argv[0]);
    cfg_load();

    vga_init();
    vga_textcolors(textinv);
    kbd_init();

    while (running) {
        int k;
        render();
        k = getkey();
        msg[0] = 0;
        handle_key(k);
    }

    kbd_done();
    vga_done();
    hangul_ic_delete(ic);
    printf("HANEDIT - bye.\n");
    return 0;
}
