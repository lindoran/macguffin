/*
 * editor.c  --  minimal CP437 text editor via thin-vga
 *
 * Deterministic, fixed-geometry 80x25 text mode.
 */

#ifdef __ia16__
#  include <i86.h>
#  include <conio.h>
#  include <dos.h>
#  include <bios.h>
#  define MAX_LINES  2048
#  define LOAD_BUF   256
#  define VGA_ROWS   25
#  define VGA_COLS   80

#  define VGA_BLACK         0
#  define VGA_BLUE          1
#  define VGA_GREEN         2
#  define VGA_CYAN          3
#  define VGA_RED           4
#  define VGA_MAGENTA       5
#  define VGA_BROWN         6
#  define VGA_LGRAY         7
#  define VGA_DGRAY         8
#  define VGA_LBLUE         9
#  define VGA_LGREEN       10
#  define VGA_LCYAN        11
#  define VGA_LRED         12
#  define VGA_LMAGENTA     13
#  define VGA_YELLOW       14
#  define VGA_WHITE        15

#  define VGA_ATTR(bg, fg) (unsigned char)(((bg) & 0x0F) << 4 | ((fg) & 0x0F))
#  define VGA_ATTR_DEFAULT VGA_ATTR(VGA_BLACK, VGA_LGRAY)
#  define KEY_CTRL(c) ((c) & 0x1F)
#  define KEY_BS     0x08
#  define KEY_ENTER  0x0D
#  define KEY_TAB    0x09
#  define KEY_UP     0x4800
#  define KEY_DOWN   0x5000
#  define KEY_LEFT   0x4B00
#  define KEY_RIGHT  0x4D00
#  define KEY_HOME   0x4700
#  define KEY_END    0x4F00
#  define KEY_PGUP   0x4900
#  define KEY_PGDN   0x5100
#  define KEY_INS    0x5200
#  define KEY_DEL    0x5300
#  define KEY_NONE   -2
#  define KEY_CLOSED -1
#else
#  include "vgaterm.h"
#  include "vio.h"
#  define MAX_LINES  65536
#  define LOAD_BUF   4096
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Line buffer                                                        */
/* ------------------------------------------------------------------ */
#define LINE_INIT  128

/* ------------------------------------------------------------------ */
/*  PCL3 page geometry                                                 */
/* ------------------------------------------------------------------ */
#define PCL_LPI    6
#define PCL_CPI    10
#define PCL_LPP    66
#define PCL_CPL    80

typedef struct {
    char *buf;
    int   len;
    int   cap;
} Line;

static Line lines[MAX_LINES];
static int  nlines = 1;

/* ------------------------------------------------------------------ */
/*  Line helpers                                                       */
/* ------------------------------------------------------------------ */
static void line_init(Line *l)
{
    l->cap = LINE_INIT;
    l->buf = (char *)malloc((size_t)l->cap);
    l->buf[0] = '\0';
    l->len = 0;
}

static void line_free(Line *l)
{
    free(l->buf);
    l->buf = NULL;
    l->len = l->cap = 0;
}

static void line_grow(Line *l, int need)
{
    while (l->cap <= need + 1) {
        l->cap *= 2;
        l->buf = (char *)realloc(l->buf, (size_t)l->cap);
    }
}

static void line_ins(Line *l, int pos, char c)
{
    line_grow(l, l->len + 1);
    memmove(l->buf + pos + 1, l->buf + pos, (size_t)(l->len - pos + 1));
    l->buf[pos] = c;
    l->len++;
}

static void line_del(Line *l, int pos)
{
    if (pos < 0 || pos >= l->len) return;
    memmove(l->buf + pos, l->buf + pos + 1, (size_t)(l->len - pos));
    l->len--;
}

