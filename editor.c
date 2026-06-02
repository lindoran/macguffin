/*
 * editor.c  --  minimal CP437 text editor via thin-vga
 *
 * Deterministic, fixed-geometry 80x25 text mode.
 *
 * _POSIX_C_SOURCE 200809L: enables popen, pclose, mkstemp, snprintf.
 */
#define _POSIX_C_SOURCE 200809L

#include "vgaterm.h"
#include "vio.h"

#define MAX_LINES  65536
#define LOAD_BUF   4096

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>

/* ------------------------------------------------------------------ */
/*  Rich text format attribute bits                                    */
/* ------------------------------------------------------------------ */
#define FMT_BOLD      0x01
#define FMT_ITALIC    0x02
#define FMT_UNDERLINE 0x04

#include "font_italic.h"

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
#define LINE_FLAG_REPEAT     0x01
#define LINE_FLAG_PAGE_BREAK 0x02
#define LINE_FLAG_FOOTER     0x04

typedef struct {
    char          *buf; /* character data                        */
    unsigned char *fmt; /* per-char format attrs (FMT_* bitmask) */
    int            len;
    int            cap;
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
    int  pending_spell_suggest;    /* in suggestion scroll mode      */
    char error_msg[80];
    char pending_path[512];
    char input[80];
    int  input_len;
    int  saved_row;
    int  saved_col;
} Console;

/* ------------------------------------------------------------------ */
/*  Spell check state                                                  */
/* ------------------------------------------------------------------ */
#define SPELL_MAX_WORDS   512
#define SPELL_WORD_LEN     64
#define SPELL_MAX_SUGS     16

typedef struct {
    char words[SPELL_MAX_WORDS][SPELL_WORD_LEN]; /* misspelled words   */
    int  count;                                  /* words found        */
    int  idx;                                    /* current position   */
    int  active;                                 /* results loaded     */
    char sugs[SPELL_MAX_SUGS][SPELL_WORD_LEN];  /* suggestions        */
    int  sug_count;                              /* suggestions found  */
    int  sug_idx;                                /* selected sug       */
} SpellState;

static SpellState spell_state;

/* Forward declaration — defined later in editing section.           */
static void wrap_current_line(void);

/* ------------------------------------------------------------------ */
/*  Mark / kill state                                                  */
/* ------------------------------------------------------------------ */
typedef enum {
    MARK_NONE = 0,
    MARK_CTRL_K,     /* anchor placed, navigating freely to second mark */
    MARK_DEFINED     /* region closed, showing kill/cancel prompt        */
} MarkMode;

typedef struct {
    MarkMode mode;
    int      anchor_row;
    int      anchor_col;
} MarkState;

static MarkState mark_state;

/* ------------------------------------------------------------------ */
/*  Undelete slot                                                      */
/* ------------------------------------------------------------------ */
#define UNDEL_MAX (VGA_COLS * 256)   /* up to 256 lines of content    */

static char          undel_text[UNDEL_MAX];
static unsigned char undel_fmt_buf[UNDEL_MAX];
static int           undel_len        = 0;
static int           undel_active     = 0;
static int           undel_was_insert = 1;  /* 0 = OVR blank, 1 = INS collapse */

static Line lines[MAX_LINES];
static int  nlines = 1;
static unsigned char line_flags[MAX_LINES];

static void mark_visible_dirty(void);

/* ------------------------------------------------------------------ */
/*  Line helpers                                                       */
/* ------------------------------------------------------------------ */
static void die_oom(void)
{
    fputs("macguffin: out of memory\n", stderr);
    exit(1);
}

static void line_init(Line *l)
{
    l->cap = LINE_INIT;
    l->buf = (char *)malloc((size_t)l->cap);
    l->fmt = (unsigned char *)malloc((size_t)l->cap);
    if (!l->buf || !l->fmt)
        die_oom();
    l->buf[0] = '\0';
    l->fmt[0] = 0;
    l->len = 0;
}

static void line_free(Line *l)
{
    free(l->buf);
    free(l->fmt);
    l->buf = NULL;
    l->fmt = NULL;
    l->len = l->cap = 0;
}

static void line_grow(Line *l, int need)
{
    char *tmp;
    unsigned char *ftmp;
    if (l->cap == 0) l->cap = LINE_INIT;
    while (l->cap <= need + 1)
        l->cap *= 2;
    tmp = (char *)realloc(l->buf, (size_t)l->cap);
    if (!tmp)
        die_oom();
    l->buf = tmp;
    ftmp = (unsigned char *)realloc(l->fmt, (size_t)l->cap);
    if (!ftmp)
        die_oom();
    l->fmt = ftmp;
}

static void line_ins(Line *l, int pos, char c, unsigned char f)
{
    if (pos > l->len) pos = l->len;
    line_grow(l, l->len + 1);
    memmove(l->buf + pos + 1, l->buf + pos, (size_t)(l->len - pos + 1));
    memmove(l->fmt + pos + 1, l->fmt + pos, (size_t)(l->len - pos));
    l->buf[pos] = c;
    l->fmt[pos] = f;
    l->len++;
}

static void line_pad_to(Line *l, int pos)
{
    if (pos <= l->len) return;
    line_grow(l, pos);
    while (l->len < pos) {
        l->buf[l->len] = ' ';
        l->fmt[l->len] = 0;
        l->len++;
    }
    l->buf[l->len] = '\0';
}

static void line_del(Line *l, int pos)
{
    if (pos < 0 || pos >= l->len) return;
    memmove(l->buf + pos, l->buf + pos + 1, (size_t)(l->len - pos));
    memmove(l->fmt + pos, l->fmt + pos + 1, (size_t)(l->len - pos - 1));
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
    memcpy(lines[row + 1].fmt, lines[row].fmt + pos, (size_t)tail);
    lines[row + 1].fmt[tail] = 0;
    lines[row + 1].len = tail;

    lines[row].buf[pos] = '\0';
    lines[row].len = pos;
    mark_visible_dirty();
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
    if (len > 0) {
        memcpy(lines[row].buf, buf, (size_t)len);
        memset(lines[row].fmt, 0, (size_t)len);
    }
    lines[row].buf[len] = '\0';
    lines[row].len = len;
    mark_visible_dirty();
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
    memcpy(lines[row].fmt + old_len,
           lines[row + 1].fmt,
           (size_t)lines[row + 1].len);
    lines[row].len += lines[row + 1].len;

    line_free(&lines[row + 1]);
    for (i = row + 1; i < nlines - 1; i++) {
        lines[i] = lines[i + 1];
        line_flags[i] = line_flags[i + 1];
    }
    nlines--;
    mark_visible_dirty();
}

/* ------------------------------------------------------------------ */
/*  Editor state                                                       */
/* ------------------------------------------------------------------ */
static int  cur_row  = 0;
static int  cur_col  = 0;
static int  top_row  = 0;
static int  ins_mode = 1;
static int  cur_fmt  = 0;   /* active format: FMT_BOLD | FMT_ITALIC | FMT_UNDERLINE */
static int  modified = 0;
static int  running  = 1;
static int  page_len = PCL_LPP;
static char fname[512] = "[new]";

static unsigned char line_dirty[MAX_LINES];
static int status_dirty  = 1;
static int content_dirty = 1;
static int ruler_dirty   = 1;

static TabStops tabs;
static Console console_state;

static VGATerm *g_vt     = NULL; /* set after vgaterm_open; used by command_scale */
static int      vga_scale = 1;   /* current scaling: 1, 2, or 4                   */
static char page_header[PAGE_HEADER_MAX + 1];
static int  page_header_len = 0;
static int  page_header_active = 0;

static char page_footer[PAGE_HEADER_MAX + 1];
static int  page_footer_len = 0;
static int  page_footer_active = 0;

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
/*  Undo system                                                        */
/* ------------------------------------------------------------------ */

#define UNDO_MAX 512

typedef enum {
    UNDO_INS_CHAR,   /* undo: delete char at (row,col)              */
    UNDO_DEL_CHAR,   /* undo: insert char ch/fmt at (row,col)       */
    UNDO_SPLIT,      /* undo: join lines[row] and lines[row+1]      */
    UNDO_JOIN,       /* undo: split lines[row] at col               */
    UNDO_OVR_BLANK   /* undo: restore ch/fmt at (row,col) in place  */
} UndoType;

typedef struct {
    UndoType      type;
    int           row, col;
    char          ch;
    unsigned char fmt_byte;
    char          line_buf[VGA_COLS + 1];
    unsigned char line_fmt[VGA_COLS];
    int           line_len;
} UndoRecord;

static UndoRecord undo_stack[UNDO_MAX];
static int        undo_top   = 0;
static int        undo_count = 0;

static UndoRecord *undo_push(void)
{
    UndoRecord *r = &undo_stack[undo_top];
    undo_top = (undo_top + 1) % UNDO_MAX;
    if (undo_count < UNDO_MAX) undo_count++;
    return r;
}

static void undo_push_ins_char(void)
{
    UndoRecord *r = undo_push();
    r->type = UNDO_INS_CHAR;
    r->row  = cur_row;
    r->col  = cur_col;
}

