# Morse Code Trainer

| Target | ESP32-S3 |
| ------ | -------- |

ESP-IDF firmware for the **[Waveshare ESP32-S3-Touch-LCD-3.49](https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.49)**
(V2 hardware revision). A digital re-creation of the physical "Morse Code
Trainer Card": key dots and dashes on the BOOT button or the on-screen tap
zone (short press = dot, long = dash) and the letter the sequence forms
lights up on the panel. Two modes -- **Training**, a free-form keyer that
walks the dichotomic Morse tree and shows a live autocomplete of every
letter still reachable from the current prefix, and **Practice**, where
you key a target word and each letter is scored as you commit it. Runs
entirely offline -- no Wi-Fi, no app.

## Hardware

- Board: Waveshare ESP32-S3-Touch-LCD-3.49, **V2 hardware revision**
  (ESP32-S3R8, octal 8MB PSRAM, 16MB flash)
- Docs: https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.49 -- the docs
  page has no pinout table; the map below came from Waveshare's own
  ESP-IDF demo repo, `github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49`

| Function | Pin(s) |
|----------|--------|
| LCD (QSPI, AXS15231B) | Host=SPI3, CS=GPIO9, PCLK=GPIO10, DATA0-3=GPIO11/12/13/14 |
| LCD reset / backlight / speaker-amp enable | behind the TCA9554 I2C expander (EXIO5 / EXIO1 / EXIO7) |
| Shared "ESP I2C" (TCA9554 expander + ES8311 control) | SDA=GPIO47, SCL=GPIO48 |
| Touch (AXS15231B, on its own I2C bus) | SDA=GPIO17, SCL=GPIO18 |
| BOOT button (dot / dash / ~3s hold = mode switch) | GPIO0 |
| Audio I2S (ES8311 DAC) | MCLK=GPIO7, BCLK=GPIO15, WS=GPIO46, DOUT=GPIO45 |

Display/touch controller is **AXS15231B** (172x640 portrait, QSPI). Audio
is the onboard **ES8311** DAC, speaker-out only (the board's ES7210 mic
array is unused). There is **no onboard speaker** -- plug a small 8 ohm
speaker into the `MX1.25 2PIN` header on the back to hear the sidetone.

The panel and touch bring-up in
[`components/lcd_ui/lcd_ui.c`](components/lcd_ui/lcd_ui.c) is copied
verbatim from the sibling `../morse_code _listener` project
(`components/lcd_display/lcd_display.c`), where it was verified against
real V2 hardware. The audio pin map and ES8311 setup are in
[`components/sidetone/sidetone.c`](components/sidetone/sidetone.c).

## How it works

```
[BOOT button] --+                            +--> lcd_ui    LVGL scene on the panel
                +--> morse_key --callbacks-- +--> sidetone  gated tone via the ES8311 DAC
[tap zone] -----+    dot/dash + letter/      +--> word_list Practice target words
                    word timing FSM
                         |
                         +-- walks --> morse_tree --> decoded letter + autocomplete
```

- **`morse_key`** -- one FreeRTOS poll task (10 ms, 25 ms debounce) reads
  the BOOT button and the on-screen tap zone in parallel and classifies
  each press: a quick tap is a dot, `>= MORSE_KEY_LONG_MS` (250 ms) is a
  dash. It owns the `morse_tree` traversal -- each symbol advances the
  pointer, `MORSE_KEY_INTER_LETTER_MS` (900 ms) of silence commits the
  letter, a further `MORSE_KEY_WORD_MS` (2100 ms) emits a space -- so the
  tree has exactly one writer and needs no lock. Also handles the ~3s
  BOOT-hold mode switch and fires a key-down/up edge callback for the
  sidetone.
- **`morse_tree`** -- the dichotomic Morse tree and its traversal. Pure C,
  no IDF deps. `morse_tree_render_candidates()` builds the autocomplete
  list: every letter/digit still reachable from the current prefix,
  closest completions first. Host-tested.
- **`word_list`** -- the bundled Practice dictionary. Pure C, no IDF deps.
  `word_list_pick(len, rnd)` returns an uppercase A-Z word of length 1-6,
  seeded from the hardware RNG. Host-tested.
- **`lcd_ui`** -- AXS15231B QSPI panel + capacitive touch + the LVGL v8
  scene (Training and Practice widget sets, the HAKISTA theme).
  Cross-task paints are deferred: the setters stash state under a short
  mutex and a 25 ms `lv_timer` applies it from the LVGL task, so the
  keyer never takes LVGL's lock.
