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
#  define KEY_ESC    0x1B
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
#define PAGE_HEADER_MAX PCL_CPL
#define LINE_FLAG_REPEAT 0x01
#define LINE_FLAG_PAGE_BREAK 0x02

typedef struct {
    char *buf;
    int   len;
    int   cap;
} Line;

typedef struct {
    int left_page;
    int right_page;
    int left_tab;
    int right_tab;
    int center_tab;
    int tab_size;
} TabStops;

typedef struct {
    int  active;
    int  has_error;
    int  pending_save_as;
    char error_msg[80];
    char pending_path[512];
    char input[80];
    int  input_len;
    int  saved_row;
    int  saved_col;
} Console;

static Line lines[MAX_LINES];
static int  nlines = 1;
static unsigned char line_flags[MAX_LINES];

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
    if (pos > l->len) pos = l->len;
    line_grow(l, l->len + 1);
    memmove(l->buf + pos + 1, l->buf + pos, (size_t)(l->len - pos + 1));
    l->buf[pos] = c;
    l->len++;
}

static void line_pad_to(Line *l, int pos)
{
    if (pos <= l->len) return;
    line_grow(l, pos);
    while (l->len < pos)
        l->buf[l->len++] = ' ';
    l->buf[l->len] = '\0';
}

static void line_del(Line *l, int pos)
{
    if (pos < 0 || pos >= l->len) return;
    memmove(l->buf + pos, l->buf + pos + 1, (size_t)(l->len - pos));
    l->len--;
}

static void line_truncate(Line *l, int len)
{
    if (len < 0) len = 0;
    if (len < l->len) {
        l->buf[len] = '\0';
        l->len = len;
    }
}

static void split_line(int row, int pos)
{
    int i, tail;
    if (nlines >= MAX_LINES) return;
    if (pos < 0) pos = 0;
    if (pos > lines[row].len)
        line_pad_to(&lines[row], pos);

    for (i = nlines; i > row + 1; i--) {
        lines[i] = lines[i - 1];
        line_flags[i] = line_flags[i - 1];
    }
    nlines++;
    line_flags[row + 1] = 0;

    line_init(&lines[row + 1]);
    tail = lines[row].len - pos;
    line_grow(&lines[row + 1], tail);
    memcpy(lines[row + 1].buf, lines[row].buf + pos, (size_t)(tail + 1));
    lines[row + 1].len = tail;

    lines[row].buf[pos] = '\0';
    lines[row].len = pos;
}

