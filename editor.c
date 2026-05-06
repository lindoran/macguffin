/*
 * editor.c  --  minimal CP437 text editor via PDCursesMod/SDL2
 *
 * Keybindings:
 *   Ctrl-S   save (no-op if [new] -- future: command bar)
 *   Ctrl-N   new file
 *   Ctrl-Q   quit
 *   Insert   toggle INS/OVR
 *   Arrows, Home, End, PgUp, PgDn, Backspace, Delete, Enter
 *
 * Build: see Makefile
 */

#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Portability                                                         */
/* ------------------------------------------------------------------ */
#define CTRL(x)  ((x) & 0x1f)

#ifndef KEY_BACKSPACE
#  define KEY_BACKSPACE 0x107
#endif

/* ------------------------------------------------------------------ */
/*  Line buffer                                                         */
/* ------------------------------------------------------------------ */
#ifdef __ia16__
#  define MAX_LINES  2048
#  define LOAD_BUF   256
#else
#  define MAX_LINES  65536
#  define LOAD_BUF   4096
#endif
#define LINE_INIT  128

/* ------------------------------------------------------------------ */
/*  PCL3 page geometry                                                  */
/*  Standard US Letter at 6lpi / 10cpi                                 */
/* ------------------------------------------------------------------ */
#define PCL_LPI    6    /* lines per inch                              */
#define PCL_CPI    10   /* characters per inch                         */
#define PCL_LPP    66   /* lines per page  (11in * 6lpi)               */
#define PCL_CPL    80   /* characters per line (8in printable * 10cpi) */

typedef struct {
    char *buf;
    int   len;
    int   cap;
} Line;

static Line lines[MAX_LINES];
static int  nlines   = 1;

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
        l->buf  = (char *)realloc(l->buf, (size_t)l->cap);
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

/* Split line[row] at pos; new line inserted after row. */
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

/* Join line[row+1] onto line[row]. */
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
/*  Editor state                                                        */
/* ------------------------------------------------------------------ */
static int  cur_row  = 0;
static int  cur_col  = 0;
static int  top_row  = 0;   /* first visible row */
static int  ins_mode = 1;   /* 1=insert, 0=overwrite */
static int  modified = 0;
static int  running  = 1;
static char fname[512] = "[new]";

#define EDIT_ROWS  (LINES - 1)   /* last row is status bar */

