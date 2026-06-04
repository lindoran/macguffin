# Macguffin

A tiny, deterministic CP437 text editor for modern Linux.

Built on the **thin-vga** stack. No SDL, no ncurses, no complex abstractions. Just a raw 4000-byte VGA text buffer and direct hardware access.

Macguffin renders directly via Xlib using an authentic 8×16 VGA bitmap font.

## Why Macguffin?

A MacGuffin is a story element — generally an object (the Holy Grail, the Maltese Falcon, a glowing briefcase) that drives the story forward without becoming the story itself. The editor works the same way: it should not be what you think about while writing.

Every modern writing tool is built around a screen-flow model — text reflows to fit the viewport, and the notion of a physical page is an afterthought bolted on at export time. This produces documents that look fine on screen and uncertain on paper.

Macguffin works the other way around. The document is defined in terms of a physical page from the moment you start typing: a pitch, a margin, a column width. Like a 90s word processor. The ruler at the top of the screen shows exactly where the boundaries are. What you type is what goes to the printer, column for column, line for line.

Macguffin tries really hard to work like you write. You don't have to grab the mouse, you don't move your fingers from the keys — you type and Macguffin keeps splitting lines at your specified column width tab stop, and lets you justify text without taking over the formatting for the whole line. Macguffin just works, is minimal and lets you get to writing with the immediacy of a typewriter, but it's not — it's better.

## Architecture

This editor treats the screen as a flat memory buffer (`character` + `attribute` bytes), exactly like a real VGA card in mode 3.

- **Resolution:** 80×25 characters.
- **Colors:** 16-color CGA/VGA palette.
- **Font:** Genuine IBM VGA 8×16 bitmap. A matching italic variant is generated from the same bitmap by `mkitalic.py` — no external fonts required.
- **Efficiency:** The entire I/O layer is very small, stays out of the way and assures Macguffin won't bind up even on tiny hardware.

## Dependencies

- `libX11` development libraries.

```sh
sudo apt install libx11-dev     # Debian/Ubuntu
sudo pacman -S libx11           # Arch
```

`python3-pillow` is optional — only needed if you want to render `italic_preview.png` via `mkitalic.py --preview`.

## Build

```sh
make          # build ./editor
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


## Status Bar

```
 [new] *             Pg 1  Ln 1  Col 1  10 CPI  B I U INS
  ^filename          ^page ^line ^col           ^fmt ^mode
```

- `*` — unsaved changes
- `B I U` — Bold / Italic / Underline, bright when active, dim when not
- `INS` / `OVR` — insert or overwrite mode



## Rich Text

Macguffin supports three character-level formatting attributes: **Bold**, *Italic*, and Underline. These are stored per character and travel with the text through wrapping, joining, splitting, and undo.

| Attribute | Key    | Screen rendering            | PCL3 output          |
|-----------|--------|-----------------------------|----------------------|
| Bold      | Ctrl-B | Bright foreground intensity | Stroke weight +3     |
| Italic    | Ctrl-I | Alternate italic font slot  | Posture: italic      |
| Underline | Ctrl-U | Pixel underline on row 14   | PCL underline mode   |

All eight combinations (B × I × U) work simultaneously. The current active attributes are shown in the status bar:

```
... B I U INS
    ↑ ↑ ↑
    │ │ └── Underline active (bright) / inactive (dim)
    │ └──── Italic active / inactive
    └────── Bold active / inactive
