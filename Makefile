# ---------------------------------------------------------------
#  Makefile -- macguffin against thin-vga
# ---------------------------------------------------------------

VGADIR  = deps/thin-vga
CFLAGS  = -O2 -Wall -Wextra -std=c99 -pedantic -I$(VGADIR)
LDFLAGS = -lX11 -lm

SRCS = editor.c $(VGADIR)/vgaterm.c $(VGADIR)/vio.c
OBJS = editor.o vgaterm.o vio.o

.PHONY: all run clean

all: editor

editor: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

editor.o: editor.c $(VGADIR)/vgaterm.h $(VGADIR)/vio.h
	$(CC) $(CFLAGS) -c -o $@ $<

vgaterm.o: $(VGADIR)/vgaterm.c $(VGADIR)/vgaterm.h $(VGADIR)/font_vga.h
	$(CC) $(CFLAGS) -c -o $@ $<

# -DVIO_FREE_CTRL_KEYS frees Ctrl+I/M/[ from Tab/Return/Escape so
# applications can bind them independently (macguffin uses Ctrl+I for italic)
vio.o: $(VGADIR)/vio.c $(VGADIR)/vio.h $(VGADIR)/vgaterm.h
	$(CC) $(CFLAGS) -DVIO_FREE_CTRL_KEYS -c -o $@ $<

$(VGADIR)/font_vga.h:
	$(MAKE) -C $(VGADIR) font_vga.h

run: editor
	./editor

clean:
	rm -f editor *.o