- **`sidetone`** -- a gated practice-oscillator tone through the onboard
  ES8311 DAC (`esp_codec_dev`): it sounds for exactly as long as the key
  is held, so you hear your own rhythm. `sidetone_cue()` plays the
  Practice success/fail jingle. Reuses `lcd_ui`'s shared I2C bus.
- **`main`** -- wires the keyer callbacks to `lcd_ui` and `sidetone` and
  owns the Training/Practice state machine and the per-letter scoring.

## Modes

Switch with the full-width violet **`MODE`** bar at the top of the panel,
or by **holding the BOOT button for ~3 s** from either screen (a "hold to
switch mode..." hint shows after ~0.8 s; release before 3 s to stay put --
the hold leaves no stray letter).

### Training (default)

Key dots and dashes; the pointer walks the Morse tree and the letter it
lands on lights up and is appended to a running output line, with an
autocomplete list of every letter/digit still reachable from the current
prefix. Tap the output strip to clear it.

### Practice

The screen shows a real word; key the whole word in Morse. Pick the word
length (1-6) with the on-screen number buttons. Each letter you commit is
scored **live against the expected position** -- **cyan** if right,
**red** if wrong, and the target advances regardless (no retry). When the
word is done the status line shows **`SUCCESS`** (every letter right) or
**`FAIL`**, with a matching rising / falling tone. The next dot/dash
starts a fresh word. The word list is bundled (`components/word_list/`),
letters `A`-`Z` only.

## Input

One short/long-press key, from either source at once:

| Source | short press | long press (>= 250 ms) |
|--------|-------------|------------------------|
| On-screen tap zone (bottom of the panel) | dot | dash |
| BOOT button (GPIO0) | dot | dash |

After ~900 ms of no input the current letter is committed; a further
~2.1 s gap emits a space. Holding **BOOT** alone for ~3 s switches mode
instead of keying. Thresholds are in
[`components/morse_key/morse_key.h`](components/morse_key/morse_key.h).

## Sound

A practice-oscillator **sidetone** plays through the onboard ES8311 DAC
while the key is held -- a real gated tone, not fixed-length blips, so you
can hear your own dot/dash rhythm. Practice mode also plays a short
**success / fail cue** (a rising or falling three-note arpeggio) when a
word is finished, queued on the same audio path so it never overlaps a
keyed tone.

Runtime API ([`components/sidetone/sidetone.h`](components/sidetone/sidetone.h)):
`sidetone_set_enabled()` mute/unmute, `sidetone_set_volume(0..100)`
(default 80), `sidetone_set_freq(hz)` (default 620),
`sidetone_cue(SIDETONE_CUE_SUCCESS / _FAIL)`. Audio bring-up failure is
non-fatal -- the trainer just runs silently.

## Screen (portrait 172 x 640)

```
   MODE: TRAINING    violet bar -- tap to switch (or hold BOOT 3 s)
o  --  o  o         big drawn dot/dash shapes for the sequence being keyed
   L               big decoded-letter glyph (flashes magenta on commit)
MORSE...           running committed output -- TAP IT to clear
------------------
6 possible         autocomplete (20px): every letter/digit still reachable,
> -.     N         closest completions first; '>' = commits right now
  -..    D
  -.-    K
  -.-.   C
------------------
   TAP KEY         short = dot   long = dash
```

In Practice mode the middle band becomes a `1 2 3 4 5 6` length picker,
the `TARGET` word (glyphs recolour cyan/red as you key), and a status
line that ends on `SUCCESS` or `FAIL`, in place of the autocomplete list
and output strip.

## Theme

The UI uses the **HAKISTA** brand palette
([`components/lcd_ui/lcd_ui.c`](components/lcd_ui/lcd_ui.c), the `BRAND_*`
/ `COLOR_*` defines):

| Brand colour | Where it shows |
|--------------|----------------|
| cyan `#07DBF9` | drawn dot/dash symbols, autocomplete list, selected length chip, "correct" glyph, key-zone press |
| white `#FFFFFF` | the big decoded letter, every button caption |
| grey `#BBB6C2` | running output text, `TARGET` caption, pending glyphs |
| red-orange `#FF4122` | "wrong" glyph in Practice |
| navy -> black `#0F1E35` -> `#000000` | screen background (vertical gradient), output strip, length chips |
| violet -> magenta `#5511C7` -> `#E531D6` | the `MODE` bar (violet at rest, magenta pressed) and the letter-commit flash |