static void split_line(int row, int pos)
{
    int i, tail;
    if (nlines >= MAX_LINES) return;

    for (i = nlines; i > row + 1; i--)
        lines[i] = lines[i - 1];
    nlines++;

    line_init(&lines[row + 1]);
    tail = lines[row].len - pos;
    line_grow(&lines[row + 1], tail);
    memcpy(lines[row + 1].buf, lines[row].buf + pos, (size_t)(tail + 1));
    lines[row + 1].len = tail;

    lines[row].buf[pos] = '\0';
    lines[row].len = pos;
}

static void join_lines(int row)
{
    int i, old_len;
    if (row + 1 >= nlines) return;

    old_len = lines[row].len;
    line_grow(&lines[row], old_len + lines[row + 1].len);
    memcpy(lines[row].buf + old_len,
           lines[row + 1].buf,
           (size_t)(lines[row + 1].len + 1));
    lines[row].len += lines[row + 1].len;

    line_free(&lines[row + 1]);
    for (i = row + 1; i < nlines - 1; i++)
        lines[i] = lines[i + 1];
    nlines--;
}

/* ------------------------------------------------------------------ */
/*  Editor state                                                       */
/* ------------------------------------------------------------------ */
static int  cur_row  = 0;
static int  cur_col  = 0;
static int  top_row  = 0;
static int  ins_mode = 1;
static int  modified = 0;
static int  running  = 1;
static char fname[512] = "[new]";
static int  fname_len   = 5;   /* strlen("[new]") */

static unsigned char line_dirty[MAX_LINES];
static int status_dirty  = 1;
static int content_dirty = 1;

#ifdef __ia16__
/* --- DOS VIO Stubs --- */
static int s_attr = VGA_ATTR_DEFAULT;
static int s_col = 0, s_row = 0;

static void vio_gotoxy(int col, int row) {
    union REGS r;
    s_col = col; s_row = row;
    r.h.ah = 0x02; r.h.bh = 0;
    r.h.dh = (unsigned char)row; r.h.dl = (unsigned char)col;
    int86(0x10, &r, &r);
}

static void vio_setattr(int attr) { s_attr = attr; }

static void vio_putch(unsigned char ch) {
    unsigned char __far *vga = (unsigned char __far *)0xB8000000L;
    int off = (s_row * 80 + s_col) * 2;
    vga[off] = ch;
    vga[off+1] = (unsigned char)s_attr;
    if (s_col < 79) s_col++;
    vio_gotoxy(s_col, s_row);
}

static void vio_puts(const char *s) {
    while (*s) vio_putch((unsigned char)*s++);
}

static void vio_puts_n(const char *s, int n) {
    int i;
    for (i = 0; i < n; i++) {
        vio_putch(s[i] ? (unsigned char)s[i] : ' ');
        if (!s[i]) s = "";
    }
}

static void vio_clreol(void) {
    int c, old_col = s_col;
    for (c = s_col; c < 80; c++) vio_putch(' ');
    vio_gotoxy(old_col, s_row);
}

static void vio_clrline(int row, int attr) {
    int c;
    int old_attr = s_attr;
    vio_gotoxy(0, row);
    vio_setattr(attr);
    for (c = 0; c < 80; c++) vio_putch(' ');
    vio_setattr(old_attr);
}

static void vio_clrscr(void) {
    int r;
    for (r = 0; r < 25; r++) vio_clrline(r, s_attr);
    vio_gotoxy(0, 0);
}

static void vio_uint(unsigned int n, int width) {
    char buf[10];
    int i = 0;
    if (n == 0) buf[i++] = '0';
    while (n > 0) { buf[i++] = (char)('0' + (n % 10)); n /= 10; }
    while (width > i) { vio_putch(' '); width--; }
    while (i > 0) vio_putch((unsigned char)buf[--i]);
}

static void vio_show_cursor(void) { vio_gotoxy(s_col, s_row); }
static void vio_flush(void) { }
static void vio_init(void *vt) { (void)vt; }
static void vio_fini(void) { }

