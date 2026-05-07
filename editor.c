/*
 * editor.c  --  minimal CP437 text editor via PDCursesMod/SDL2
 *
 * Deterministic, fixed-geometry 80x24 text mode.
 * Dirty-row rendering, no shadow buffer, no memcmp.
 * SDL2 window locked to fixed size via PDCursesMod.
 */

#include <curses.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Portability                                                        */
/* ------------------------------------------------------------------ */
#define CTRL(x)  ((x) & 0x1f)

#ifndef KEY_BACKSPACE
#  define KEY_BACKSPACE 0x107
#endif

/* ------------------------------------------------------------------ */
/*  Line buffer                                                        */
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

#define EDIT_ROWS (LINES - 1)

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
 
/* ------------------------------------------------------------------   
  Drawing                                                               
    we unrolled this because it was a stdio funciton that was basically 
    a tiny interperater that was firing nearly every frame with a status bar
    update (basically every frame with typing)                           
   ------------------------------------------------------------------   */
static void draw_status(void)
{
    char bar[80];
    int pos = 0;
    int page = cur_row / PCL_LPP + 1;
    int line = cur_row % PCL_LPP + 1;
    int col  = cur_col + 1;
    int i; /*for index*/

    /* fname padded/truncated to 20 chars */
    for (i = 0; i < 20; i++)
        bar[pos++] = (i < fname_len) ? fname[i] : ' ';

    bar[pos++] = ' ';

    /* p.xxx */
    bar[pos++] = 'p';
    bar[pos++] = '.';
    bar[pos++] = '0' + (page / 100) % 10;
    bar[pos++] = '0' + (page / 10)  % 10;
    bar[pos++] = '0' + (page % 10);
    bar[pos++] = ' ';

    /* l.xxxx */
    bar[pos++] = 'l';
    bar[pos++] = '.';
    bar[pos++] = '0' + (line / 1000) % 10;
    bar[pos++] = '0' + (line / 100)  % 10;
    bar[pos++] = '0' + (line / 10)   % 10;
    bar[pos++] = '0' + (line % 10);
    bar[pos++] = ' ';

    /* c.xxxx */
    bar[pos++] = 'c';
    bar[pos++] = '.';
    bar[pos++] = '0' + (col / 1000) % 10;
    bar[pos++] = '0' + (col / 100)  % 10;
    bar[pos++] = '0' + (col / 10)   % 10;
    bar[pos++] = '0' + (col % 10);
    bar[pos++] = ' ';

    /* 10cpi literal */
    bar[pos++] = '1';
    bar[pos++] = '0';
    bar[pos++] = 'c';
    bar[pos++] = 'p';
    bar[pos++] = 'i';
    bar[pos++] = ' ';

    /* INS / OVR */
    bar[pos++] = ins_mode ? 'I' : 'O';
    bar[pos++] = ins_mode ? 'N' : 'V';
    bar[pos++] = ins_mode ? 'S' : 'R';

    /* pad rest */
    while (pos < 80)
        bar[pos++] = ' ';

    move(LINES - 1, 0);
    attron(A_REVERSE);
    addnstr(bar, 80);
    attroff(A_REVERSE);

    status_dirty = 0;
}



static void draw_content(void)
{
    int r;
    char *txt;
    int len;

    for (r = 0; r < EDIT_ROWS; r++) {
        int lr = top_row + r;
        if (lr < 0 || lr >= nlines)
            continue;
        if (!line_dirty[lr])
            continue;

        txt = lines[lr].buf;
        len = lines[lr].len;
        if (len > COLS) len = COLS;

        move(r, 0);
        if (len > 0)
            addnstr(txt, len);

        if (len < COLS) {
            int pad = COLS - len;
            while (pad--) addch(' ');
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

    fname_len = 0;
    while (fname[fname_len] && fname_len < (int)sizeof(fname) - 1)
        fname_len++;


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
    switch (ch) {

    case CTRL('q'):
        running = 0;
        break;

    case CTRL('s'):
        do_save();
        break;

    case CTRL('n'):
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

    case KEY_PPAGE:
        cur_row -= EDIT_ROWS - 1;
        if (cur_row < 0) cur_row = 0;
        clamp_col();
        ensure_visible();
        status_dirty = 1;
        break;

    case KEY_NPAGE:
        cur_row += EDIT_ROWS - 1;
        if (cur_row >= nlines) cur_row = nlines - 1;
        clamp_col();
        ensure_visible();
        status_dirty = 1;
        break;

    /* Insert/overwrite ---------------------------------------------- */
    case KEY_IC:
        ins_mode = !ins_mode;
        status_dirty = 1;
        break;

    /* Newline -------------------------------------------------------- */
    case '\r':
    case '\n':
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

    /* Backspace ------------------------------------------------------ */
    case KEY_BACKSPACE:
    case 127:
    case 8:
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
    case KEY_DC:
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

    line_init(&lines[0]);
    if (argc > 1) do_load(argv[1]);

    initscr();

    /* Lock curses logical size */
    resize_term(24, 80);

    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    

#ifndef __ia16__
    /* SDL2 window lock via PDCursesMod */
    PDC_set_title("editor");
    mousemask(0, NULL);
    PDC_set_blink(0);
    PDC_set_resize_limits(24, 80, 24, 80); /*lock limits*/
#endif

    start_color();
    init_pair(1, COLOR_CYAN, COLOR_BLACK);
    bkgd(COLOR_PAIR(1));

    ensure_visible();

    while (running) {

        if (content_dirty)
            draw_content();

        if (status_dirty)
            draw_status();

        move(cur_row - top_row, cur_col);

        wnoutrefresh(stdscr);
        doupdate();

        ch = getch();
        handle_key(ch);

        /* Drain queued keys */
        nodelay(stdscr, TRUE);
        while (running && (ch = getch()) != ERR) {
            handle_key(ch);

            if (content_dirty)
                draw_content();
            if (status_dirty)
                draw_status();

            move(cur_row - top_row, cur_col);
            wnoutrefresh(stdscr);
            doupdate();
        }
        nodelay(stdscr, FALSE);
    }

    endwin();
    return 0;
}