static void undo_push_del_char(int row, int col)
{
    UndoRecord *r = undo_push();
    r->type     = UNDO_DEL_CHAR;
    r->row      = row;
    r->col      = col;
    r->ch       = lines[row].buf[col];
    r->fmt_byte = lines[row].fmt[col];
}

static void undo_push_split(void)
{
    UndoRecord *r = undo_push();
    r->type = UNDO_SPLIT;
    r->row  = cur_row;
    r->col  = cur_col;
}

static void undo_push_join(int row, int split_col)
{
    UndoRecord *r  = undo_push();
    Line       *l1 = &lines[row + 1];
    int         tail = l1->len < VGA_COLS ? l1->len : VGA_COLS;
    r->type     = UNDO_JOIN;
    r->row      = row;
    r->col      = split_col;
    r->line_len = tail;
    memcpy(r->line_buf, l1->buf, (size_t)tail);
    r->line_buf[tail] = '\0';
    memcpy(r->line_fmt, l1->fmt, (size_t)tail);
}

/* Record an overwrite-blank so Ctrl+Z can restore the original char. */
static void undo_push_ovr_blank(int row, int col)
{
    UndoRecord *r = undo_push();
    r->type     = UNDO_OVR_BLANK;
    r->row      = row;
    r->col      = col;
    r->ch       = (col < lines[row].len) ? lines[row].buf[col] : ' ';
    r->fmt_byte = (col < lines[row].len) ? lines[row].fmt[col] : 0;
}

static void do_undo(void)
{
    UndoRecord *r;

    if (undo_count == 0) {
        set_console_error("nothing to undo");
        return;
    }

    undo_top = (undo_top - 1 + UNDO_MAX) % UNDO_MAX;
    undo_count--;
    r = &undo_stack[undo_top];

    switch (r->type) {

    case UNDO_INS_CHAR:
        cur_row = r->row;
        cur_col = r->col;
        if (cur_col < lines[cur_row].len) {
            line_del(&lines[cur_row], cur_col);
            line_dirty[cur_row] = 1;
        }
        break;

    case UNDO_DEL_CHAR:
        cur_row = r->row;
        cur_col = r->col;
        line_ins(&lines[cur_row], cur_col, r->ch, r->fmt_byte);
        line_dirty[cur_row] = 1;
        break;

    case UNDO_SPLIT:
        cur_row = r->row;
        cur_col = r->col;
        if (cur_row + 1 < nlines) {
            join_lines(cur_row);
            line_dirty[cur_row] = 1;
            mark_visible_dirty();
        }
        break;

    case UNDO_JOIN:
        cur_row = r->row;
        cur_col = r->col;
        split_line(cur_row, r->col);
        if (cur_row + 1 < nlines) {
            Line *l = &lines[cur_row + 1];
            line_grow(l, r->line_len);
            memcpy(l->buf, r->line_buf, (size_t)r->line_len);
            l->buf[r->line_len] = '\0';
            memcpy(l->fmt, r->line_fmt, (size_t)r->line_len);
            l->len = r->line_len;
            line_dirty[cur_row + 1] = 1;
        }
        line_dirty[cur_row] = 1;
        mark_visible_dirty();
        break;

    case UNDO_OVR_BLANK:
        /* Restore original char in place — no shifting.             */
        cur_row = r->row;
        cur_col = r->col;
        if (cur_col < lines[cur_row].len) {
            lines[cur_row].buf[cur_col] = r->ch;
            lines[cur_row].fmt[cur_col] = r->fmt_byte;
            line_dirty[cur_row] = 1;
        }
        break;
    }

    modified      = 1;
    status_dirty  = 1;
    content_dirty = 1;
    ensure_visible();
}

/* ------------------------------------------------------------------ */
/*  Mark helpers                                                       */
/* ------------------------------------------------------------------ */

/* Return region bounds in document order (start <= end row-major).   */
static void mark_region_bounds(int *sr, int *sc, int *er, int *ec)
{
    int ar = mark_state.anchor_row, ac = mark_state.anchor_col;
    int cr = cur_row, cc = cur_col;

    if (ar < cr || (ar == cr && ac <= cc)) {
        *sr = ar; *sc = ac; *er = cr; *ec = cc;
    } else {
        *sr = cr; *sc = cc; *er = ar; *ec = ac;
    }
}

/* Is document cell (lr, c) inside the current defined region?        */
static int mark_cell_selected(int lr, int c)
{
    int sr, sc, er, ec;
    if (mark_state.mode != MARK_DEFINED) return 0;
    mark_region_bounds(&sr, &sc, &er, &ec);
    if (lr < sr || lr > er) return 0;
    if (lr == sr && c < sc) return 0;
    if (lr == er && c >= ec) return 0;
    return 1;
}

/* Clear mark and force full visible redraw.                          */
static void mark_clear(void)
{
    mark_state.mode = MARK_NONE;
    mark_visible_dirty();
    status_dirty  = 1;
    content_dirty = 1;
}

/* Delete line r entirely, shifting everything above it down.         */
static void delete_line(int r)
{
    int i;
    if (r < 0 || r >= nlines) return;
    line_free(&lines[r]);
    for (i = r; i < nlines - 1; i++) {
        lines[i]      = lines[i + 1];
        line_flags[i] = line_flags[i + 1];
        line_dirty[i] = line_dirty[i + 1];
    }
    nlines--;
}

/* Blank chars [start, start+len) on line row with spaces.
 * Used by all OVR-mode kill operations.                              */
static void ovr_blank_range(int row, int start, int len)
{
    int i;
    Line *l = &lines[row];
    for (i = start; i < start + len && i < l->len; i++) {
        l->buf[i] = ' ';
        l->fmt[i] = 0;
    }
    line_dirty[row] = 1;
}

/* Kill the defined marked region into the undelete slot.             */
static void command_kill_mark(void)
{
    int sr, sc, er, ec, r, i;

    if (mark_state.mode != MARK_DEFINED) return;
    mark_region_bounds(&sr, &sc, &er, &ec);

    /* --- Store to undelete slot ----------------------------------- */
    undel_len = 0;

    if (sr == er) {
        int len = ec - sc;
        if (len > 0 && len < UNDEL_MAX) {
            memcpy(undel_text    + undel_len, lines[sr].buf + sc, (size_t)len);
            memcpy(undel_fmt_buf + undel_len, lines[sr].fmt + sc, (size_t)len);
            undel_len += len;
        }
    } else {
        /* First line: sc to end */
        int flen = lines[sr].len - sc;
        if (flen > 0 && undel_len + flen < UNDEL_MAX) {
            memcpy(undel_text    + undel_len, lines[sr].buf + sc, (size_t)flen);
            memcpy(undel_fmt_buf + undel_len, lines[sr].fmt + sc, (size_t)flen);
            undel_len += flen;
        }
        if (undel_len < UNDEL_MAX) {
            undel_text[undel_len]    = '\n';
            undel_fmt_buf[undel_len] = 0;
            undel_len++;
        }
        /* Middle lines */
        for (r = sr + 1; r < er; r++) {
            int mlen = lines[r].len;
            if (undel_len + mlen < UNDEL_MAX) {
                memcpy(undel_text    + undel_len, lines[r].buf, (size_t)mlen);
                memcpy(undel_fmt_buf + undel_len, lines[r].fmt, (size_t)mlen);
                undel_len += mlen;
            }
            if (undel_len < UNDEL_MAX) {
                undel_text[undel_len]    = '\n';
                undel_fmt_buf[undel_len] = 0;
                undel_len++;
            }
        }
        /* Last line: 0 to ec */
        if (ec > 0 && undel_len + ec < UNDEL_MAX) {
            memcpy(undel_text    + undel_len, lines[er].buf, (size_t)ec);
            memcpy(undel_fmt_buf + undel_len, lines[er].fmt, (size_t)ec);
            undel_len += ec;
        }
    }
    undel_active     = (undel_len > 0);
    undel_was_insert = ins_mode;

    /* --- Delete content from buffer ------------------------------ */
    if (!ins_mode) {
        /* OVR: blank region with spaces, geometry unchanged          */
        if (sr == er) {
            ovr_blank_range(sr, sc, ec - sc);
        } else {
            ovr_blank_range(sr, sc, lines[sr].len - sc);
            for (r = sr + 1; r < er; r++)
                ovr_blank_range(r, 0, lines[r].len);
            ovr_blank_range(er, 0, ec);
        }
        cur_row = sr;
        cur_col = sc;
    } else {
        /* INS: collapse — existing logic                             */
        if (sr == er) {
            int count = ec - sc;
            for (i = 0; i < count; i++)
                line_del(&lines[sr], sc);
            line_dirty[sr] = 1;
        } else {
            line_truncate(&lines[sr], sc);
            line_dirty[sr] = 1;
            for (r = er - 1; r > sr; r--)
                delete_line(r);
            for (i = 0; i < ec; i++)
                line_del(&lines[sr + 1], 0);
            line_dirty[sr + 1] = 1;
            join_lines(sr);
            line_dirty[sr] = 1;
        }
        cur_row = sr;
        cur_col = sc;
    }
    mark_state.mode = MARK_NONE;
    modified      = 1;
    content_dirty = 1;
    status_dirty  = 1;
    ensure_visible();
    mark_visible_dirty();
}

