# Macguffin

A tiny, deterministic CP437 text editor for modern Linux and vintage DOS.

Built on the **thin-vga** stack. No SDL, no ncurses, no complex abstractions. Just a raw 4000-byte VGA text buffer and direct hardware access.

- **Linux / X11** — Renders directly via Xlib using an authentic 8x16 VGA bitmap font.
- **DOS / ia16** — Runs in native 16-bit real mode. Writes directly to `$B800` with optimized cursor sync for zero-latency feedback and minimal flicker.

## Why Macguffin?
A MacGuffin is a story element — generally an object (the Holy Grail, the Maltese Falcon, a glowing briefcase) that drives the story forward without becoming the story itself. The editor works the same way: it should not be what you think about while writing.

Every modern writing tool is built around a screen-flow model — text reflows to fit the viewport, and the notion of a physical page is an afterthought bolted on at export time. This produces documents that look fine on screen and uncertain on paper.

macguffin works the other way around. The document is defined in terms of a physical page from the moment you start typing: a pitch, a margin, a column width. Like a 90s word processor. The ruler at the top of the screen shows exactly where the boundaries are. What you type is what goes to the printer, column for column, line for line. The screen render is as close as can be to an ideal manuscript esthetic.

Macguffin tries really hard to work like you write.  You don't have to grab the mouse, you don't move your fingers from the keys - you type and macguffin keeps splitting lines at your specified column width tab stop, and lets you justify text without taking over the formatting for the whole line. Macguffin just works, is minimal and lets you get to writing with the immediacy of a typewriter, but it's not -- it's better.

## Architecture

This editor treats the screen as a flat memory buffer (`character` + `attribute` bytes), exactly like a real VGA card in mode 3.

- **Resolution:** 80x25 characters.
- **Colors:** 16-color CGA/VGA palette.
- **Font:** Genuine IBM VGA 8x16 bitmap (built-in for Linux, native for DOS).
- **Efficiency:** The entire I/O layer is very small, stays out of the way and assures macguffin won't bind up even on tiny hardware

## Dependencies

### Linux / X11

- `libX11` development libraries.

```sh
sudo apt install libx11-dev     # Debian/Ubuntu
sudo pacman -S libx11           # Arch
```

### DOS target

- [ia16-elf-gcc](https://github.com/tkchia/build-ia16) toolchain.
- `libi86` for DOS/BIOS interrupt support.

## Build

```sh
make          # Linux binary -> ./editor
make dos      # DOS binary   -> ./editor.com
```

Pass a filename as the first argument to open a file on startup:

```sh
./editor myfile.txt
```

## Keybindings

| Key        | Action                   |
|------------|--------------------------|
| Ctrl-S     | Save (no-op if unnamed)  |
| Ctrl-N     | New file                 |
| Ctrl-Q     | Quit                     |
| Insert     | Toggle INS / OVR         |
| Arrows     | Move cursor              |
| Home / End | Start / end of line      |
| PgUp/PgDn  | Scroll a screenful       |
| Enter      | Split line               |
| Backspace  | Delete before cursor     |
| Delete     | Delete under cursor      |

Characters 32–255 are passed straight through as CP437 glyphs.

## Status bar

```
 [new] *             Pg 1  Ln 1  Col 1  10 CPI  INSERT
  ^filename          ^page ^line ^col   ^fixed  ^mode
```
The `*` indicator appears when the file has unsaved changes.

## Roadmap

- [ ] Command bar for `save-as`, `open`, `quit-confirm`
- [ ] Undo / redo
- [ ] Search / replace

