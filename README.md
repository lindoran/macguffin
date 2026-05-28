# Macguffin

A tiny, deterministic CP437 text editor for modern Linux and vintage DOS.

Built on the **thin-vga** stack. No SDL, no ncurses, no complex abstractions. Just a raw 4000-byte VGA text buffer and direct hardware access.

- **Linux / X11** — Renders directly via Xlib using an authentic 8×16 VGA bitmap font.
- **DOS / ia16** — Runs in native 16-bit real mode. Writes directly to `$B800` with optimized cursor sync for zero-latency feedback and minimal flicker.

## Why Macguffin?

A MacGuffin is a story element — generally an object (the Holy Grail, the Maltese Falcon, a glowing briefcase) that drives the story forward without becoming the story itself. The editor works the same way: it should not be what you think about while writing.

Every modern writing tool is built around a screen-flow model — text reflows to fit the viewport, and the notion of a physical page is an afterthought bolted on at export time. This produces documents that look fine on screen and uncertain on paper.

Macguffin works the other way around. The document is defined in terms of a physical page from the moment you start typing: a pitch, a margin, a column width. Like a 90s word processor. The ruler at the top of the screen shows exactly where the boundaries are. What you type is what goes to the printer, column for column, line for line.

Macguffin tries really hard to work like you write. You don't have to grab the mouse, you don't move your fingers from the keys — you type and Macguffin keeps splitting lines at your specified column width tab stop, and lets you justify text without taking over the formatting for the whole line. Macguffin just works, is minimal and lets you get to writing with the immediacy of a typewriter, but it's not — it's better.

## Architecture

This editor treats the screen as a flat memory buffer (`character` + `attribute` bytes), exactly like a real VGA card in mode 3.

- **Resolution:** 80×25 characters.
- **Colors:** 16-color CGA/VGA palette.
- **Font:** Genuine IBM VGA 8×16 bitmap (built-in for Linux, native for DOS). A matching italic variant is generated from the same bitmap by `mkitalic.py` — no external fonts required.
- **Efficiency:** The entire I/O layer is very small, stays out of the way and assures Macguffin won't bind up even on tiny hardware.

## Dependencies

### Linux / X11

- `libX11` development libraries.

```sh
sudo apt install libx11-dev     # Debian/Ubuntu
sudo pacman -S libx11           # Arch
```

`python3-pillow` is optional — only needed if you want to render `italic_preview.png` via `mkitalic.py --preview`.

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

Scaling option:

```sh
./editor --scale=2   # 2× pixel scaling; use 4 for 4×
```

## Editing Model

Macguffin edits fixed 80-column lines. A newline is a hard line break, and the editor wraps by inserting real lines rather than maintaining a hidden flow model. The top screen row is a ruler:

```text
|---L----------------------------------^----------------------------------R---|
```

The markers are:

- `|` page stops
- `L` left tab / normal typing start
- `^` derived center tab
- `R` right tab / normal wrap target

The default page stops are columns `0` and `79`, with tab stops four columns inward. `tab 0` makes the tab stops equal to the page stops, which disables auto-indent.

## Keybindings

| Key        | Action                            |
|------------|-----------------------------------|
| Ctrl-S     | Save (no-op if unnamed)           |
| Ctrl-N     | New file                          |
| Ctrl-Q     | Quit                              |
| Ctrl-B     | Toggle **Bold**                   |
| Ctrl-L     | Toggle *Italic*                   |
| Ctrl-U     | Toggle Underline                  |
| Ctrl-J     | Justify word under cursor         |
| Ctrl-T     | Move current line to page header slot |
| Ctrl-F     | Move current line to page footer slot |
| Ctrl-R     | Arm current header/footer line as repeating template |
| Esc        | Open / close console              |
| Tab        | Insert current tab size spaces    |
| Insert     | Toggle INS / OVR                  |
| Arrows     | Move cursor                       |
| Home / End | Left tab / end of line            |
| PgUp/PgDn  | Scroll a screenful                |
| Enter      | Split line                        |
| Backspace  | Delete before cursor              |
| Delete     | Delete under cursor               |

Characters 32–255 are passed straight through as CP437 glyphs.

## Rich Text

Macguffin supports three character-level formatting attributes: **Bold**, *Italic*, and Underline. These are stored per character and travel with the text through wrapping, joining, splitting, and undo.

| Attribute | Key    | Screen rendering            | PCL3 output          |
|-----------|--------|-----------------------------|----------------------|
| Bold      | Ctrl-B | Bright foreground intensity | Stroke weight +3     |
| Italic    | Ctrl-L | Alternate italic font slot  | Posture: italic      |
| Underline | Ctrl-U | Pixel underline on row 14   | PCL underline mode   |

All eight combinations (B × I × U) work simultaneously. The current active attributes are shown in the status bar:

```
... B I U INS
    ↑ ↑ ↑
    │ │ └── Underline active (bright) / inactive (dim)
    │ └──── Italic active / inactive
    └────── Bold active / inactive
```

Formatting is stored in the `.mgf` project file alongside the text. Plain-text `.txt` export strips all formatting. PCL3 export emits the appropriate escape sequences so formatting prints correctly on HP DeskJet and compatible printers.

