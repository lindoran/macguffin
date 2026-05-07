# editor

A tiny, deterministic CP437 text editor for modern Linux and vintage DOS.

Built on the **thin-vga** stack. No SDL, no ncurses, no complex abstractions. Just a raw 4000-byte VGA text buffer and direct hardware access.

- **Linux / X11** — Renders directly via Xlib using an authentic 8x16 VGA bitmap font.
- **DOS / ia16** — Runs in native 16-bit real mode. Writes directly to `$B800` with optimized cursor sync for zero-latency feedback and minimal flicker.

## Architecture

This editor treats the screen as a flat memory buffer (`character` + `attribute` bytes), exactly like a real VGA card in mode 3.

- **Resolution:** 80x25 characters.
- **Colors:** 16-color CGA/VGA palette.
- **Font:** Genuine IBM VGA 8x16 bitmap (built-in for Linux, native for DOS).
- **Efficiency:** The entire I/O layer is under 500 lines of code.

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

