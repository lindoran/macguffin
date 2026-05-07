# ---------------------------------------------------------------
#  Makefile -- editor against thin-vga + ia16 DOS support
# ---------------------------------------------------------------

VGADIR  = deps/thin-vga
CFLAGS  = -O2 -Wall -Wextra -std=c99 -pedantic -I$(VGADIR)
LDFLAGS = -lX11 -lm

SRCS = editor.c $(VGADIR)/vgaterm.c $(VGADIR)/vio.c
OBJS = editor.o vgaterm.o vio.o

# --- DOS / ia16 ------------------------------------------------
CC_DOS     = ia16-elf-gcc
CFLAGS_DOS  = -O2 -Wall -Wextra -mcmodel=small -march=i8086 -li86

.PHONY: all dos run clean

all: editor

dos: editor.com

editor: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

editor.o: editor.c $(VGADIR)/vgaterm.h $(VGADIR)/vio.h
	$(CC) $(CFLAGS) -c -o $@ $<

vgaterm.o: $(VGADIR)/vgaterm.c $(VGADIR)/vgaterm.h $(VGADIR)/font_vga.h
	$(CC) $(CFLAGS) -c -o $@ $<

vio.o: $(VGADIR)/vio.c $(VGADIR)/vio.h $(VGADIR)/vgaterm.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(VGADIR)/font_vga.h:
	$(MAKE) -C $(VGADIR) font_vga.h

# --- DOS build -------------------------------------------------
editor.com: editor.c
	$(CC_DOS) -O2 -Wall -Wextra -mcmodel=small -march=i8086 -o $@ $< -li86

run: editor
	./editor

clean:
	rm -f editor editor.com *.o