> **Note:** Ctrl-L is used for italic because Ctrl-I is ASCII 9 (Tab) and cannot be distinguished from it at this level.

## Italic Font

The italic character set is generated from `font_vga.h` by `mkitalic.py` using a pure algorithmic slant — no external fonts, no dependencies beyond the Python standard library.

The algorithm shifts each row of every glyph rightward by `(15 - r) >> 2` pixels:

```
rows  0– 3  →  shift 3 px   (top leans right)
rows  4– 7  →  shift 2 px
rows  8–11  →  shift 1 px
rows 12–15  →  shift 0 px   (bottom is the anchor)
```

Because it starts from the VGA font, the italic variant has identical stroke weight and pixel density to the normal font. Everything is consistent — same design language, same feel.

To regenerate `deps/thin-vga/font_italic.h`:

```sh
python3 mkitalic.py > deps/thin-vga/font_italic.h
```

To preview before regenerating (requires Pillow):

```sh
python3 mkitalic.py --preview   # writes italic_preview.png
```

## Console

Press `Esc` to open the bottom-row console. Press `Esc` again to close it, or press `Esc` once to clear an error and a second time to close.

Commands:

| Command         | Action                                              |
|-----------------|-----------------------------------------------------|
| `save`          | Save the current file                               |
| `save as PATH`  | Save using `PATH`'s extension                       |
| `load PATH`     | Load a text or `.mgf` file                          |
| `export PATH`   | Write print/plain-text output with page macros expanded |
| `quit`          | Quit                                                |
| `pb`            | Insert a page break at the cursor                   |
| `tab N`         | Set tab size and symmetric tab stops                |
| `stops N`       | Set page stops to `N` and `79 - N`                  |
| `page N`        | Set page length in lines                            |
| `scale N`       | Set pixel scaling: 1, 2, or 4                       |

## Justification

`Ctrl-J` operates on the word under the cursor. Repeated presses move the word through the active stops:

- before center: center on `^`
- at/after center: right-justify to `R`
- at `R`: right-justify to the right page stop and start the next line

Justification preserves text to the left of the justified word, clears only to the right of that word, and keeps `$p` / `$t` macros literal while editing.

## Page Headers and Footers

Macguffin supports a repeating header (first line of each page) and a repeating footer (last line of each page). The setup is a two-step process that keeps the repeat state deterministic — you always know exactly what line is acting as the template.

**Step 1 — move the line into position**

- `Ctrl-T` physically moves the current line to position 0 of its page. The line turns **red** to confirm it is in the header slot.
- `Ctrl-F` physically moves the current line to the last position of its page. The line turns **green** to confirm it is in the footer slot.

**Step 2 — arm repetition**

Navigate to the red or green line and press `Ctrl-R`. This captures the line's content as the repeating template and activates auto-insertion. `Ctrl-R` is a no-op on any other line, so it cannot be triggered accidentally.

Once armed, Macguffin inserts a fresh copy of the header at the top of each new page and a fresh copy of the footer at the bottom as you type past each page boundary.

Header and footer text may contain page macros:

- `$p` expands to the current page number on export
- `$t` expands to the total page count on export

## Page Breaks

Run `pb` in the console to insert an early page break. Macguffin inserts a blue `- break -` marker centered on the center tab, pads the document to the next page boundary, and keeps any repeating header and footer structure in place.

The marker is editor metadata. It is saved in `.mgf` but does not print or export as text.

## Files

Macguffin has two save modes:

- `.mgf` project files preserve full Macguffin state: document rows, per-character rich text attributes, page length, tab and page stops, and the repeating header and footer templates. Literal `$p` and `$t` are preserved.
- `.txt` files are plain-text export output. Page macros are expanded to visible numbers, formatting is stripped, and the file contains ordinary text rows only.

Use `save as name.mgf` for a project file and `save as name.txt` for plain text. Unknown extensions ask whether to save as an `.mgf` project. `Ctrl-S` on an unnamed file opens a console error prompting `type save as <filename>`.

### MGF Format

MGF is a simple line-oriented text format. The current version is **MGF5**.

```
MGF5
page 66
stops 0 79 8 72 8
header 0 <hex>
footer 0 <hex>
lines <count>
<flags> <hex_text>|<hex_fmt>
...
```

Each line record encodes text and format attributes as paired hex strings separated by `|`. Since text content is hex-encoded, a literal `|` in the document becomes `7C` in the hex stream and is never ambiguous with the separator. Earlier versions (MGF1–MGF4) load cleanly; missing format data defaults to unstyled.

## Status Bar

```
 [new] *             Pg 1  Ln 1  Col 1  10 CPI  B I U INS
  ^filename          ^page ^line ^col           ^fmt ^mode
```

- `*` — unsaved changes
- `B I U` — Bold / Italic / Underline, bright when active, dim when not
- `INS` / `OVR` — insert or overwrite mode

## Roadmap

- [x] Save-as / unnamed-file save flow
- [x] Explicit page breaks
- [x] Bold / Italic / Underline rich text
- [x] MGF5 per-character format storage
- [x] PCL3 rich text export (escape sequences on format transitions)
- [x] Undo (Ctrl-Z, 512 levels)
- [ ] Search / replace