The brand face is **Russo One**; this build only has Montserrat compiled
in (LVGL needs a converted font blob), so the type is unchanged -- adding
it is a separate step.

## Building & flashing

This project uses ESP-IDF v6.0.2. Activate the toolchain, then use
`idf.py` as normal:

```sh
. ~/.espressif/v6.0.2/esp-idf/export.sh   # or wherever your ESP-IDF install lives
idf.py set-target esp32s3                 # first time only
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

Find your port with `ls /dev/tty.usb*` while the board is plugged in. (To
exit the serial monitor, press `Ctrl-]`.)

Managed components (`esp_lcd_axs15231b`, `esp_lvgl_port`, `lvgl` v8,
`esp_io_expander_tca9554`, `esp_codec_dev`) are pulled from the ESP
Component Registry on the first build.

## Tests

The pure-C components have host unit tests -- no board or ESP-IDF needed,
just a C compiler:

```sh
components/morse_tree/test/run.sh
components/word_list/test/run.sh
```

`morse_tree`'s covers the traversal and the autocomplete ordering;
`word_list`'s checks every length bucket is non-empty and every word
matches its bucket length and is `A`-`Z`.

## Project layout

Each piece besides the app entry point lives in its own ESP-IDF component
under `components/`, with its own `CMakeLists.txt` declaring exactly what
it requires:

```
main/
  morse_code_trainer_main.c  -- app_main: brings up the tree/panel/keyer/sidetone,
                                  owns the Training/Practice state machine + scoring
  CMakeLists.txt
components/
  morse_tree/                -- dichotomic Morse tree + traversal + autocomplete.
    morse_tree.c/.h              Pure C, no IDF deps
    test/                    -- host unit test (./run.sh, needs only cc)
    CMakeLists.txt
  word_list/                 -- bundled length-bucketed word list for Practice mode.
    word_list.c/.h              Pure C, no IDF deps
    test/                    -- host unit test (./run.sh, needs only cc)
    CMakeLists.txt
  morse_key/                 -- BOOT button + tap-zone short/long-press classifier,
    morse_key.c/.h              letter/word timing FSM, ~3s BOOT-hold mode toggle
    CMakeLists.txt
  lcd_ui/                    -- AXS15231B QSPI panel + capacitive touch + LVGL v8
    lcd_ui.c/.h                 scene (Training + Practice widget sets, HAKISTA theme).
    idf_component.yml           Panel bring-up copied from ../morse_code _listener
    CMakeLists.txt
  sidetone/                  -- gated practice-oscillator tone + success/fail cue
    sidetone.c/.h              via the onboard ES8311 DAC (esp_codec_dev)
    idf_component.yml
    CMakeLists.txt
sdkconfig.defaults           -- octal PSRAM, 16MB flash, custom partition table,
                                  Montserrat 20/48 fonts, RGB565 byte swap
partitions.csv               -- single-app table, app partition grown to 4MB
dependencies.lock             -- pins the managed component versions
```

**Partition table:** a custom `partitions.csv` -- the standard single-app
layout with the `factory` partition grown to 4MB, because LVGL plus the
AXS15231B/touch drivers don't fit the default 1MB. This board has 16MB of
flash, so the app is grown rather than trimmed. The 24KB `nvs` partition
is unchanged (nothing is persisted yet). `sdkconfig` is a local cache,
not checked in -- delete it and rebuild if a partition-size mismatch
shows up after pulling changes.

## Open items

- **No settings persistence** -- timings, sidetone defaults, and the
  last-used mode / word length are all compile-time / power-on defaults.
- **Russo One isn't wired in** -- the colours are themed, text still
  renders in Montserrat.
- **No on-screen sidetone control** -- mute / volume / pitch are API-only,
  no settings screen.
- **No "faithful full-tree art" screen** -- the autocomplete list is
  plain text, not a drawn tree.
- **Practice doesn't replay the word as audio** -- you key from the
  letters on screen, not by ear.
- Not yet flashed to real V2 hardware, and not published as a repo (no
  browser flasher / prebuilt firmware).

## Attribution

This project is shared publicly for anyone to fork, learn from, and build
on. If you use this code -- in full or in a substantial part, source or
compiled firmware -- in your own project, please credit **Hakista TV**:

- [github.com/hakistatv](https://github.com/hakistatv)
- [youtube.com/HakistaTV](https://youtube.com/HakistaTV)

A link back to this repo in your README or project description is enough.