/* ------------------------------------------------------------------ */
/*  Clamp cursor column to line length                                  */
/* ------------------------------------------------------------------ */
static void clamp_col(void)
{
    if (cur_col > lines[cur_row].len)
        cur_col = lines[cur_row].len;
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                             */
/* ------------------------------------------------------------------ */
static void draw_status(void)
{
    char bar[256];
    int  page = cur_row / PCL_LPP + 1;
    int  line = cur_row % PCL_LPP + 1;

    sprintf(bar, " %-20s p.%-3d l.%-4d c.%-4d %dcpi  %-3s",
            fname,
            page,
            line,
            cur_col + 1,
            PCL_CPI,
            ins_mode ? "INS" : "OVR");

    move(LINES - 1, 0);
    attron(A_REVERSE);
    clrtoeol();
    addstr(bar);
    attroff(A_REVERSE);
}

static void draw_screen(void)
{
    int r, c;

    /* scroll so cursor is visible */
    if (cur_row < top_row)
        top_row = cur_row;
    if (cur_row >= top_row + EDIT_ROWS)
        top_row = cur_row - EDIT_ROWS + 1;

    for (r = 0; r < EDIT_ROWS; r++) {
        int lr = r + top_row;
        move(r, 0);
        clrtoeol();
        if (lr < nlines) {
            for (c = 0; c < lines[lr].len && c < COLS; c++)
                addch((unsigned char)lines[lr].buf[c]);
        }
    }

    draw_status();
    move(cur_row - top_row, cur_col);
    refresh();
}

/* ------------------------------------------------------------------ */
/*  File I/O                                                            */
/* ------------------------------------------------------------------ */
static void reset_editor(void)
{
    int i;
    for (i = 0; i < nlines; i++) line_free(&lines[i]);
    nlines   = 1;
    cur_row  = cur_col = top_row = 0;
    modified = 0;
    line_init(&lines[0]);
}

static void do_new(void)
{
    reset_editor();
    strcpy(fname, "[new]");
}

static int do_save(void)
{
    FILE *f;
    int   i;

    if (strcmp(fname, "[new]") == 0)
        return 0;   /* need filename -- command bar (future) */

    f = fopen(fname, "wb");
    if (!f) return -1;

    for (i = 0; i < nlines; i++) {
        fwrite(lines[i].buf, 1, (size_t)lines[i].len, f);
        if (i < nlines - 1) fputc('\n', f);
    }
    fclose(f);
    modified = 0;
    return 0;
}

static void do_load(const char *path)
{
    FILE *f;
    char  buf[LOAD_BUF];
    int   len;

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
    if (nlines == 0) { line_init(&lines[0]); nlines = 1; }
    fclose(f);

    strncpy(fname, path, (int)sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Input handling                                                      */
/* ------------------------------------------------------------------ */
static void handle_key(int ch)
{
    switch (ch) {

    /* --- control keys --- */
    case CTRL('q'):
        running = 0;
        break;
    case CTRL('s'):
        do_save();
        break;
    case CTRL('n'):
        do_new();
        break;

    /* --- cursor movement --- */
    case KEY_UP:
        if (cur_row > 0) { cur_row--; clamp_col(); }
        break;
    case KEY_DOWN:
        if (cur_row < nlines - 1) { cur_row++; clamp_col(); }
        break;
    case KEY_LEFT:
        if (cur_col > 0)
            cur_col--;
        else if (cur_row > 0) {
            cur_row--;
            cur_col = lines[cur_row].len;
        }
        break;
    case KEY_RIGHT:
        if (cur_col < lines[cur_row].len)
            cur_col++;
        else if (cur_row < nlines - 1) {
            cur_row++;
            cur_col = 0;
        }
        break;
    case KEY_HOME:
        cur_col = 0;
        break;
    case KEY_END:
        cur_col = lines[cur_row].len;
        break;
    case KEY_PPAGE:
        cur_row -= EDIT_ROWS - 1;
        if (cur_row < 0) cur_row = 0;
        clamp_col();
        break;
    case KEY_NPAGE:
        cur_row += EDIT_ROWS - 1;
        if (cur_row >= nlines) cur_row = nlines - 1;
        clamp_col();
        break;

    /* --- insert/overwrite toggle --- */
    case KEY_IC:
        ins_mode = !ins_mode;
        break;

    /* --- newline --- */
    case '\r':
    case '\n':
    case KEY_ENTER:
        split_line(cur_row, cur_col);
        cur_row++;
        cur_col  = 0;
        modified = 1;
        break;

    /* --- backspace --- */
    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (cur_col > 0) {
            cur_col--;
            line_del(&lines[cur_row], cur_col);
            modified = 1;
        } else if (cur_row > 0) {
            int prev_len = lines[cur_row - 1].len;
            join_lines(cur_row - 1);
            cur_row--;
            cur_col  = prev_len;
            modified = 1;
        }
        break;

    /* --- delete --- */
    case KEY_DC:
        if (cur_col < lines[cur_row].len) {
            line_del(&lines[cur_row], cur_col);
            modified = 1;
        } else if (cur_row < nlines - 1) {
            join_lines(cur_row);
            modified = 1;
        }
        break;

    /* --- printable / CP437 --- */
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
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    int ch;

    line_init(&lines[0]);
    if (argc > 1) do_load(argv[1]);

    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

#ifndef __ia16__
    PDC_set_title("editor");
#endif

    /* Cyan on black -- matches the classic look in the screenshot */
    start_color();
    init_pair(1, COLOR_CYAN, COLOR_BLACK);
    bkgd(COLOR_PAIR(1));

    while (running) {
        draw_screen();
        ch = getch();
        handle_key(ch);
    }

    endwin();
    return 0;
}