```

When a region is selected, Ctrl-B / Ctrl-I / Ctrl-U apply the attribute to every character in the selection rather than toggling the insertion state. If any character in the region already has the attribute, it is cleared from all; otherwise it is set on all.

Formatting is stored in the `.mgf` project file alongside the text. Plain-text `.txt` export strips all formatting. PCL3 export emits the appropriate escape sequences so formatting prints correctly on HP DeskJet and compatible printers.

## Keybindings

| Key              | Action                                          |
|------------------|-------------------------------------------------|
| Ctrl-S           | Save (no-op if unnamed)                         |
| Ctrl-N           | New file                                        |
| Ctrl-Q           | Quit                                            |
| Ctrl-B           | Toggle **Bold**                                 |
| Ctrl-I           | Toggle *Italic*                                 |
| Ctrl-U           | Toggle Underline                                |
| Ctrl-J           | Justify word under cursor                       |
| Ctrl-T           | Move current line to page header slot           |
| Ctrl-E           | Move current line to page footer slot           |
| Ctrl-F           | Open console with `find ` typed                 |
| Ctrl-G           | Repeat previous find                            |
| Ctrl-R           | Arm current header/footer as repeating template |
| Ctrl-Z           | Undelete (restore last killed to its origin)    |
| Ctrl-K           | Drop mark anchor / close mark region            |
| Ctrl-Y           | Kill current line → undelete slot               |
| Ctrl-C           | Copy marked region to X11 clipboard             |
| Ctrl-X           | Cut marked region to X11 clipboard              |
| Ctrl-V           | Paste from X11 clipboard                        |
| Ctrl-Backspace   | Kill word backward → undelete slot              |
| Ctrl-Delete      | Kill word forward → undelete slot               |
| Esc              | Cancel mark / open console                      |
| Tab              | Insert current tab size spaces                  |
| Insert           | Toggle INS / OVR                                |
| Arrows           | Move cursor (clears defined mark)               |
| Shift+Arrows     | Select text — region highlights as you extend; releasing Shift closes the selection |
| Home / End       | Left tab / end of line                          |
| PgUp / PgDn      | Scroll a screenful                              |
| Enter            | Split line (kills mark if defined)              |
| Backspace        | INS: collapse left / OVR: move left, blank cell |
| Delete           | INS: collapse right / OVR: blank cell in place  |

Characters 32–255 are passed straight through as CP437 glyphs.

## Mark and Kill

Macguffin uses a **mark and kill** model for region operations. This fits the fixed-geometry editing philosophy: operations are intentional and permanent, with a single-slot undelete buffer for immediate recovery.

### Defining a region

There are two ways to define a region.

**Shift+arrow** is the fastest path. Hold Shift and press any arrow key — an anchor drops at the current position and the selection highlights immediately as you move. Each additional Shift+arrow extends the region. Releasing Shift closes it; the kill/copy prompt appears in the status bar and the region stays highlighted.

**Ctrl+K** is the keyboard-only path. Press **Ctrl+K** to drop an anchor. Navigate freely with plain arrows — the mark stays active and the selected region is shown inverted. Press **Ctrl+K** again to close the region and show the kill/copy prompt. **ESC** cancels at any point without killing.

The two methods are interchangeable: you can start with Shift+arrow and finish with Ctrl+K, or vice versa.

Plain arrows clear a closed selection and move normally. Typing a character also clears the mark.

### Clipboard (Ctrl-C, Ctrl-X, Ctrl-V)

Macguffin integrates with the standard X11 `CLIPBOARD` selection, so text flows naturally between the editor and other applications.

**Copy** (`Ctrl-C`) exports the marked region to the clipboard as UTF-8. CP437 extended characters (accented letters, box-drawing, symbols) are translated to their Unicode equivalents. The mark stays active after copying.

**Cut** (`Ctrl-X`) copies the region to the clipboard and then kills it, exactly as if you had pressed `Ctrl-C` followed by `F1`.

**Paste** (`Ctrl-V`) requests the current clipboard content from the system. The text arrives as UTF-8, is translated back to CP437, and is fed into the editor as if typed — so it obeys the current INS/OVR mode, tab stops, line wrapping, and all other editing rules. Rich text attributes (bold, italic, underline) are not carried through paste; pasted text arrives unstyled.

Both copy and paste work with any X11 application that speaks `UTF8_STRING` — terminals, browsers, office applications, and so on.

### INS and OVR mode affect all delete operations

The Insert/Overwrite toggle (Insert key) controls how every delete action — backspace, delete, word kill, and mark kill — behaves. This keeps the editing model consistent throughout.

**In INS mode** delete operations collapse: characters are removed and content shifts to fill the gap. Lines shorten or are joined. This is the conventional text-editor behaviour.

**In OVR mode** delete operations blank: characters are replaced with spaces and nothing shifts. The physical space on the page is preserved. Lines are never shortened or joined by a delete in OVR mode. This matches typewriter correction behaviour — like correction fluid that erases without disturbing the surrounding layout.

| Operation      | INS                                 | OVR                                |
|----------------|-------------------------------------|-------------------------------------|
| Backspace      | Collapse left; join lines at col 0  | Move left, blank cell; stop at col 0 |
| Delete         | Collapse right; join lines at end   | Blank cell in place; stop at end   |
| Ctrl+Backspace | Collapse word span                  | Blank word span with spaces        |
| Ctrl+Delete    | Collapse word span                  | Blank word span with spaces        |
| Ctrl+Y         | Truncate line to empty              | Blank all content, line stays      |
| F1 mark kill   | Collapse region, remove lines       | Blank region, no line removal      |

Every OVR blank is individually recorded in the undo ring (Ctrl+Z restores the original character in place). INS collapses use the existing character and join undo records.

### Kill operations

All kill operations (F1, Ctrl+Y, Ctrl+Backspace, Ctrl+Delete) store the killed content in the single undelete slot, replacing whatever was there previously. There is no kill ring.

| Key            | What is killed                          |
|----------------|-----------------------------------------|
| BS / Del       | Defined marked region (when mark active)|
| Ctrl+Y         | Current line content (line stays empty) |
| Ctrl+Backspace | Word before cursor                      |
| Ctrl+Delete    | Word after cursor                       |

### Undelete (Ctrl+Z)

**Ctrl+Z** restores the last killed content to its exact origin. The editor remembers where the kill happened and navigates back there before restoring — you can move the cursor freely between the kill and Ctrl+Z and it will still land in the right place.

Undelete is the inverse of the kill that produced it:

- If the kill was done in **INS mode**: Ctrl+Z re-inserts the content, shifting existing content open. Obeys tab stops and wraps identically to typing.
- If the kill was done in **OVR mode**: Ctrl+Z overwrites from the origin position with the original characters, restoring them in place without shifting anything.

The mode at kill time is remembered automatically — you do not need to be in the same mode when you undelete.

There is no character-level undo. Backspace and Delete are permanent for individual characters. For word and region recovery, use the kill operations and Ctrl+Z.

## Spell Check

Macguffin integrates with `aspell` as an external spell checker, run on demand from the console — not inline while typing. This fits naturally into a proofing-before-print workflow.

### Running a spell check

```
spell
```

Macguffin exports the document text to a temporary file, runs `aspell list`, deduplicates the results, and jumps to the first misspelled word. The status bar enters suggestion mode:

```
spell 3/12: "recieve" → receive   ↑↓ scroll  ⏎ accept  ESC skip  Q quit
```

| Key    | Action                                      |
|--------|---------------------------------------------|
| ↑ / ↓  | Scroll through aspell's suggestions         |
| Enter  | Accept selected suggestion and advance      |
| ESC    | Skip this word, advance to next             |
| Q      | Quit spell check entirely                   |

If aspell has no suggestions for a word, `(no suggestions)` is shown and only ESC and Q apply.

From the console, `spell n`, `spell p`, and `spell q` also navigate and quit without re-running the full check.

### Requirements

`aspell` must be installed:

```sh
sudo apt install aspell aspell-en     # Debian/Ubuntu
sudo pacman -S aspell aspell-en       # Arch
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
| `pb`, `break`   | Insert a page break at the cursor                   |
| `tab N`, `tabs N` | Set tab size and symmetric tab stops              |
| `stops N`       | Set page stops to `N` and `79 - N`                  |
| `page N`        | Set page length in lines                            |
| `header`        | Move current line to page header slot               |
| `footer`        | Move current line to page footer slot               |
| `repeat`        | Arm current header/footer line as repeating template |
| `find TEXT`     | Find `TEXT` from just after the cursor              |
| `find`, `f`     | Repeat the previous find                            |
| `replace OLD/NEW` | Replace the next `OLD` match with `NEW`           |
| `replace all OLD/NEW` | Replace every `OLD` match with `NEW`          |
| `scale N`       | Set pixel scaling: 1, 2, or 4                       |
| `spell`         | Run spell check, jump to first misspelling          |
| `spell n`       | Advance to next misspelled word                     |
| `spell p`       | Go back to previous misspelled word                 |
| `spell q`       | Quit spell check mode                               |

