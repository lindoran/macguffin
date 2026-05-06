# editor

A minimal CP437 text editor built on PDCursesMod. Targets two backends:

- **SDL2** — windowed, runs on Linux/Windows/macOS
- **DOS** — real-mode 16-bit x86, built with ia16-elf-gcc

## Dependencies

[PDCursesMod](https://github.com/Bill-Gray/PDCursesMod) is required for both
targets. Clone it alongside this directory:

```sh
git clone https://github.com/Bill-Gray/PDCursesMod
```

### SDL2 target

- SDL2 development libraries

```sh
sudo apt install libsdl2-dev     # Debian/Ubuntu
sudo pacman -S sdl2              # Arch
pacman -S mingw-w64-x86_64-SDL2  # MSYS2/MinGW
```

### DOS target

- [ia16-elf-gcc](https://github.com/tkchia/build-ia16) toolchain
- libi86

## Build

```sh
make          # SDL2 binary  -> ./editor
make dos      # DOS binary   -> ./editor.com
```

The Makefile builds the appropriate PDCursesMod port automatically on first
run. Pass a filename as the first argument to open a file on startup:

```sh
./editor myfile.txt
editor.com myfile.txt
```

> **Note on the DOS output format:** ia16-elf-gcc with `-mcmodel=small`
> produces an MZ `.exe` rather than a true flat `.com`. If the toolchain
> complains about the `.com` extension just rename the target in the Makefile.

## CP437 font (SDL2 only)

PDCursesMod's SDL2 port ships a built-in CP437 bitmap font. Override it with
the `PDC_FONT` environment variable:

```sh
PDC_FONT=/path/to/myfont.bmp ./editor
```

Ready-to-use fonts live in `PDCursesMod/sdl2/`. On the DOS target CP437 is
native to the hardware — no font file needed.

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

Characters 32–255 are passed straight through, so any CP437 glyph reachable
from your keyboard or input method will be inserted as-is.

## Status bar

```
 [new]                p.1   l.1    c.1    INS
  ^filename           ^page ^line  ^col   ^mode
```

## Roadmap

- [ ] Command bar for `save-as`, `open`, `quit-confirm`
- [ ] Undo / redo
- [ ] Search / replace