static void insert_line_copy(int row, const char *buf, int len,
                             unsigned char flags)
{
    int i;

    if (nlines >= MAX_LINES) return;
    if (row < 0) row = 0;
    if (row > nlines) row = nlines;
    if (len > VGA_COLS) len = VGA_COLS;

    for (i = nlines; i > row; i--) {
        lines[i] = lines[i - 1];
        line_flags[i] = line_flags[i - 1];
    }
    nlines++;
    line_flags[row] = flags;

    line_init(&lines[row]);
    line_grow(&lines[row], len);
    if (len > 0)
        memcpy(lines[row].buf, buf, (size_t)len);
    lines[row].buf[len] = '\0';
    lines[row].len = len;
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
    for (i = row + 1; i < nlines - 1; i++) {
        lines[i] = lines[i + 1];
        line_flags[i] = line_flags[i + 1];
    }
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
static int  page_len = PCL_LPP;
static char fname[512] = "[new]";
static int  fname_len   = 5;   /* strlen("[new]") */

static unsigned char line_dirty[MAX_LINES];
static int status_dirty  = 1;
static int content_dirty = 1;
static int ruler_dirty   = 1;

static TabStops tabs;
static Console console_state;
static char page_header[PAGE_HEADER_MAX + 1];
static int  page_header_len = 0;
static int  page_header_active = 0;

#ifdef __ia16__
/* --- DOS VIO Stubs --- */
static int s_attr = VGA_ATTR_DEFAULT;
static int s_col = 0, s_row = 0;

static void vio_gotoxy(int col, int row) {
    s_col = col; s_row = row;
}

static void vio_setattr(int attr) { s_attr = attr; }

static void vio_putch(unsigned char ch) {
    unsigned char __far *vga = (unsigned char __far *)0xB8000000L;
    int off = (s_row * 80 + s_col) * 2;
    vga[off] = ch;
    vga[off+1] = (unsigned char)s_attr;
    if (s_col < 79) s_col++;
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

static void vio_show_cursor(void) {
    union REGS r;
    r.h.ah = 0x02; r.h.bh = 0;
    r.h.dh = (unsigned char)s_row; r.h.dl = (unsigned char)s_col;
    int86(0x10, &r, &r);
}
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

#define EDIT_TOP_ROW 1
#define EDIT_ROWS (VGA_ROWS - 2)

/* ------------------------------------------------------------------ */
/*  Tab stops + console                                                */
/* ------------------------------------------------------------------ */
static int tab_center(int left_tab, int right_tab)
{
    return (left_tab + right_tab) / 2;
}

static int tab_values_valid(int left_page, int left_tab, int right_tab,
                            int right_page)
{
    int center_tab = tab_center(left_tab, right_tab);
    int page_tabs = left_page == left_tab && right_tab == right_page;

    return left_page >= 0 &&
           right_page < VGA_COLS &&
           (page_tabs || left_page < left_tab) &&
           left_tab < center_tab &&
           center_tab < right_tab &&
           (page_tabs || right_tab < right_page);
}

static void tab_init(void)
{
    tabs.left_page = 0;
    tabs.right_page = VGA_COLS - 1;
    tabs.tab_size = 4;
    tabs.left_tab = tabs.left_page + tabs.tab_size;
    tabs.right_tab = tabs.right_page - tabs.tab_size;
    tabs.center_tab = tab_center(tabs.left_tab, tabs.right_tab);
}

static void set_console_error(const char *msg)
{
    if (!console_state.active) {
        console_state.saved_row = cur_row;
        console_state.saved_col = cur_col;
        console_state.input_len = 0;
        console_state.input[0] = '\0';
    }
    console_state.active = 1;
    strncpy(console_state.error_msg, msg, sizeof(console_state.error_msg) - 1);
    console_state.error_msg[sizeof(console_state.error_msg) - 1] = '\0';
    console_state.has_error = 1;
    status_dirty = 1;
}

static void mark_visible_dirty(void)
{
    int r;

    for (r = 0; r < EDIT_ROWS; r++) {
        int lr = top_row + r;
        if (lr >= 0 && lr < MAX_LINES)
            line_dirty[lr] = 1;
    }
    content_dirty = 1;
}

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
    int page = cur_row / page_len + 1;
    int line = cur_row % page_len + 1;
    int col  = cur_col + 1;

    if (console_state.active) {
        int i;
        int input_start = 50;

        vio_setattr(VGA_ATTR(VGA_CYAN, VGA_BLACK));
        vio_clrline(VGA_ROWS - 1, VGA_ATTR(VGA_CYAN, VGA_BLACK));

        if (console_state.has_error) {
            vio_gotoxy(0, VGA_ROWS - 1);
            for (i = 0; i < input_start - 1 && console_state.error_msg[i]; i++)
                vio_putch((unsigned char)console_state.error_msg[i]);
        }

        vio_gotoxy(input_start, VGA_ROWS - 1);
        vio_putch('>');
        if (console_state.input_len > 0)
            vio_puts_n(console_state.input, console_state.input_len);

        status_dirty = 0;
        return;
    }

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

static void draw_ruler(void)
{
    int c;
    char ruler[VGA_COLS];

    for (c = 0; c < VGA_COLS; c++)
        ruler[c] = '-';

    ruler[tabs.left_page] = '|';
    ruler[tabs.right_page] = '|';
    ruler[tabs.left_tab] = 'L';
    ruler[tabs.center_tab] = '^';
    ruler[tabs.right_tab] = 'R';

    vio_gotoxy(0, 0);
    vio_setattr(VGA_ATTR(VGA_BLUE, VGA_LGRAY));
    vio_puts_n(ruler, VGA_COLS);
    ruler_dirty = 0;
}

static int line_attr(int lr)
{
    if (line_flags[lr] & LINE_FLAG_PAGE_BREAK)
        return VGA_ATTR(VGA_BLUE, VGA_WHITE);
    if (line_flags[lr] & LINE_FLAG_REPEAT)
        return VGA_ATTR(VGA_RED, VGA_WHITE);
    return VGA_ATTR_DEFAULT;
}

static void draw_content(void)
{
    int r;
    char *txt;
    int len;

    for (r = 0; r < EDIT_ROWS; r++) {
        int lr = top_row + r;
        if (lr < 0 || lr >= nlines) {
            vio_gotoxy(0, r + EDIT_TOP_ROW);
            vio_setattr(VGA_ATTR_DEFAULT);
            vio_clreol();
            continue;
        }
        if (!line_dirty[lr])
            continue;

        txt = lines[lr].buf;
        len = lines[lr].len;
        if (len > VGA_COLS) len = VGA_COLS;

        vio_gotoxy(0, r + EDIT_TOP_ROW);
        vio_setattr(line_attr(lr));
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
    cur_row = top_row = 0;
    cur_col = tabs.left_tab;
    modified = 0;
    page_header_active = 0;
    page_header_len = 0;
    page_header[0] = '\0';

    line_init(&lines[0]);

    for (i = 0; i < MAX_LINES; i++)
        line_dirty[i] = 1;
    for (i = 0; i < MAX_LINES; i++)
        line_flags[i] = 0;

    status_dirty  = 1;
    content_dirty = 1;
    ruler_dirty   = 1;
}

static void do_new(void)
{
    reset_editor();
    strcpy(fname, "[new]");
    fname_len = 5;
}

static int total_pages(void)
{
    int pages = (nlines + page_len - 1) / page_len;
    return pages > 0 ? pages : 1;
}

static void file_put_uint(FILE *f, int n)
{
    char buf[12];
    int i = 0;

    if (n == 0)
        buf[i++] = '0';
    while (n > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0)
        fputc(buf[--i], f);
}

static void save_line_expanded(FILE *f, Line *l, int row, int pages)
{
    int i;
    int page = row / page_len + 1;

    for (i = 0; i < l->len; i++) {
        if (l->buf[i] == '$' && i + 1 < l->len) {
            if (l->buf[i + 1] == 'p') {
                file_put_uint(f, page);
                i++;
                continue;
            }
            if (l->buf[i + 1] == 't') {
                file_put_uint(f, pages);
                i++;
                continue;
            }
        }
        fputc((unsigned char)l->buf[i], f);
    }
}

static int has_ext(const char *path, const char *ext)
{
    int plen = (int)strlen(path);
    int elen = (int)strlen(ext);
    int i;

    if (plen < elen)
        return 0;
    path += plen - elen;
    for (i = 0; i < elen; i++) {
        char a = path[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static void file_put_hex(FILE *f, const char *buf, int len)
{
    static const char h[] = "0123456789ABCDEF";
    int i;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        fputc(h[c >> 4], f);
        fputc(h[c & 0x0F], f);
    }
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int line_from_hex(Line *l, const char *hex)
{
    int hex_len = (int)strlen(hex);
    int i, hi, lo, len;

    if (hex_len > 0 && hex[hex_len - 1] == '\n') hex_len--;
    if (hex_len > 0 && hex[hex_len - 1] == '\r') hex_len--;
    if ((hex_len & 1) != 0)
        return 0;

    len = hex_len / 2;
    line_grow(l, len);
    for (i = 0; i < len; i++) {
        hi = hex_val((unsigned char)hex[i * 2]);
        lo = hex_val((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        l->buf[i] = (char)((hi << 4) | lo);
    }
    l->buf[len] = '\0';
    l->len = len;
    return 1;
}

static int do_export(const char *path)
{
    FILE *f;
    int i, pages;

    f = fopen(path, "wb");
    if (!f) return -1;

    pages = total_pages();
    for (i = 0; i < nlines; i++) {
        if (!(line_flags[i] & LINE_FLAG_PAGE_BREAK))
            save_line_expanded(f, &lines[i], i, pages);
        if (i < nlines - 1) fputc('\n', f);
    }
    fclose(f);

    return 0;
}

static int save_project(const char *path)
{
    FILE *f;
    int i;

    f = fopen(path, "wb");
    if (!f) return -1;

    fprintf(f, "MGF2\n");
    fprintf(f, "page %d\n", page_len);
    fprintf(f, "header %d ", page_header_active ? 1 : 0);
    file_put_hex(f, page_header, page_header_len);
    fputc('\n', f);
    fprintf(f, "lines %d\n", nlines);
    for (i = 0; i < nlines; i++) {
        fprintf(f, "%u ", (unsigned int)line_flags[i]);
        file_put_hex(f, lines[i].buf, lines[i].len);
        fputc('\n', f);
    }

    fclose(f);
    return 0;
}

static int save_to_path(const char *path, int project)
{
    int err;

    if (project)
        err = save_project(path);
    else
        err = do_export(path);

    if (err != 0)
        return err;

    strncpy(fname, path, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';
    fname_len = (int)strlen(fname);
    modified = 0;
    status_dirty = 1;
    return 0;
}

static int save_as_path(const char *path)
{
    if (!*path)
        return -1;
    if (has_ext(path, ".mgf"))
        return save_to_path(path, 1);
    if (has_ext(path, ".txt"))
        return save_to_path(path, 0);

    strncpy(console_state.pending_path, path,
            sizeof(console_state.pending_path) - 1);
    console_state.pending_path[sizeof(console_state.pending_path) - 1] = '\0';
    console_state.pending_save_as = 1;
    set_console_error("unknown extension. save as mgf? y/n");
    return 1;
}

static int do_save(void)
{
    if (strcmp(fname, "[new]") == 0) {
        set_console_error("type save as <filename>");
        return -1;
    }
    if (!has_ext(fname, ".mgf") && !has_ext(fname, ".txt")) {
        strncpy(console_state.pending_path, fname,
                sizeof(console_state.pending_path) - 1);
        console_state.pending_path[sizeof(console_state.pending_path) - 1] = '\0';
        console_state.pending_save_as = 1;
        set_console_error("unknown extension. save as mgf? y/n");
        return 1;
    }
    return save_to_path(fname, has_ext(fname, ".mgf"));
}

static int load_project(FILE *f, int version)
{
    char buf[LOAD_BUF];
    int count, i;

    if (!fgets(buf, (int)sizeof(buf), f))
        return -1;
    if (sscanf(buf, "page %d", &page_len) != 1 || page_len < 2)
        return -1;

    if (!fgets(buf, (int)sizeof(buf), f))
        return -1;
    if (strncmp(buf, "header ", 7) != 0)
        return -1;
    page_header_active = buf[7] == '1';
    if (buf[8] != ' ')
        return -1;
    {
        Line tmp;
        line_init(&tmp);
        if (!line_from_hex(&tmp, buf + 9)) {
            line_free(&tmp);
            return -1;
        }
        page_header_len = tmp.len;
        if (page_header_len > PAGE_HEADER_MAX)
            page_header_len = PAGE_HEADER_MAX;
        if (page_header_len > 0)
            memcpy(page_header, tmp.buf, (size_t)page_header_len);
        page_header[page_header_len] = '\0';
        line_free(&tmp);
    }

    if (!fgets(buf, (int)sizeof(buf), f))
        return -1;
    if (sscanf(buf, "lines %d", &count) != 1 || count < 1 || count > MAX_LINES)
        return -1;

    line_free(&lines[0]);
    nlines = 0;
    for (i = 0; i < count; i++) {
        char *hex = buf;
        if (!fgets(buf, (int)sizeof(buf), f))
            return -1;
        line_init(&lines[nlines]);
        line_flags[nlines] = 0;
        if (version >= 2) {
            unsigned int flags = 0;
            while (*hex >= '0' && *hex <= '9') {
                flags = flags * 10 + (unsigned int)(*hex - '0');
                hex++;
            }
            if (*hex != ' ')
                return -1;
            hex++;
            line_flags[nlines] = (unsigned char)flags;
        }
        if (!line_from_hex(&lines[nlines], hex))
            return -1;
        nlines++;
    }

    if (nlines == 0) {
        line_init(&lines[0]);
        nlines = 1;
    }
    return 0;
}

static int do_load(const char *path)
{
    FILE *f;
    char buf[LOAD_BUF];
    int len, i;
    int project = has_ext(path, ".mgf");
    int project_version = 0;

    f = fopen(path, "rb");
    if (!f) return -1;

    if (project) {
        if (!fgets(buf, (int)sizeof(buf), f)) {
            fclose(f);
            return -1;
        }
        if (strcmp(buf, "MGF1\n") == 0 || strcmp(buf, "MGF1\r\n") == 0) {
            project_version = 1;
        } else if (strcmp(buf, "MGF2\n") == 0 || strcmp(buf, "MGF2\r\n") == 0) {
            project_version = 2;
        } else {
            fclose(f);
            return -1;
        }
    }

    reset_editor();
    if (project) {
        if (load_project(f, project_version) != 0) {
            fclose(f);
            return -1;
        }
    } else {
        nlines = 0;

        while (fgets(buf, (int)sizeof(buf), f)) {
            len = (int)strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
            if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';

            line_init(&lines[nlines]);
            line_grow(&lines[nlines], len);
            memcpy(lines[nlines].buf, buf, (size_t)(len + 1));
            lines[nlines].len = len;
            line_flags[nlines] = 0;
            nlines++;
            if (nlines >= MAX_LINES) break;
        }
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
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Input handling                                                     */
/* ------------------------------------------------------------------ */
static void prefix_line_spaces(Line *l, int count)
{
    int i;

    if (count <= 0) return;
    line_grow(l, l->len + count);
    memmove(l->buf + count, l->buf, (size_t)(l->len + 1));
    for (i = 0; i < count; i++)
        l->buf[i] = ' ';
    l->len += count;
}

static void reserve_page_header_if_needed(void)
{
    if (!page_header_active)
        return;
    if (page_len <= 1)
        return;
    if (cur_row <= 0 || cur_row % page_len != 0)
        return;
    if (nlines >= MAX_LINES)
        return;

    insert_line_copy(cur_row, page_header, page_header_len, LINE_FLAG_REPEAT);
    line_dirty[cur_row] = 1;
    cur_row++;
    line_dirty[cur_row] = 1;
    mark_visible_dirty();
}

static void repeat_current_row(void)
{
    Line *l = &lines[cur_row];

    page_header_len = l->len;
    if (page_header_len > PAGE_HEADER_MAX)
        page_header_len = PAGE_HEADER_MAX;
    if (page_header_len > 0)
        memcpy(page_header, l->buf, (size_t)page_header_len);
    page_header[page_header_len] = '\0';
    page_header_active = 1;
    line_flags[cur_row] |= LINE_FLAG_REPEAT;
    line_dirty[cur_row] = 1;
    modified = 1;
    content_dirty = 1;
    status_dirty = 1;
}

static void make_page_break_text(char *buf, int *len)
{
    const char *label = "- break -";
    int label_len = 9;
    int leading = tabs.center_tab - ((label_len - 1) / 2);

    if (leading < tabs.left_page)
        leading = tabs.left_page;
    memset(buf, ' ', (size_t)leading);
    memcpy(buf + leading, label, (size_t)label_len);
    *len = leading + label_len;
}

static void insert_page_break(void)
{
    char marker[VGA_COLS];
    int marker_len;
    int row;

    if (nlines >= MAX_LINES)
        return;

    split_line(cur_row, cur_col);
    row = cur_row + 1;
    make_page_break_text(marker, &marker_len);
    insert_line_copy(row, marker, marker_len, LINE_FLAG_PAGE_BREAK);
    line_dirty[cur_row] = 1;
    line_dirty[row] = 1;
    row++;

    while (row % page_len != 0 && nlines < MAX_LINES) {
        insert_line_copy(row, "", 0, 0);
        line_dirty[row] = 1;
        row++;
    }

    cur_row = row;
    reserve_page_header_if_needed();
    prefix_line_spaces(&lines[cur_row], tabs.left_tab);
    cur_col = tabs.left_tab;
    line_dirty[cur_row] = 1;
    modified = 1;
    mark_visible_dirty();
    status_dirty = 1;
    ensure_visible();
}

static void wrap_current_line(void)
{
    int wrap_col = tabs.right_tab;
    int split_at = -1;
    int split_pos;
    int old_col = cur_col;
    int i;

    if (cur_col <= tabs.right_tab + 1 && lines[cur_row].len <= tabs.right_page + 1)
        return;
    if (nlines >= MAX_LINES)
        return;

    for (i = wrap_col; i > tabs.left_tab; i--) {
        if (i < lines[cur_row].len && lines[cur_row].buf[i] == ' ') {
            split_at = i;
            break;
        }
    }

    if (split_at > tabs.left_tab) {
        split_pos = split_at + 1;
        split_line(cur_row, split_pos);
        line_del(&lines[cur_row], split_at);
        cur_col = tabs.left_tab + old_col - split_pos;
    } else {
        split_pos = wrap_col + 1;
        if (split_pos > lines[cur_row].len)
            split_pos = lines[cur_row].len;
        split_line(cur_row, split_pos);
        cur_col = tabs.left_tab + old_col - split_pos;
    }

    cur_row++;
    if (cur_col < tabs.left_tab)
        cur_col = tabs.left_tab;
    reserve_page_header_if_needed();
    prefix_line_spaces(&lines[cur_row], tabs.left_tab);

    line_dirty[cur_row - 1] = 1;
    line_dirty[cur_row] = 1;
    mark_visible_dirty();
    ensure_visible();
}

static void insert_printable(int ch)
{
    Line *l = &lines[cur_row];

    if (cur_col < tabs.left_page)
        cur_col = tabs.left_page;
    if (cur_col > tabs.right_page + 1)
        cur_col = tabs.right_page + 1;

    line_pad_to(l, cur_col);
    if (ins_mode) {
        line_ins(l, cur_col, (char)ch);
    } else {
        if (cur_col < l->len)
            l->buf[cur_col] = (char)ch;
        else
            line_ins(l, cur_col, (char)ch);
    }

    cur_col++;
    modified = 1;
    line_dirty[cur_row] = 1;
    content_dirty = 1;
    status_dirty = 1;

    wrap_current_line();
}

static int find_cursor_word(int *word_start, int *word_end)
{
    Line *l = &lines[cur_row];
    int pos = cur_col;

    if (l->len == 0)
        return 0;
    if (pos >= l->len)
        pos = l->len - 1;
    if (l->buf[pos] == ' ') {
        if (pos == 0 || l->buf[pos - 1] == ' ')
            return 0;
        pos--;
    }
    if (l->buf[pos] == ' ')
        return 0;

    *word_start = pos;
    while (*word_start > 0 && l->buf[*word_start - 1] != ' ')
        (*word_start)--;

    *word_end = pos;
    while (*word_end + 1 < l->len && l->buf[*word_end + 1] != ' ')
        (*word_end)++;

    return 1;
}

static void justify_current_word(void)
{
    Line *l = &lines[cur_row];
    int start, end, word_len, anchor_col, anchor_index;
    int leading, new_len, make_break = 0;
    char *word;

    if (!find_cursor_word(&start, &end))
        return;

    word_len = end - start + 1;
    if (end < tabs.center_tab) {
        anchor_col = tabs.center_tab;
        anchor_index = (word_len - 1) / 2;
    } else if (end == tabs.right_tab) {
        anchor_col = tabs.right_page;
        anchor_index = word_len - 1;
        make_break = 1;
    } else if (end == tabs.right_page) {
        return;
    } else {
        anchor_col = tabs.right_tab;
        anchor_index = word_len - 1;
    }

    leading = anchor_col - anchor_index;
    if (leading < tabs.left_page || leading + word_len - 1 > tabs.right_page)
        return;
    if (leading < start)
        return;

    word = (char *)malloc((size_t)word_len);
    if (!word)
        return;
    memcpy(word, l->buf + start, (size_t)word_len);

    new_len = leading + word_len;
    line_grow(l, new_len);
    if (leading > start)
        memset(l->buf + start, ' ', (size_t)(leading - start));
    memcpy(l->buf + leading, word, (size_t)word_len);
    l->buf[new_len] = '\0';
    l->len = new_len;
    line_truncate(l, new_len);
    free(word);

    cur_col = new_len;
    modified = 1;
    line_dirty[cur_row] = 1;
    content_dirty = 1;
    status_dirty = 1;

    if (make_break && nlines < MAX_LINES) {
        split_line(cur_row, l->len);
        cur_row++;
        reserve_page_header_if_needed();
        prefix_line_spaces(&lines[cur_row], tabs.left_tab);
        cur_col = tabs.left_tab;
        line_dirty[cur_row] = 1;
        ensure_visible();
    }
}

static void console_exit(void)
{
    console_state.active = 0;
    console_state.has_error = 0;
    console_state.pending_save_as = 0;
    console_state.input_len = 0;
    console_state.input[0] = '\0';
    cur_row = console_state.saved_row;
    cur_col = console_state.saved_col;
    status_dirty = 1;
}

static void console_enter(void)
{
    console_state.active = 1;
    console_state.has_error = 0;
    console_state.pending_save_as = 0;
    console_state.input_len = 0;
    console_state.input[0] = '\0';
    console_state.saved_row = cur_row;
    console_state.saved_col = cur_col;
    status_dirty = 1;
}

static int parse_uint(const char *s, int *out)
{
    int n = 0;

    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        n = n * 10 + (*s - '0');
        s++;
    }
    *out = n;
    return 1;
}

static void command_tabs(const char *arg)
{
    int n, left_tab, right_tab;

    if (!parse_uint(arg, &n)) {
        set_console_error("tabs needs a number");
        return;
    }

    left_tab = tabs.left_page + n;
    right_tab = tabs.right_page - n;
    if (!tab_values_valid(tabs.left_page, left_tab, right_tab, tabs.right_page)) {
        set_console_error("tabs would cross current stops");
        return;
    }

    tabs.tab_size = n;
    tabs.left_tab = left_tab;
    tabs.right_tab = right_tab;
    tabs.center_tab = tab_center(tabs.left_tab, tabs.right_tab);
    ruler_dirty = 1;
    status_dirty = 1;
}

static void command_stops(const char *arg)
{
    int n, left_page, right_page, left_tab, right_tab;

    if (!parse_uint(arg, &n)) {
        set_console_error("stops needs a number");
        return;
    }

    left_page = n;
    right_page = (VGA_COLS - 1) - n;
    left_tab = left_page + tabs.tab_size;
    right_tab = right_page - tabs.tab_size;
    if (!tab_values_valid(left_page, left_tab, right_tab, right_page)) {
        set_console_error("stops would cross current tabs");
        return;
    }

    tabs.left_page = left_page;
    tabs.right_page = right_page;
    tabs.left_tab = left_tab;
    tabs.right_tab = right_tab;
    tabs.center_tab = tab_center(tabs.left_tab, tabs.right_tab);
    ruler_dirty = 1;
    status_dirty = 1;
}

static void command_page(const char *arg)
{
    int n;

    if (!parse_uint(arg, &n) || n < 2) {
        set_console_error("page needs a number greater than 1");
        return;
    }

    page_len = n;
    status_dirty = 1;
}

static void handle_pending_save_as(int ch)
{
    int err;

    if (ch == 'y' || ch == 'Y') {
        err = save_to_path(console_state.pending_path, 1);
        console_state.pending_save_as = 0;
        if (err != 0) {
            set_console_error("save failed");
            return;
        }
        console_exit();
        return;
    }

    if (ch == 'n' || ch == 'N') {
        console_state.pending_save_as = 0;
        set_console_error("type save as <filename>");
    }
}

static void console_execute(void)
{
    char cmd[80];
    char *arg;

    strncpy(cmd, console_state.input, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';
    arg = cmd;
    while (*arg == ' ') arg++;

    if (strncmp(arg, "tabs ", 5) == 0) {
        command_tabs(arg + 5);
    } else if (strncmp(arg, "tab ", 4) == 0) {
        command_tabs(arg + 4);
    } else if (strncmp(arg, "stops ", 6) == 0) {
        command_stops(arg + 6);
    } else if (strncmp(arg, "page ", 5) == 0) {
        command_page(arg + 5);
    } else if (strcmp(arg, "break") == 0) {
        insert_page_break();
    } else if (strcmp(arg, "save") == 0) {
        if (do_save() < 0 && !console_state.has_error)
            set_console_error("save failed");
    } else if (strncmp(arg, "save as ", 8) == 0) {
        if (save_as_path(arg + 8) < 0 && !console_state.has_error)
            set_console_error("save failed");
    } else if (strncmp(arg, "export ", 7) == 0) {
        if (do_export(arg + 7) != 0)
            set_console_error("export failed");
    } else if (strncmp(arg, "load ", 5) == 0) {
        if (do_load(arg + 5) != 0)
            set_console_error("load failed");
        else {
            console_state.saved_row = cur_row;
            console_state.saved_col = cur_col;
        }
    } else if (strcmp(arg, "quit") == 0) {
        running = 0;
    } else if (arg[0] == '\0') {
        console_exit();
    } else {
        set_console_error("unknown command");
    }

    if (!console_state.has_error && running)
        console_exit();
}

static void handle_console_key(int ch)
{
    if (console_state.pending_save_as) {
        if (ch == KEY_ESC) {
            console_state.pending_save_as = 0;
            console_state.has_error = 0;
            status_dirty = 1;
            return;
        }
        handle_pending_save_as(ch);
        return;
    }

    if (ch == KEY_ESC) {
        if (console_state.has_error) {
            console_state.has_error = 0;
            status_dirty = 1;
        } else {
            console_exit();
        }
        return;
    }

    if (ch == KEY_ENTER) {
        console_execute();
        return;
    }

    if (ch == KEY_BS) {
        if (console_state.input_len > 0) {
            console_state.input[--console_state.input_len] = '\0';
            status_dirty = 1;
        }
        return;
    }

    if (ch >= 32 && ch <= 126) {
        int max_len = (VGA_COLS - 1) - 51;
        if (console_state.input_len < max_len) {
            console_state.input[console_state.input_len++] = (char)ch;
            console_state.input[console_state.input_len] = '\0';
            console_state.has_error = 0;
            status_dirty = 1;
        }
    }
}

static void handle_key(int ch)
{
    if (ch == KEY_CLOSED) {
        running = 0;
        return;
    }

    if (console_state.active) {
        handle_console_key(ch);
        return;
    }

    switch (ch) {

    case KEY_ESC:
        console_enter();
        break;

    case KEY_CTRL('q'):
        running = 0;
        break;

    case KEY_CTRL('s'):
        if (do_save() < 0 && !console_state.has_error)
            set_console_error("save failed");
        break;

    case KEY_CTRL('n'):
        do_new();
        break;

    case KEY_CTRL('j'):
        justify_current_word();
        break;

    case KEY_CTRL('r'):
        repeat_current_row();
        break;

    case KEY_CTRL('b'):
        insert_page_break();
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
            cur_col = tabs.left_tab;
            ensure_visible();
            status_dirty = 1;
        }
        break;

    case KEY_HOME:
        cur_col = tabs.left_tab;
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
        reserve_page_header_if_needed();
        prefix_line_spaces(&lines[cur_row], tabs.left_tab);
        cur_col = tabs.left_tab;
        modified = 1;
        content_dirty = 1;
        status_dirty = 1;
        ensure_visible();
        break;

    case KEY_TAB: {
        int i;
        for (i = 0; i < tabs.tab_size; i++) insert_printable(' ');
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
            insert_printable(ch);
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

    tab_init();
    memset(&console_state, 0, sizeof(console_state));

    line_init(&lines[0]);
    cur_col = tabs.left_tab;
    if (argc > 1) do_load(argv[1]);

    ensure_visible();

    while (running) {
        if (ruler_dirty)
            draw_ruler();

        if (content_dirty)
            draw_content();

        if (status_dirty)
            draw_status();

        if (console_state.active)
            vio_gotoxy(51 + console_state.input_len, VGA_ROWS - 1);
        else
            vio_gotoxy(cur_col, cur_row - top_row + EDIT_TOP_ROW);
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

            if (ruler_dirty)
                draw_ruler();

            if (console_state.active)
                vio_gotoxy(51 + console_state.input_len, VGA_ROWS - 1);
            else
                vio_gotoxy(cur_col, cur_row - top_row + EDIT_TOP_ROW);
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
