# ---------------------------------------------------------------
#  Makefile -- editor against PDCursesMod
#
#  Targets:
#    make            -- SDL2 build (host gcc)
#    make dos        -- DOS build  (ia16-elf-gcc)
#    make clean      -- remove binaries
#    make distclean  -- also clean PDCursesMod build artifacts
#
#  Prerequisites:
#    git clone https://github.com/Bill-Gray/PDCursesMod
#    SDL2: libsdl2-dev (Linux) or SDL2.dll + headers (Windows)
#    DOS:  ia16-elf-gcc + libi86
# ---------------------------------------------------------------

PDCDIR  = deps/PDCursesMod
PDCINC  = -I$(PDCDIR)
CSTD    = -std=c89
WARN    = -Wall -Wextra -pedantic

# --- SDL2 (host) -----------------------------------------------
CC_SDL2     = gcc
PDCLIB_SDL2 = $(PDCDIR)/sdl2/libpdcurses.a

SDL2_CFLAGS != sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2 -D_REENTRANT
SDL2_LIBS   != sdl2-config --libs   2>/dev/null || echo -lSDL2

CFLAGS_SDL2  = $(CSTD) $(WARN) -O2 $(PDCINC) $(SDL2_CFLAGS)
LDFLAGS_SDL2 = $(PDCLIB_SDL2) $(SDL2_LIBS)

# --- DOS / ia16 ------------------------------------------------
CC_DOS     = ia16-elf-gcc
PDCLIB_DOS = $(PDCDIR)/dos/libpdcurses.a

CFLAGS_DOS  = $(CSTD) $(WARN) -O2 $(PDCINC) -mcmodel=small -march=i8086
LDFLAGS_DOS = $(PDCLIB_DOS) -li86

# ---------------------------------------------------------------

.PHONY: all dos run clean distclean

all: editor

dos: editor.com

# --- SDL2 build ------------------------------------------------
editor: editor.c $(PDCLIB_SDL2)
	$(CC_SDL2) $(CFLAGS_SDL2) -o $@ editor.c $(LDFLAGS_SDL2)

$(PDCLIB_SDL2):
	@if [ ! -d "$(PDCDIR)" ]; then \
	    echo "ERROR: $(PDCDIR) not found."; \
	    echo "  git clone https://github.com/Bill-Gray/PDCursesMod"; \
	    exit 1; \
	fi
	$(MAKE) -C $(PDCDIR)/sdl2

# --- DOS build -------------------------------------------------
editor.com: editor.c $(PDCLIB_DOS)
	$(CC_DOS) $(CFLAGS_DOS) -o $@ editor.c $(LDFLAGS_DOS)

$(PDCLIB_DOS):
	@if [ ! -d "$(PDCDIR)" ]; then \
	    echo "ERROR: $(PDCDIR) not found."; \
	    echo "  git clone https://github.com/Bill-Gray/PDCursesMod"; \
	    exit 1; \
	fi
	$(MAKE) -C $(PDCDIR)/dos CC=$(CC_DOS)

# ---------------------------------------------------------------
run: editor
	./editor

clean:
	rm -f editor editor.com

distclean: clean
	$(MAKE) -C $(PDCDIR)/sdl2 clean 2>/dev/null || true
	$(MAKE) -C $(PDCDIR)/dos  clean 2>/dev/null || true