static int vio_getch(void) {
    unsigned short k = _bios_keybrd(_KEYBRD_READ);
    if ((k & 0xFF) != 0) return (int)(k & 0xFF);
    return (int)(k & 0xFF00);
}

static int vio_kbhit(void) {
    unsigned short k = _bios_keybrd(_KEYBRD_READY);
    if (k == 0) return KEY_NONE;
    return vio_getch();
}
#endif

#define EDIT_ROWS (VGA_ROWS - 1)

/* ------------------------------------------------------------------ */
/*  Cursor clamp + visibility                                          */
/* ------------------------------------------------------------------ */
static void clamp_col(void)
{
    if (cur_col > lines[cur_row].len)
        cur_col = lines[cur_row].len;
}

static void ensure_visible(void)
{
    int old_top = top_row;

    if (cur_row < top_row)
        top_row = cur_row;
    if (cur_row >= top_row + EDIT_ROWS)
        top_row = cur_row - EDIT_ROWS + 1;

    if (top_row < 0) top_row = 0;
    if (top_row > nlines - 1) top_row = nlines - 1;

    if (top_row != old_top) {
        int r;
        for (r = 0; r < EDIT_ROWS; r++) {
            int lr = top_row + r;
            if (lr >= 0 && lr < nlines)
                line_dirty[lr] = 1;
        }
        content_dirty = 1;
    }
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */
static void draw_status(void)
{
    int page = cur_row / PCL_LPP + 1;
    int line = cur_row % PCL_LPP + 1;
    int col  = cur_col + 1;

    /* Clear status line with cyan background */
    vio_setattr(VGA_ATTR(VGA_CYAN, VGA_BLACK));
    vio_clrline(VGA_ROWS - 1, VGA_ATTR(VGA_CYAN, VGA_BLACK));

    /* Filename and Modified flag */
    vio_gotoxy(1, VGA_ROWS - 1);
    vio_puts(fname);
    if (modified) vio_puts(" *");

    /* Page, Line, Col stats */
    vio_gotoxy(25, VGA_ROWS - 1);
    vio_puts("Pg ");  vio_uint(page, 3);
    vio_puts("  Ln "); vio_uint(line, 4);
    vio_puts("  Col "); vio_uint(col, 3);

    /* Mode and Typeface info */
    vio_gotoxy(62, VGA_ROWS - 1);
    vio_puts("10 CPI  ");
    vio_puts(ins_mode ? "INSERT" : "OVERWRITE");

    status_dirty = 0;
}

static void draw_content(void)
{
    int r;
    char *txt;
    int len;

    for (r = 0; r < EDIT_ROWS; r++) {
        int lr = top_row + r;
        if (lr < 0 || lr >= nlines) {
            vio_gotoxy(0, r);
            vio_setattr(VGA_ATTR_DEFAULT);
            vio_clreol();
            continue;
        }
        if (!line_dirty[lr])
            continue;

        txt = lines[lr].buf;
        len = lines[lr].len;
        if (len > VGA_COLS) len = VGA_COLS;

        vio_gotoxy(0, r);
        vio_setattr(VGA_ATTR_DEFAULT);
        if (len > 0)
            vio_puts_n(txt, len);

        if (len < VGA_COLS)
            vio_clreol();

        line_dirty[lr] = 0;
    }

    content_dirty = 0;
}

/* ------------------------------------------------------------------ */
/*  File I/O                                                           */
/* ------------------------------------------------------------------ */
static void reset_editor(void)
{
    int i;
    for (i = 0; i < nlines; i++)
        line_free(&lines[i]);

    nlines = 1;
    cur_row = cur_col = top_row = 0;
    modified = 0;

    line_init(&lines[0]);

    for (i = 0; i < MAX_LINES; i++)
        line_dirty[i] = 1;

    status_dirty  = 1;
    content_dirty = 1;
}

static void do_new(void)
{
    reset_editor();
    strcpy(fname, "[new]");
    fname_len = 5;
}

static int do_save(void)
{
    FILE *f;
    int i;

    if (strcmp(fname, "[new]") == 0)
        return 0;

    f = fopen(fname, "wb");
    if (!f) return -1;

    for (i = 0; i < nlines; i++) {
        fwrite(lines[i].buf, 1, (size_t)lines[i].len, f);
        if (i < nlines - 1) fputc('\n', f);
    }
    fclose(f);

    modified = 0;
    status_dirty = 1;
    return 0;
}

static void do_load(const char *path)
{
    FILE *f;
    char buf[LOAD_BUF];
    int len, i;

    f = fopen(path, "rb");
    if (!f) return;

    reset_editor();
    nlines = 0;

    while (fgets(buf, (int)sizeof(buf), f)) {
        len = (int)strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';

        line_init(&lines[nlines]);
        line_grow(&lines[nlines], len);
        memcpy(lines[nlines].buf, buf, (size_t)(len + 1));
        lines[nlines].len = len;
        nlines++;
        if (nlines >= MAX_LINES) break;
    }
    fclose(f);

    if (nlines == 0) {
        line_init(&lines[0]);
        nlines = 1;
    }
    
    fname_len = 0;
    strncpy(fname, path, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';

    fname_len = (int)strlen(fname);

    for (i = 0; i < nlines; i++)
        line_dirty[i] = 1;

    status_dirty  = 1;
    content_dirty = 1;
}

/* ------------------------------------------------------------------ */
/*  Input handling                                                     */
/* ------------------------------------------------------------------ */
static void handle_key(int ch)
{
    if (ch == KEY_CLOSED) {
        running = 0;
        return;
    }

    switch (ch) {

    case KEY_CTRL('q'):
        running = 0;
        break;

    case KEY_CTRL('s'):
        do_save();
        break;

    case KEY_CTRL('n'):
        do_new();
        break;

    /* Movement ------------------------------------------------------ */
    case KEY_UP:
        if (cur_row > 0) {
            cur_row--;
            clamp_col();
            ensure_visible();
            status_dirty = 1;
        }
        break;

    case KEY_DOWN:
        if (cur_row < nlines - 1) {
            cur_row++;
            clamp_col();
            ensure_visible();
            status_dirty = 1;
        }
        break;

    case KEY_LEFT:
        if (cur_col > 0) {
            cur_col--;
            status_dirty = 1;
        } else if (cur_row > 0) {
            cur_row--;
            cur_col = lines[cur_row].len;
            ensure_visible();
            status_dirty = 1;
        }
        break;

    case KEY_RIGHT:
        if (cur_col < lines[cur_row].len) {
            cur_col++;
            status_dirty = 1;
        } else if (cur_row < nlines - 1) {
            cur_row++;
            cur_col = 0;
            ensure_visible();
            status_dirty = 1;
        }
        break;

    case KEY_HOME:
        cur_col = 0;
        status_dirty = 1;
        break;

    case KEY_END:
        cur_col = lines[cur_row].len;
        status_dirty = 1;
        break;

    case KEY_PGUP:
        cur_row -= EDIT_ROWS - 1;
        if (cur_row < 0) cur_row = 0;
        clamp_col();
        ensure_visible();
        status_dirty = 1;
        break;

    case KEY_PGDN:
        cur_row += EDIT_ROWS - 1;
        if (cur_row >= nlines) cur_row = nlines - 1;
        clamp_col();
        ensure_visible();
        status_dirty = 1;
        break;

    /* Insert/overwrite ---------------------------------------------- */
    case KEY_INS:
        ins_mode = !ins_mode;
        status_dirty = 1;
        break;

    /* Newline -------------------------------------------------------- */
    case KEY_ENTER:
        split_line(cur_row, cur_col);
        line_dirty[cur_row] = 1;
        line_dirty[cur_row + 1] = 1;
        cur_row++;
        cur_col = 0;
        modified = 1;
        content_dirty = 1;
        status_dirty = 1;
        ensure_visible();
        break;

    case KEY_TAB: {
        int i;
        for (i = 0; i < 4; i++) handle_key(' ');
        break;
    }

    /* Backspace ------------------------------------------------------ */
    case KEY_BS:
        if (cur_col > 0) {
            cur_col--;
            line_del(&lines[cur_row], cur_col);
            line_dirty[cur_row] = 1;
            modified = 1;
            content_dirty = 1;
            status_dirty = 1;
        } else if (cur_row > 0) {
            int prev_len = lines[cur_row - 1].len;
            join_lines(cur_row - 1);
            line_dirty[cur_row - 1] = 1;
            modified = 1;
            cur_row--;
            cur_col = prev_len;
            content_dirty = 1;
            status_dirty = 1;
            ensure_visible();
        }
        break;

    /* Delete --------------------------------------------------------- */
    case KEY_DEL:
        if (cur_col < lines[cur_row].len) {
            line_del(&lines[cur_row], cur_col);
            line_dirty[cur_row] = 1;
            modified = 1;
            content_dirty = 1;
            status_dirty = 1;
        } else if (cur_row < nlines - 1) {
            join_lines(cur_row);
            line_dirty[cur_row] = 1;
            modified = 1;
            content_dirty = 1;
            status_dirty = 1;
        }
        break;

    /* Printable ------------------------------------------------------ */
    default:
        if (ch >= 32 && ch <= 255) {
            if (ins_mode) {
                line_ins(&lines[cur_row], cur_col, (char)ch);
            } else {
                if (cur_col < lines[cur_row].len)
                    lines[cur_row].buf[cur_col] = (char)ch;
                else
                    line_ins(&lines[cur_row], cur_col, (char)ch);
            }
            cur_col++;
            modified = 1;
            line_dirty[cur_row] = 1;
            content_dirty = 1;
            status_dirty = 1;

            /* --- Simple Word Wrap --- */
            if (cur_col >= VGA_COLS) {
                int split_at = -1;
                int i;
                for (i = cur_col - 1; i >= 0; i--) {
                    if (lines[cur_row].buf[i] == ' ') {
                        split_at = i;
                        break;
                    }
                }
                if (split_at > 0 && split_at > cur_col - 20) {
                    split_line(cur_row, split_at + 1);
                    line_del(&lines[cur_row], split_at);
                    cur_row++;
                    cur_col = cur_col - (split_at + 1);
                } else {
                    split_line(cur_row, cur_col);
                    cur_row++;
                    cur_col = 0;
                }
                ensure_visible();
                for (i = 0; i < VGA_ROWS; i++) {
                    int lr = top_row + i;
                    if (lr >= 0 && lr < MAX_LINES) line_dirty[lr] = 1;
                }
            }
        }
        break;
}
}

#ifdef __ia16__
typedef void VGATerm;
#endif

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int ch;
    VGATerm *vt = NULL;

#ifndef __ia16__
    vt = vgaterm_open("editor");
    if (!vt) return 1;
#endif

    vio_init(vt);
    vio_setattr(VGA_ATTR_DEFAULT);
    vio_clrscr();

    line_init(&lines[0]);
    if (argc > 1) do_load(argv[1]);

    ensure_visible();

    while (running) {
        if (content_dirty)
            draw_content();

        if (status_dirty)
            draw_status();

        vio_gotoxy(cur_col, cur_row - top_row);
        vio_show_cursor();
        vio_flush();

        ch = vio_getch();
        handle_key(ch);

        /* Drain queued keys */
        while (running && (ch = vio_kbhit()) != KEY_NONE) {
            handle_key(ch);

            if (content_dirty)
                draw_content();
            if (status_dirty)
                draw_status();

            vio_gotoxy(cur_col, cur_row - top_row);
            vio_show_cursor();
            vio_flush();
        }
    }

    vio_fini();
#ifndef __ia16__
    vgaterm_close(vt);
#endif
    return 0;
}