/* Restore undelete slot at cursor — preserves fmt.                  */
static void command_undelete(void)
{
    int i;
    if (!undel_active || undel_len == 0) return;

    if (!undel_was_insert) {
        /* OVR kill: overwrite spaces back with original content     */
        for (i = 0; i < undel_len; i++) {
            char          ch  = undel_text[i];
            unsigned char fmt = undel_fmt_buf[i];

            if (ch == '\n') {
                /* Multi-line OVR kill: advance to next line, col 0  */
                cur_row++;
                cur_col = 0;
                if (cur_row >= nlines) break;
            } else {
                Line *l = &lines[cur_row];
                if (cur_col < l->len) {
                    undo_push_ovr_blank(cur_row, cur_col);
                    l->buf[cur_col] = ch;
                    l->fmt[cur_col] = fmt;
                    line_dirty[cur_row] = 1;
                }
                cur_col++;
                modified      = 1;
                content_dirty = 1;
            }
        }
    } else {
        /* INS kill: re-insert, shifting content open                */
        for (i = 0; i < undel_len; i++) {
            char          ch  = undel_text[i];
            unsigned char fmt = undel_fmt_buf[i];

            if (ch == '\n') {
                if (nlines < MAX_LINES) {
                    undo_push_split();
                    split_line(cur_row, cur_col);
                    line_dirty[cur_row]     = 1;
                    line_dirty[cur_row + 1] = 1;
                    cur_row++;
                    cur_col       = 0;
                    modified      = 1;
                    content_dirty = 1;
                }
            } else {
                Line *l = &lines[cur_row];
                if (cur_col < tabs.left_page)      cur_col = tabs.left_page;
                if (cur_col > tabs.right_page + 1) cur_col = tabs.right_page + 1;
                line_pad_to(l, cur_col);
                undo_push_ins_char();
                line_ins(l, cur_col, ch, fmt);
                line_dirty[cur_row] = 1;
                cur_col++;
                modified      = 1;
                content_dirty = 1;
                wrap_current_line();
            }
        }
    }

    ensure_visible();
    status_dirty = 1;
}

/* Kill current line content → undelete slot. Line stays (empty).    */
static void command_kill_line(void)
{
    Line *l   = &lines[cur_row];
    int   len = l->len;

    undel_len        = 0;
    undel_active     = 0;
    undel_was_insert = ins_mode;

    if (len > 0 && len < UNDEL_MAX) {
        memcpy(undel_text,    l->buf, (size_t)len);
        memcpy(undel_fmt_buf, l->fmt, (size_t)len);
        undel_len    = len;
        undel_active = 1;
    }

    if (ins_mode) {
        line_truncate(l, 0);
    } else {
        /* OVR: blank the content, keep the line length              */
        ovr_blank_range(cur_row, 0, len);
    }

    cur_col             = 0;
    line_dirty[cur_row] = 1;
    modified            = 1;
    content_dirty       = 1;
    status_dirty        = 1;
}

/* Kill word backward (to previous word boundary) → undelete slot.   */
static void command_kill_word_backward(void)
{
    Line *l   = &lines[cur_row];
    int   pos = cur_col;
    int   start, len, i;

    if (pos == 0) return;

    while (pos > 0 && l->buf[pos - 1] == ' ') pos--;
    while (pos > 0 && l->buf[pos - 1] != ' ') pos--;

    start = pos;
    len   = cur_col - start;
    if (len <= 0) return;

    undel_was_insert = ins_mode;
    if (len < UNDEL_MAX) {
        memcpy(undel_text,    l->buf + start, (size_t)len);
        memcpy(undel_fmt_buf, l->fmt + start, (size_t)len);
        undel_len    = len;
        undel_active = 1;
    }

    if (ins_mode) {
        for (i = 0; i < len; i++)
            line_del(l, start);
    } else {
        ovr_blank_range(cur_row, start, len);
    }

    cur_col             = start;
    line_dirty[cur_row] = 1;
    modified            = 1;
    content_dirty       = 1;
    status_dirty        = 1;
}