Find and replace wrap around the document. Word-like searches, such as
`find is` or `replace teh/the`, match whole words only, so `is` does not match
inside `this`. Searches containing spaces or punctuation match the exact typed
sequence.

Spaces around the `/` separator in replace commands are stripped automatically,
so `replace is / was` and `replace is/was` are equivalent.

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
- `Ctrl-E` physically moves the current line to the last position of its page. The line turns **green** to confirm it is in the footer slot.

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

MGF is a simple line-oriented text format. The current version is **MGF6**.

```
MGF6
page 66
stops 0 79 8 72 8
header 0 <hex>
footer 0 <hex>
lines <count>
<flags> <hex_text>|<hex_fmt>
...
```

Each line record encodes text and format attributes as paired hex strings separated by `|`. Since text content is hex-encoded, a literal `|` in the document becomes `7C` in the hex stream and is never ambiguous with the separator. Earlier versions (MGF1–MGF4) load cleanly; missing format data defaults to unstyled.

## Macguffin is a pencil.

Macguffin is a tool, like a pencil, as such it belongs in a toolbox, not in a box for sale. It is for you to make other things. You can support Macguffin if it's useful, and you are so inclined, by emailing me at z80dad (at) gmail (dot) com.

## License

MIT License

Copyright David Collins (c) 2026

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