/* Kill word forward (to next word boundary) → undelete slot.        */
static void command_kill_word_forward(void)
{
    Line *l   = &lines[cur_row];
    int   pos = cur_col;
    int   len, i;

    if (pos >= l->len) return;

    while (pos < l->len && l->buf[pos] != ' ') pos++;
    while (pos < l->len && l->buf[pos] == ' ') pos++;

    len = pos - cur_col;
    if (len <= 0) return;

    undel_was_insert = ins_mode;
    if (len < UNDEL_MAX) {
        memcpy(undel_text,    l->buf + cur_col, (size_t)len);
        memcpy(undel_fmt_buf, l->fmt + cur_col, (size_t)len);
        undel_len    = len;
        undel_active = 1;
    }

    if (ins_mode) {
        for (i = 0; i < len; i++)
            line_del(l, cur_col);
    } else {
        ovr_blank_range(cur_row, cur_col, len);
    }

    line_dirty[cur_row] = 1;
    modified            = 1;
    content_dirty       = 1;
    status_dirty        = 1;
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */
static void draw_status(void)
{
    int page = cur_row / page_len + 1;
    int line = cur_row % page_len + 1;
    int col  = cur_col + 1;

    if (mark_state.mode != MARK_NONE && !console_state.active) {
        vio_setattr(VGA_ATTR(VGA_CYAN, VGA_BLACK));
        vio_clrline(VGA_ROWS - 1, VGA_ATTR(VGA_CYAN, VGA_BLACK));
        vio_gotoxy(0, VGA_ROWS - 1);
        if (mark_state.mode == MARK_CTRL_K)
            vio_puts("[^K] second mark   [ESC] cancel");
        else
            vio_puts("[F1] kill   [ESC] cancel");
        vio_gotoxy(55, VGA_ROWS - 1);
        vio_puts("Pg "); vio_uint(page, 3);
        vio_puts(" Ln "); vio_uint(line, 4);
        status_dirty = 0;
        return;
    }

    if (console_state.active) {
        int i;
        int input_start = 50;

        vio_setattr(VGA_ATTR(VGA_CYAN, VGA_BLACK));
        vio_clrline(VGA_ROWS - 1, VGA_ATTR(VGA_CYAN, VGA_BLACK));

        /* Suggestion scroll mode: full-width, no input prompt         */
        if (console_state.pending_spell_suggest) {
            char left[50];
            const char *word = spell_state.words[spell_state.idx];

            if (spell_state.sug_count > 0) {
                snprintf(left, sizeof(left), "spell %d/%d: \"%s\" \xf0 %s",
                         spell_state.idx + 1, spell_state.count, word,
                         spell_state.sugs[spell_state.sug_idx]);
            } else {
                snprintf(left, sizeof(left), "spell %d/%d: \"%s\" (no suggestions)",
                         spell_state.idx + 1, spell_state.count, word);
            }
            vio_gotoxy(0, VGA_ROWS - 1);
            for (i = 0; left[i] && i < input_start - 1; i++)
                vio_putch((unsigned char)left[i]);

            /* Right side: hint text */
            vio_gotoxy(input_start, VGA_ROWS - 1);
            if (spell_state.sug_count > 0)
                vio_puts("\x18\x19 sel  \x0d acc  ESC skip  Q quit");
            else
                vio_puts("ESC skip  Q quit");

            status_dirty = 0;
            return;
        }

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
    vio_gotoxy(50, VGA_ROWS - 1);
    vio_puts("10CPI ");

    /* Rich text indicators */
    vio_setattr(cur_fmt & FMT_BOLD
                ? VGA_ATTR(VGA_CYAN, VGA_WHITE)
                : VGA_ATTR(VGA_CYAN, VGA_DGRAY));
    vio_puts("B");
    vio_setattr(cur_fmt & FMT_ITALIC
                ? VGA_ATTR(VGA_CYAN, VGA_WHITE)
                : VGA_ATTR(VGA_CYAN, VGA_DGRAY));
    vio_puts("I");
    vio_setattr(cur_fmt & FMT_UNDERLINE
                ? VGA_ATTR(VGA_CYAN, VGA_WHITE)
                : VGA_ATTR(VGA_CYAN, VGA_DGRAY));
    vio_puts("U");

    vio_setattr(VGA_ATTR(VGA_CYAN, VGA_BLACK));
    vio_puts(" ");
    vio_puts(ins_mode ? "INS" : "OVR");

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
    if (line_flags[lr] & LINE_FLAG_FOOTER)
        return VGA_ATTR(VGA_GREEN, VGA_WHITE);
    return VGA_ATTR_DEFAULT;
}

static void draw_content(void)
{
    int r;

    for (r = 0; r < EDIT_ROWS; r++) {
        int lr = top_row + r;
        int screen_row = r + EDIT_TOP_ROW;

        if (lr < 0 || lr >= nlines) {
            vio_gotoxy(0, screen_row);
            vio_setattr(VGA_ATTR_DEFAULT);
            vio_clreol();
            continue;
        }
        if (!line_dirty[lr])
            continue;

        {
            int c;
            int len = lines[lr].len;
            uint8_t base_attr = (uint8_t)line_attr(lr);
            int is_special = (line_flags[lr] != 0);

            if (len > VGA_COLS) len = VGA_COLS;

            {
                uint8_t *fplane = vgaterm_fplane(g_vt);
                uint8_t *uplane = vgaterm_uplane(g_vt);
                for (c = 0; c < len; c++) {
                    uint8_t f = is_special ? 0 : lines[lr].fmt[c];
                    uint8_t attr = base_attr;
                    uint8_t slot = 0;

                    if (!is_special) {
                        if (f & FMT_BOLD)
                            attr = (uint8_t)((attr & 0xF0) | ((attr | 0x08) & 0x0F));
                        if (f & FMT_ITALIC)
                            slot = 1;
                    }

                    vio_putch_at(c, screen_row,
                                 (uint8_t)lines[lr].buf[c],
                                 mark_cell_selected(lr, c)
                                     ? (uint8_t)((attr << 4) | (attr >> 4))
                                     : attr);
                    if (fplane)
                        fplane[screen_row * VGA_COLS + c] = slot;
                    if (uplane)
                        uplane[screen_row * VGA_COLS + c] =
                            (!is_special && (f & FMT_UNDERLINE)) ? 1 : 0;
                }
                /* clear rest of line */
                for (c = len; c < VGA_COLS; c++) {
                    vio_putch_at(c, screen_row, ' ', base_attr);
                    if (fplane)
                        fplane[screen_row * VGA_COLS + c] = 0;
                    if (uplane)
                        uplane[screen_row * VGA_COLS + c] = 0;
                }
            }
        }

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

    page_footer_active = 0;
    page_footer_len = 0;
    page_footer[0] = '\0';

    line_init(&lines[0]);

    for (i = 0; i < EDIT_ROWS; i++)
        line_dirty[i] = 1;
    memset(line_flags, 0, sizeof(line_flags));

    status_dirty  = 1;
    content_dirty = 1;
    ruler_dirty   = 1;
}

static void do_new(void)
{
    reset_editor();
    strcpy(fname, "[new]");
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
    /* stop at '|' (fmt separator) — count only the text portion */
    {   int j;
        for (j = 0; j < hex_len; j++) {
            if (hex[j] == '|') { hex_len = j; break; }
        }
    }
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
    /* zero fmt for all loaded chars — caller fills from hex if MGF5 */
    memset(l->fmt, 0, (size_t)len);
    return 1;
}

/* Parse the hex fmt section (after '|') into l->fmt.
 * l->len must already be set correctly by line_from_hex.      */
static int fmt_from_hex(Line *l, const char *hex)
{
    int i, hi, lo;
    /* skip to '|' */
    while (*hex && *hex != '|') hex++;
    if (*hex != '|') return 1;  /* no fmt section — leave zeros */
    hex++;
    for (i = 0; i < l->len; i++) {
        if (!hex[0] || !hex[1]) break;
        hi = hex_val((unsigned char)hex[0]);
        lo = hex_val((unsigned char)hex[1]);
        if (hi < 0 || lo < 0) break;
        l->fmt[i] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  PCL3 rich text export                                              */
/* ------------------------------------------------------------------ */

/* Emit the minimal PCL escape sequences needed to transition from
 * prev_fmt to next_fmt.  Only changed bits cause output.             */
static void pcl_emit_fmt(FILE *f, unsigned char prev, unsigned char next)
{
    unsigned char changed = prev ^ next;
    if (!changed) return;

    if (changed & FMT_BOLD)
        fputs(next & FMT_BOLD ? "\x1B(s3B" : "\x1B(s0B", f);
    if (changed & FMT_ITALIC)
        fputs(next & FMT_ITALIC ? "\x1B(s1S" : "\x1B(s0S", f);
    if (changed & FMT_UNDERLINE)
        fputs(next & FMT_UNDERLINE ? "\x1B&d0D" : "\x1B&d@", f);
}

static void save_line_expanded_pcl(FILE *f, Line *l, int row, int pages)
{
    int i;
    int page = row / page_len + 1;
    unsigned char cur = 0;

    for (i = 0; i < l->len; i++) {
        unsigned char f_attr = l->fmt[i];

        /* macro expansion — emit with current fmt state unchanged    */
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

        pcl_emit_fmt(f, cur, f_attr);
        cur = f_attr;
        fputc((unsigned char)l->buf[i], f);
    }

    /* reset all formatting at end of line                            */
    pcl_emit_fmt(f, cur, 0);
}

static int do_export_pcl(const char *path)
{
    FILE *f;
    int i, pages;

    f = fopen(path, "wb");
    if (!f) return -1;

    pages = total_pages();

    /* PCL3 job header ------------------------------------------------
     * Reset → portrait → letter → 6 LPI → 10 CPI (Courier)
     * Left margin at left_page col, text length = page_len lines     */
    fputs("\x1B" "E", f);                             /* reset             */
    fputs("\x1B&l0O", f);                          /* portrait          */
    fputs("\x1B&l2A", f);                          /* letter paper      */
    fprintf(f, "\x1B&l%dD", PCL_LPI);             /* lines per inch    */
    fprintf(f, "\x1B(s%dH", PCL_CPI);             /* chars per inch    */
    fprintf(f, "\x1B&a%dL", tabs.left_page);       /* left margin col   */
    fprintf(f, "\x1B&l%dF", page_len
            - (page_header_active ? 1 : 0)
            - (page_footer_active ? 1 : 0));       /* text length lines */

    for (i = 0; i < nlines; i++) {
        if (!(line_flags[i] & LINE_FLAG_PAGE_BREAK)) {
            save_line_expanded_pcl(f, &lines[i], i, pages);
            if (i < nlines - 1) fputc('\n', f);
        }
    }

    /* PCL3 job end */
    fputs("\x1B" "E", f);

    fclose(f);
    return 0;
}

static int do_export(const char *path)
{
    FILE *f;
    int i, pages;

    if (has_ext(path, ".pcl") || has_ext(path, ".prn"))
        return do_export_pcl(path);

    f = fopen(path, "wb");
    if (!f) return -1;

    pages = total_pages();
    for (i = 0; i < nlines; i++) {
        if (!(line_flags[i] & LINE_FLAG_PAGE_BREAK)) {
            save_line_expanded(f, &lines[i], i, pages);
            if (i < nlines - 1) fputc('\n', f);
        }
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

    fprintf(f, "MGF5\n");
    fprintf(f, "page %d\n", page_len);
    fprintf(f, "stops %d %d %d %d %d\n",
            tabs.left_page, tabs.right_page,
            tabs.left_tab, tabs.right_tab, tabs.tab_size);
    fprintf(f, "header %d ", page_header_active ? 1 : 0);
    file_put_hex(f, page_header, page_header_len);
    fputc('\n', f);
    fprintf(f, "footer %d ", page_footer_active ? 1 : 0);
    file_put_hex(f, page_footer, page_footer_len);
    fputc('\n', f);
    fprintf(f, "lines %d\n", nlines);
    for (i = 0; i < nlines; i++) {
        fprintf(f, "%u ", (unsigned int)line_flags[i]);
        file_put_hex(f, lines[i].buf, lines[i].len);
        fputc('|', f);
        file_put_hex(f, (const char *)lines[i].fmt, lines[i].len);
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

    if (path != fname) {
        strncpy(fname, path, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = '\0';
    }
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
    if (has_ext(path, ".pcl") || has_ext(path, ".prn"))
        return do_export(path);

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
    if (sscanf(buf, "page %d", &page_len) != 1 ||
        page_len < 2 || page_len > MAX_LINES)
        return -1;

    if (version >= 3) {
        int lp, rp, lt, rt, ts;
        if (!fgets(buf, (int)sizeof(buf), f))
            return -1;
        if (sscanf(buf, "stops %d %d %d %d %d", &lp, &rp, &lt, &rt, &ts) != 5)
            return -1;
        if (tab_values_valid(lp, lt, rt, rp) && ts > 0 && ts < VGA_COLS) {
            tabs.left_page  = lp;
            tabs.right_page = rp;
            tabs.left_tab   = lt;
            tabs.right_tab  = rt;
            tabs.tab_size   = ts;
            tabs.center_tab = tab_center(lt, rt);
        }
    }

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

    if (version >= 4) {
        Line tmp;
        if (!fgets(buf, (int)sizeof(buf), f))
            return -1;
        if (strncmp(buf, "footer ", 7) != 0)
            return -1;
        page_footer_active = buf[7] == '1';
        if (buf[8] != ' ')
            return -1;
        line_init(&tmp);
        if (!line_from_hex(&tmp, buf + 9)) {
            line_free(&tmp);
            return -1;
        }
        page_footer_len = tmp.len;
        if (page_footer_len > PAGE_HEADER_MAX)
            page_footer_len = PAGE_HEADER_MAX;
        if (page_footer_len > 0)
            memcpy(page_footer, tmp.buf, (size_t)page_footer_len);
        page_footer[page_footer_len] = '\0';
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
        int idx;
        if (!fgets(buf, (int)sizeof(buf), f))
            return -1;
        line_init(&lines[nlines]);
        idx = nlines++;
        line_flags[idx] = 0;
        if (version >= 2) {
            unsigned int flags = 0;
            while (*hex >= '0' && *hex <= '9') {
                flags = flags * 10 + (unsigned int)(*hex - '0');
                hex++;
            }
            if (*hex != ' ')
                return -1;
            hex++;
            line_flags[idx] = (unsigned char)flags;
        }
        if (!line_from_hex(&lines[idx], hex))
            return -1;
        if (version >= 5)
            fmt_from_hex(&lines[idx], hex);
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

    /* PCL/PRN are output-only formats — loading them is not supported */
    if (has_ext(path, ".pcl") || has_ext(path, ".prn")) {
        set_console_error("pcl/prn files cannot be loaded — use export");
        return -1;
    }

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
        } else if (strcmp(buf, "MGF3\n") == 0 || strcmp(buf, "MGF3\r\n") == 0) {
            project_version = 3;
        } else if (strcmp(buf, "MGF4\n") == 0 || strcmp(buf, "MGF4\r\n") == 0) {
            project_version = 4;
        } else if (strcmp(buf, "MGF5\n") == 0 || strcmp(buf, "MGF5\r\n") == 0) {
            project_version = 5;
        } else {
            fclose(f);
            return -1;
        }
    }

    reset_editor();
    if (project) {
        if (load_project(f, project_version) != 0) {
            fclose(f);
            reset_editor();
            return -1;
        }
    } else {
        line_free(&lines[0]);
        nlines = 0;

        while (fgets(buf, (int)sizeof(buf), f)) {
            len = (int)strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
            if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';

            line_init(&lines[nlines]);
            line_grow(&lines[nlines], len);
            memcpy(lines[nlines].buf, buf, (size_t)(len + 1));
            memset(lines[nlines].fmt, 0, (size_t)len);
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
    
    strncpy(fname, path, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';

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
    memmove(l->fmt + count, l->fmt, (size_t)l->len);
    for (i = 0; i < count; i++) {
        l->buf[i] = ' ';
        l->fmt[i] = 0;
    }
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
    if (line_flags[cur_row] & LINE_FLAG_REPEAT)
        return; /* header already present at this boundary */
    if (nlines >= MAX_LINES)
        return;

    insert_line_copy(cur_row, page_header, page_header_len, LINE_FLAG_REPEAT);
    line_dirty[cur_row] = 1;
    cur_row++;
    line_dirty[cur_row] = 1;
    mark_visible_dirty();
}

static void reserve_page_footer_if_needed(void)
{
    if (!page_footer_active)
        return;
    if (page_len <= 1)
        return;
    if (cur_row <= 0 || cur_row % page_len != page_len - 1)
        return;
    if (line_flags[cur_row] & LINE_FLAG_FOOTER)
        return; /* footer already present at this boundary */
    if (nlines >= MAX_LINES)
        return;

    insert_line_copy(cur_row, page_footer, page_footer_len, LINE_FLAG_FOOTER);
    line_dirty[cur_row] = 1;
    cur_row++;
    line_dirty[cur_row] = 1;
    mark_visible_dirty();
}

static void remove_row(int row)
{
    int i;
    if (row < 0 || row >= nlines) return;
    line_free(&lines[row]);
    for (i = row; i < nlines - 1; i++) {
        lines[i]      = lines[i + 1];
        line_flags[i] = line_flags[i + 1];
        line_dirty[i] = 1;
    }
    nlines--;
    if (nlines < 1) {
        line_init(&lines[0]);
        nlines = 1;
    }
    mark_visible_dirty();
}

static void move_to_header(void)
{
    int page_start = (cur_row / page_len) * page_len;
    char tmp[PAGE_HEADER_MAX + 1];
    int  tmp_len;

    if (cur_row == page_start) {
        line_flags[cur_row] =
            (unsigned char)((line_flags[cur_row] & (unsigned char)~LINE_FLAG_FOOTER) |
                            LINE_FLAG_REPEAT);
        line_dirty[cur_row] = 1;
        modified = 1;
        status_dirty = 1;
        return;
    }

    tmp_len = lines[cur_row].len;
    if (tmp_len > PAGE_HEADER_MAX) tmp_len = PAGE_HEADER_MAX;
    if (tmp_len > 0) memcpy(tmp, lines[cur_row].buf, (size_t)tmp_len);
    tmp[tmp_len] = '\0';

    remove_row(cur_row);
    /* page_start <= original cur_row so index is unchanged after removal */
    insert_line_copy(page_start, tmp, tmp_len, LINE_FLAG_REPEAT);
    cur_row = page_start;
    cur_col = tabs.left_tab;
    ensure_visible();
    mark_visible_dirty();
    modified = 1;
    status_dirty = 1;
}

static void move_to_footer(void)
{
    int page_end = (cur_row / page_len + 1) * page_len - 1;
    char tmp[PAGE_HEADER_MAX + 1];
    int  tmp_len;

    if (page_end >= nlines) page_end = nlines - 1;

    if (cur_row == page_end) {
        line_flags[cur_row] =
            (unsigned char)((line_flags[cur_row] & (unsigned char)~LINE_FLAG_REPEAT) |
                            LINE_FLAG_FOOTER);
        line_dirty[cur_row] = 1;
        modified = 1;
        status_dirty = 1;
        return;
    }

    tmp_len = lines[cur_row].len;
    if (tmp_len > PAGE_HEADER_MAX) tmp_len = PAGE_HEADER_MAX;
    if (tmp_len > 0) memcpy(tmp, lines[cur_row].buf, (size_t)tmp_len);
    tmp[tmp_len] = '\0';

    remove_row(cur_row);
    /* If page_end was below cur_row it's now shifted down by one */
    if (page_end > cur_row) page_end--;

    insert_line_copy(page_end + 1, tmp, tmp_len, LINE_FLAG_FOOTER);
    cur_row = page_end + 1;
    if (cur_row >= nlines) cur_row = nlines - 1;
    cur_col = tabs.left_tab;
    ensure_visible();
    mark_visible_dirty();
    modified = 1;
    status_dirty = 1;
}

static void repeat_current_row(void)
{
    Line *l = &lines[cur_row];
    int len  = l->len;
    int is_header = line_flags[cur_row] & LINE_FLAG_REPEAT;
    int is_footer = line_flags[cur_row] & LINE_FLAG_FOOTER;

    if (!is_header && !is_footer)
        return; /* Ctrl+R only valid on a header or footer line */

    if (len > PAGE_HEADER_MAX) len = PAGE_HEADER_MAX;

    if (is_header) {
        page_header_len = len;
        if (len > 0) memcpy(page_header, l->buf, (size_t)len);
        page_header[len] = '\0';
        page_header_active = 1;
    } else {
        page_footer_len = len;
        if (len > 0) memcpy(page_footer, l->buf, (size_t)len);
        page_footer[len] = '\0';
        page_footer_active = 1;
    }

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

    if (nlines > MAX_LINES - 2)
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

    if (row >= nlines)
        row = nlines - 1;
    cur_row = row;
    reserve_page_footer_if_needed();
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
    reserve_page_footer_if_needed();
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
    undo_push_ins_char();
    if (ins_mode) {
        line_ins(l, cur_col, (char)ch, (unsigned char)cur_fmt);
    } else {
        if (cur_col < l->len) {
            l->buf[cur_col] = (char)ch;
            l->fmt[cur_col] = (unsigned char)cur_fmt;
        } else {
            line_ins(l, cur_col, (char)ch, (unsigned char)cur_fmt);
        }
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
        reserve_page_footer_if_needed();
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
    spell_state.active = 0;
    console_state.pending_spell_suggest = 0;
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

static void console_enter_with(const char *input)
{
    console_enter();
    strncpy(console_state.input, input, sizeof(console_state.input) - 1);
    console_state.input[sizeof(console_state.input) - 1] = '\0';
    console_state.input_len = (int)strlen(console_state.input);
}

static int parse_uint(const char *s, int *out)
{
    int n = 0;

    if (!*s) return 0;
    while (*s) {
        int digit;
        if (*s < '0' || *s > '9') return 0;
        digit = *s - '0';
        if (n > (INT_MAX - digit) / 10)
            return 0;
        n = n * 10 + digit;
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

    if (!parse_uint(arg, &n) || n < 2 || n > MAX_LINES) {
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

static void command_scale(const char *arg)
{
    int n = 0;

    if (!parse_uint(arg, &n) || (n != 1 && n != 2 && n != 4)) {
        set_console_error("scale: use 1, 2, or 4");
        return;
    }
    if (vgaterm_setup_scaling(g_vt, n) != 0) {
        set_console_error("scale: resize failed");
        return;
    }
    vga_scale = n;
    mark_visible_dirty();
    ruler_dirty  = 1;
    status_dirty = 1;
}

/* ------------------------------------------------------------------ */
/*  Find / Replace                                                     */
/* ------------------------------------------------------------------ */

#define FIND_MAX VGA_COLS

static char last_find[FIND_MAX + 1] = "";

static int find_is_word_char(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

static int find_term_is_word(const char *term)
{
    while (*term) {
        if (!find_is_word_char((unsigned char)*term))
            return 0;
        term++;
    }
    return 1;
}

static int find_matches_at(int row, int col, const char *term, int tlen,
                           int whole_word)
{
    if (memcmp(lines[row].buf + col, term, (size_t)tlen) != 0)
        return 0;

    if (whole_word) {
        if (col > 0 &&
            find_is_word_char((unsigned char)lines[row].buf[col - 1]))
            return 0;
        if (col + tlen < lines[row].len &&
            find_is_word_char((unsigned char)lines[row].buf[col + tlen]))
            return 0;
    }

    return 1;
}

/* Search for term from (start_row, start_col).
 * Wraps once if needed. Returns 1 on match, 0 if not found.          */
static int find_from_mode(const char *term, int start_row, int start_col,
                          int whole_word, int *out_row, int *out_col)
{
    int tlen = (int)strlen(term);
    int pass, r, c, llen;

    if (tlen == 0) return 0;

    for (pass = 0; pass < 2; pass++) {
        int r_start = (pass == 0) ? start_row : 0;
        int r_end   = (pass == 0) ? nlines    : start_row + 1;

        for (r = r_start; r < r_end && r < nlines; r++) {
            llen = lines[r].len;
            c = (r == r_start && pass == 0) ? start_col : 0;
            for (; c <= llen - tlen; c++) {
                if (find_matches_at(r, c, term, tlen, whole_word)) {
                    *out_row = r;
                    *out_col = c;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int find_from(const char *term, int start_row, int start_col,
                     int *out_row, int *out_col)
{
    return find_from_mode(term, start_row, start_col, find_term_is_word(term),
                          out_row, out_col);
}

static void do_find(const char *term)
{
    int found_row, found_col;

    if (term && *term) {
        strncpy(last_find, term, FIND_MAX);
        last_find[FIND_MAX] = '\0';
    }

    if (!last_find[0]) {
        set_console_error("nothing to find");
        return;
    }

    /* search from one past cursor so repeated find advances           */
    if (find_from(last_find, cur_row, cur_col + 1, &found_row, &found_col)) {
        cur_row = found_row;
        cur_col = found_col;
        console_state.saved_row = cur_row;
        console_state.saved_col = cur_col;
        ensure_visible();
        mark_visible_dirty();
        content_dirty = 1;
        status_dirty  = 1;
    } else {
        set_console_error("not found");
    }
}

/* Replace chars at (row, col): delete old_len, insert replacement.
 * Updates cur_row/cur_col to point just past the replacement.        */
static void replace_at(int row, int col, int old_len, const char *repl)
{
    int i, rlen = (int)strlen(repl);

    /* delete old */
    for (i = 0; i < old_len; i++)
        line_del(&lines[row], col);

    /* insert replacement with no formatting */
    for (i = 0; i < rlen; i++)
        line_ins(&lines[row], col + i, repl[i], 0);

    line_dirty[row] = 1;
    cur_row = row;
    cur_col = col + rlen;
}

/* Parse "old/new" into old and new parts. Returns 1 on success.      */
static void trim_str(char *s)
{
    int len, start;

    /* ltrim */
    start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) {
        len = (int)strlen(s) - start;
        memmove(s, s + start, (size_t)(len + 1));
    }

    /* rtrim */
    len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
}

static int parse_replace_args(const char *arg,
                               char *old_buf, int old_max,
                               char *new_buf, int new_max)
{
    const char *sep = strchr(arg, '/');
    int olen, nlen;

    if (!sep) return 0;
    olen = (int)(sep - arg);
    nlen = (int)strlen(sep + 1);

    if (olen <= 0 || olen >= old_max || nlen >= new_max) return 0;

    memcpy(old_buf, arg, (size_t)olen);
    old_buf[olen] = '\0';
    memcpy(new_buf, sep + 1, (size_t)nlen);
    new_buf[nlen] = '\0';

    trim_str(old_buf);
    trim_str(new_buf);

    if (old_buf[0] == '\0') return 0;

    return 1;
}

static void do_replace(const char *arg)
{
    char old_buf[FIND_MAX + 1], new_buf[FIND_MAX + 1];
    int found_row, found_col;

    if (!parse_replace_args(arg, old_buf, sizeof(old_buf),
                                  new_buf, sizeof(new_buf))) {
        set_console_error("usage: replace old/new");
        return;
    }

    strncpy(last_find, old_buf, FIND_MAX);
    last_find[FIND_MAX] = '\0';

    if (!find_from(old_buf, cur_row, cur_col, &found_row, &found_col)) {
        set_console_error("not found");
        return;
    }

    replace_at(found_row, found_col, (int)strlen(old_buf), new_buf);
    console_state.saved_row = cur_row;
    console_state.saved_col = cur_col;
    modified = 1;
    content_dirty = 1;
    status_dirty  = 1;
    ensure_visible();
}

static void do_replace_all(const char *arg)
{
    char old_buf[FIND_MAX + 1], new_buf[FIND_MAX + 1];
    int found_row, found_col;
    int count = 0;
    int search_row = 0, search_col = 0;

    if (!parse_replace_args(arg, old_buf, sizeof(old_buf),
                                  new_buf, sizeof(new_buf))) {
        set_console_error("usage: replace all old/new");
        return;
    }

    strncpy(last_find, old_buf, FIND_MAX);
    last_find[FIND_MAX] = '\0';

    while (find_from(old_buf, search_row, search_col,
                     &found_row, &found_col)) {
        replace_at(found_row, found_col, (int)strlen(old_buf), new_buf);
        /* advance past replacement to avoid infinite loop             */
        search_row = found_row;
        search_col = found_col + (int)strlen(new_buf);
        if (search_col > lines[search_row].len) {
            search_row++;
            search_col = 0;
            if (search_row >= nlines) break;
        }
        count++;
        if (count > MAX_LINES * VGA_COLS) break; /* safety cap        */
    }

    if (count == 0) {
        set_console_error("not found");
    } else {
        console_state.saved_row = cur_row;
        console_state.saved_col = cur_col;
        modified = 1;
        mark_visible_dirty();
        status_dirty  = 1;
        content_dirty = 1;
        ensure_visible();
    }
}

/* ------------------------------------------------------------------ */
/*  Spell check                                                        */
/* ------------------------------------------------------------------ */

/* Run aspell list on the document text and populate spell_state.
 * Returns number of misspelled words found, or -1 on error.         */
static int spell_run(void)
{
    char tmp_path[] = "/tmp/mg_spell_XXXXXX";
    char cmd[sizeof(tmp_path) + 32];
    FILE *f;
    int fd, i, pages;
    char line[SPELL_WORD_LEN * 2];

    /* Write plain text to a safe temp file */
    fd = mkstemp(tmp_path);
    if (fd < 0) return -1;

    f = fdopen(fd, "w");
    if (!f) { close(fd); remove(tmp_path); return -1; }

    pages = total_pages();
    for (i = 0; i < nlines; i++) {
        if (!(line_flags[i] & LINE_FLAG_PAGE_BREAK)) {
            save_line_expanded(f, &lines[i], i, pages);
            fputc('\n', f);
        }
    }
    fclose(f);

    /* Build: aspell list < tmpfile */
    snprintf(cmd, sizeof(cmd), "aspell list < \"%s\" 2>/dev/null", tmp_path);

    f = popen(cmd, "r");
    if (!f) {
        remove(tmp_path);
        return -1;
    }

    spell_state.count = 0;
    while (fgets(line, sizeof(line), f) && spell_state.count < SPELL_MAX_WORDS) {
        int len = (int)strlen(line);
        /* strip trailing newline */
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0) continue;
        /* skip duplicates */
        for (i = 0; i < spell_state.count; i++) {
            if (strcmp(spell_state.words[i], line) == 0) break;
        }
        if (i == spell_state.count) {
            /* truncate to SPELL_WORD_LEN-1; anything longer is exotic */
            int copy_len = len < SPELL_WORD_LEN - 1 ? len : SPELL_WORD_LEN - 1;
            memcpy(spell_state.words[spell_state.count], line, (size_t)copy_len);
            spell_state.words[spell_state.count][copy_len] = '\0';
            spell_state.count++;
        }
    }

    pclose(f);
    remove(tmp_path);

    return spell_state.count;
}

/* Navigate to the current spell_state.idx word in the buffer.
 * Returns 1 if found and moved, 0 if the word isn't in the document.*/
static int spell_goto_current(void)
{
    int found_row, found_col;
    const char *word;

    if (spell_state.idx < 0 || spell_state.idx >= spell_state.count)
        return 0;

    word = spell_state.words[spell_state.idx];

    /* whole-word search from top each time — simple and correct      */
    if (!find_from_mode(word, 0, 0, 1, &found_row, &found_col))
        return 0;

    cur_row = found_row;
    cur_col = found_col;
    console_state.saved_row = cur_row;
    console_state.saved_col = cur_col;
    ensure_visible();
    mark_visible_dirty();
    content_dirty = 1;
    status_dirty  = 1;
    return 1;
}

/* Show current spell result in the status bar error area.            */
/* Fetch suggestions for word via "echo word | aspell -a".
 * Populates spell_state.sugs / sug_count / sug_idx.                 */
static void spell_fetch_suggestions(const char *word)
{
    char tmp_path[] = "/tmp/mg_sug_XXXXXX";
    char cmd[sizeof(tmp_path) + 32];
    char line[256];
    FILE *f;
    int  fd;

    spell_state.sug_count = 0;
    spell_state.sug_idx   = 0;

    /* Write word to tmpfile â avoids all shell quoting issues        */
    fd = mkstemp(tmp_path);
    if (fd < 0) return;
    f = fdopen(fd, "w");
    if (!f) { close(fd); remove(tmp_path); return; }
    fputs(word, f);
    fputc('\n', f);
    fclose(f);

    snprintf(cmd, sizeof(cmd), "aspell -a < \"%s\" 2>/dev/null", tmp_path);
    f = popen(cmd, "r");
    if (!f) { remove(tmp_path); return; }

    /* First line is the aspell version banner — discard it           */
    if (!fgets(line, sizeof(line), f)) { pclose(f); remove(tmp_path); return; }

    /* Second line is the result for our word                         */
    if (!fgets(line, sizeof(line), f)) { pclose(f); remove(tmp_path); return; }

    pclose(f);
    remove(tmp_path);

    /* '&' = misspelled with suggestions: & word count offset: s1, s2 */
    if (line[0] == '&') {
        char *colon = strchr(line, ':');
        char *p;
        if (!colon) return;
        p = colon + 2; /* skip ": " */
        while (*p && spell_state.sug_count < SPELL_MAX_SUGS) {
            char *comma = strchr(p, ',');
            int   len;
            if (comma) {
                len = (int)(comma - p);
            } else {
                len = (int)strlen(p);
                /* strip trailing newline */
                while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r'))
                    len--;
            }
            if (len > 0) {
                int copy = len < SPELL_WORD_LEN - 1 ? len : SPELL_WORD_LEN - 1;
                memcpy(spell_state.sugs[spell_state.sug_count], p, (size_t)copy);
                spell_state.sugs[spell_state.sug_count][copy] = '\0';
                spell_state.sug_count++;
            }
            if (!comma) break;
            p = comma + 2; /* skip ", " */
        }
    }
    /* '#' = no suggestions — sug_count stays 0, handled in draw     */
}

/* Enter suggestion scroll mode for the current word.
 * Assumes spell_goto_current() has already placed the cursor.        */
static void spell_enter_suggest(void)
{
    spell_fetch_suggestions(spell_state.words[spell_state.idx]);
    console_state.pending_spell_suggest = 1;
    console_state.has_error = 0;
    status_dirty = 1;
}

/* Advance to the next word in the list.
 * Returns 1 if there is a next word, 0 if the list is exhausted.    */
static int spell_advance(void)
{
    spell_state.idx++;
    if (spell_state.idx >= spell_state.count) {
        spell_state.active = 0;
        console_state.pending_spell_suggest = 0;
        set_console_error("spell: done");
        return 0;
    }
    if (spell_goto_current()) {
        spell_enter_suggest();
    } else {
        /* Word was edited away; show status, let user keep navigating */
        spell_enter_suggest();
    }
    return 1;
}

/* Handle keys while in suggestion scroll mode.                       */
static void handle_spell_suggest_key(int ch)
{
    if (ch == 'q' || ch == 'Q') {
        /* Hard quit — leave console entirely                         */
        spell_state.active = 0;
        console_state.pending_spell_suggest = 0;
        console_exit();
        return;
    }

    if (ch == KEY_ESC) {
        /* Skip this word, advance to next                            */
        console_state.pending_spell_suggest = 0;
        spell_advance();
        return;
    }

    if (ch == KEY_ENTER) {
        /* Accept selected suggestion — replace word in buffer        */
        if (spell_state.sug_count > 0) {
            const char *word = spell_state.words[spell_state.idx];
            const char *sug  = spell_state.sugs[spell_state.sug_idx];
            replace_at(cur_row, cur_col, (int)strlen(word), sug);
            modified      = 1;
            content_dirty = 1;
            status_dirty  = 1;
            ensure_visible();
        }
        console_state.pending_spell_suggest = 0;
        spell_advance();
        return;
    }

    if (ch == KEY_UP) {
        if (spell_state.sug_count > 0) {
            spell_state.sug_idx--;
            if (spell_state.sug_idx < 0)
                spell_state.sug_idx = spell_state.sug_count - 1;
            status_dirty = 1;
        }
        return;
    }

    if (ch == KEY_DOWN) {
        if (spell_state.sug_count > 0) {
            spell_state.sug_idx++;
            if (spell_state.sug_idx >= spell_state.sug_count)
                spell_state.sug_idx = 0;
            status_dirty = 1;
        }
        return;
    }
}

static void spell_show_status(void)
{
    char msg[80];

    if (!spell_state.active) return;

    if (spell_state.count == 0) {
        set_console_error("spell: no misspellings found");
        return;
    }

    snprintf(msg, sizeof(msg), "spell %d/%d: \"%s\"  [n]ext [p]rev [q]uit",
             spell_state.idx + 1, spell_state.count,
             spell_state.words[spell_state.idx]);
    set_console_error(msg);
}

/* Console command: spell — run aspell and go to first hit.           */
static void command_spell(void)
{
    int n;

    n = spell_run();
    if (n < 0) {
        set_console_error("spell: aspell not found or failed");
        spell_state.active = 0;
        return;
    }

    spell_state.active = 1;
    spell_state.idx    = 0;

    if (n == 0) {
        set_console_error("spell: no misspellings found");
        return;
    }

    if (spell_goto_current())
        spell_enter_suggest();
    else
        set_console_error("spell: word not locatable in buffer");
}

/* Console commands: spell n / spell p / spell q (fallback nav)       */
static void command_spell_nav(const char *arg)
{
    if (!spell_state.active || spell_state.count == 0) {
        set_console_error("spell: run spell first");
        return;
    }

    if (strcmp(arg, "n") == 0 || strcmp(arg, "next") == 0) {
        spell_state.idx++;
        if (spell_state.idx >= spell_state.count) spell_state.idx = 0;
    } else if (strcmp(arg, "p") == 0 || strcmp(arg, "prev") == 0) {
        spell_state.idx--;
        if (spell_state.idx < 0) spell_state.idx = spell_state.count - 1;
    } else if (strcmp(arg, "q") == 0 || strcmp(arg, "quit") == 0) {
        spell_state.active = 0;
        set_console_error("spell: done");
        return;
    } else {
        set_console_error("spell: n, p, or q");
        return;
    }

    if (spell_goto_current())
        spell_enter_suggest();
    else
        spell_show_status();
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
    } else if (strcmp(arg, "header") == 0) {
        move_to_header();
        console_state.saved_row = cur_row;
        console_state.saved_col = cur_col;
    } else if (strcmp(arg, "footer") == 0) {
        move_to_footer();
        console_state.saved_row = cur_row;
        console_state.saved_col = cur_col;
    } else if (strcmp(arg, "repeat") == 0) {
        repeat_current_row();
    } else if (strcmp(arg, "break") == 0) {
        insert_page_break();
        console_state.saved_row = cur_row;
        console_state.saved_col = cur_col;
    } else if (strcmp(arg, "save") == 0) {
        if (do_save() < 0 && !console_state.has_error)
            set_console_error("save failed");
    } else if (strcmp(arg, "pb") == 0) {
        insert_page_break();
        console_state.saved_row = cur_row;
        console_state.saved_col = cur_col;
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
    } else if (strncmp(arg, "find ", 5) == 0) {
        do_find(arg + 5);
    } else if (strcmp(arg, "find") == 0 || strcmp(arg, "f") == 0) {
        do_find(NULL);
    } else if (strncmp(arg, "replace all ", 12) == 0) {
        do_replace_all(arg + 12);
    } else if (strncmp(arg, "replace ", 8) == 0) {
        do_replace(arg + 8);
    } else if (strcmp(arg, "quit") == 0) {
        running = 0;
    } else if (strncmp(arg, "scale ", 6) == 0) {
        command_scale(arg + 6);
    } else if (strcmp(arg, "spell") == 0) {
        command_spell();
    } else if (strncmp(arg, "spell ", 6) == 0) {
        command_spell_nav(arg + 6);
    } else if (arg[0] == '\0') {
        console_exit();
    } else {
        set_console_error("unknown command");
    }

    if (!console_state.has_error && running && !spell_state.active)
        console_exit();
}

static void handle_console_key(int ch)
{
    if (console_state.pending_spell_suggest) {
        handle_spell_suggest_key(ch);
        return;
    }

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
        if (mark_state.mode != MARK_NONE) {
            mark_clear();
        } else {
            console_enter();
        }
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

    case KEY_CTRL('t'):
        move_to_header();
        break;

    case KEY_CTRL('e'):
        move_to_footer();
        break;

    case KEY_CTRL('f'):
        console_enter_with("find ");
        break;

    case KEY_CTRL('r'):
        repeat_current_row();
        break;

    case KEY_CTRL('z'):
        do_undo();
        break;

    case KEY_CTRL('g'):
        do_find(NULL);
        break;

    case KEY_CTRL('b'):
        cur_fmt ^= FMT_BOLD;
        status_dirty = 1;
        break;

    case KEY_CTRL('l'):
        cur_fmt ^= FMT_ITALIC;
        status_dirty = 1;
        break;

    case KEY_CTRL('u'):
        cur_fmt ^= FMT_UNDERLINE;
        status_dirty = 1;
        break;

    /* Movement ------------------------------------------------------ */
    case KEY_UP:
        if (mark_state.mode == MARK_DEFINED) mark_clear();
        if (cur_row > 0) {
            cur_row--;
            /* If we landed in page-break padding, snap up to the marker */
            if (lines[cur_row].len == 0 && line_flags[cur_row] == 0) {
                int r = cur_row;
                while (r > 0 && lines[r].len == 0 && line_flags[r] == 0)
                    r--;
                if (line_flags[r] & LINE_FLAG_PAGE_BREAK)
                    cur_row = r;
            }
            clamp_col();
            ensure_visible();
            status_dirty = 1;
        }
        break;

    case KEY_DOWN:
        if (mark_state.mode == MARK_DEFINED) mark_clear();
        if (cur_row < nlines - 1) {
            int was_break = line_flags[cur_row] & LINE_FLAG_PAGE_BREAK;
            cur_row++;
            /* If we left a break marker, skip over the blank padding */
            if (was_break) {
                while (cur_row < nlines - 1 &&
                       lines[cur_row].len == 0 &&
                       line_flags[cur_row] == 0)
                    cur_row++;
            }
            clamp_col();
            ensure_visible();
            status_dirty = 1;
        }
        break;

    case KEY_LEFT:
        if (mark_state.mode == MARK_DEFINED) mark_clear();
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
        if (mark_state.mode == MARK_DEFINED) mark_clear();
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

    /* Mark ---------------------------------------------------------- */
    case KEY_CTRL('k'):
        if (mark_state.mode == MARK_NONE || mark_state.mode == MARK_DEFINED) {
            /* Drop a fresh anchor */
            mark_state.anchor_row = cur_row;
            mark_state.anchor_col = cur_col;
            mark_state.mode = MARK_CTRL_K;
            mark_visible_dirty();
        } else {
            /* Second Ctrl+K closes the region */
            mark_state.mode = MARK_DEFINED;
            mark_visible_dirty();
        }
        status_dirty = 1;
        break;

    case KEY_SHIFT_UP:
    case KEY_SHIFT_DOWN:
    case KEY_SHIFT_LEFT:
    case KEY_SHIFT_RIGHT:
        /* Start over from current cursor regardless of prior mark     */
        mark_state.anchor_row = cur_row;
        mark_state.anchor_col = cur_col;
        mark_state.mode = MARK_DEFINED;
        /* Move cursor one step */
        if (ch == KEY_SHIFT_UP && cur_row > 0) {
            cur_row--; clamp_col();
        } else if (ch == KEY_SHIFT_DOWN && cur_row < nlines - 1) {
            cur_row++; clamp_col();
        } else if (ch == KEY_SHIFT_LEFT) {
            if (cur_col > 0) cur_col--;
        } else if (ch == KEY_SHIFT_RIGHT) {
            if (cur_col < lines[cur_row].len) cur_col++;
        }
        ensure_visible();
        mark_visible_dirty();
        status_dirty = 1;
        break;

    case KEY_F1:
        if (mark_state.mode == MARK_DEFINED)
            command_kill_mark();
        break;

    case KEY_F4:
        command_undelete();
        break;

    case KEY_CTRL('y'):
        command_kill_line();
        break;

    case KEY_CTRL_BS:
        command_kill_word_backward();
        break;

    case KEY_CTRL_DEL:
        command_kill_word_forward();
        break;

    /* Newline -------------------------------------------------------- */
    case KEY_ENTER:
        if (nlines >= MAX_LINES) {
            set_console_error("maximum lines reached");
            break;
        }
        undo_push_split();
        split_line(cur_row, cur_col);
        line_dirty[cur_row] = 1;
        line_dirty[cur_row + 1] = 1;
        cur_row++;
        reserve_page_footer_if_needed();
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
        if (!ins_mode) {
            /* OVR: move left, blank that cell, stay there           */
            if (cur_col > 0) {
                cur_col--;
                if (cur_col < lines[cur_row].len) {
                    undo_push_ovr_blank(cur_row, cur_col);
                    lines[cur_row].buf[cur_col] = ' ';
                    lines[cur_row].fmt[cur_col] = 0;
                    line_dirty[cur_row] = 1;
                    modified      = 1;
                    content_dirty = 1;
                    status_dirty  = 1;
                }
            }
            /* at col 0 in OVR: stop, do not join lines             */
        } else {
            /* INS: collapse                                         */
            if (cur_col > 0) {
                undo_push_del_char(cur_row, cur_col - 1);
                cur_col--;
                line_del(&lines[cur_row], cur_col);
                line_dirty[cur_row] = 1;
                modified      = 1;
                content_dirty = 1;
                status_dirty  = 1;
            } else if (cur_row > 0) {
                int prev_len = lines[cur_row - 1].len;
                undo_push_join(cur_row - 1, prev_len);
                join_lines(cur_row - 1);
                line_dirty[cur_row - 1] = 1;
                mark_visible_dirty();
                modified = 1;
                cur_row--;
                cur_col       = prev_len;
                content_dirty = 1;
                status_dirty  = 1;
                ensure_visible();
            }
        }
        break;

    /* Delete --------------------------------------------------------- */
    case KEY_DEL:
        if (!ins_mode) {
            /* OVR: blank cell in place, cursor stays                */
            if (cur_col < lines[cur_row].len) {
                undo_push_ovr_blank(cur_row, cur_col);
                lines[cur_row].buf[cur_col] = ' ';
                lines[cur_row].fmt[cur_col] = 0;
                line_dirty[cur_row] = 1;
                modified      = 1;
                content_dirty = 1;
                status_dirty  = 1;
            }
            /* at end of line in OVR: stop, do not join lines       */
        } else {
            /* INS: collapse                                         */
            if (cur_col < lines[cur_row].len) {
                undo_push_del_char(cur_row, cur_col);
                line_del(&lines[cur_row], cur_col);
                line_dirty[cur_row] = 1;
                modified      = 1;
                content_dirty = 1;
                status_dirty  = 1;
            } else if (cur_row < nlines - 1) {
                undo_push_join(cur_row, lines[cur_row].len);
                join_lines(cur_row);
                line_dirty[cur_row] = 1;
                mark_visible_dirty();
                modified      = 1;
                content_dirty = 1;
                status_dirty  = 1;
            }
        }
        break;

    /* Printable ------------------------------------------------------ */
    default:
        if (ch >= 32 && ch <= 255) {
            if (mark_state.mode != MARK_NONE) mark_clear();
            insert_printable(ch);
        }
        break;
}
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int ch;
    VGATerm *vt = NULL;
    int i;
    int new_scale;
    int n_scale;
    const char *load_file;

    new_scale = 1;
    load_file = NULL;

    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--scale=", 8) == 0) {
            n_scale = 0;
            parse_uint(argv[i] + 8, &n_scale);
            if (n_scale == 1 || n_scale == 2 || n_scale == 4)
                new_scale = n_scale;
        } else if (load_file == NULL) {
            load_file = argv[i];
        }
    }

    vt = vgaterm_open("editor");
    if (!vt) return 1;
    if (new_scale != 1)
        vgaterm_setup_scaling(vt, new_scale);
    vga_scale = new_scale;
    g_vt = vt;
    vgaterm_set_font_slot(vt, 1, vga_font_italic_8x16);

    vio_init(vt);
    vio_setattr(VGA_ATTR_DEFAULT);
    vio_clrscr();

    tab_init();
    memset(&console_state, 0, sizeof(console_state));

    line_init(&lines[0]);
    cur_col = tabs.left_tab;
    if (load_file) {
        if (has_ext(load_file, ".pcl") || has_ext(load_file, ".prn")) {
            /* output-only format: set as save target, don't load */
            strncpy(fname, load_file, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
            status_dirty = 1;
        } else {
            do_load(load_file);
        }
    }

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
    vgaterm_close(vt);
    return 0;
}
