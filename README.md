# Uplink 1.6 (GOG) — native SDL2 and WebAssembly port, with a Thai translation

Research / educational project. **Bring your own copy**: everything here is built from the `uplink.exe` and
`*.dat` files of your own installation (the folder above this one). No game code or data is stored in the
sources; the generated C, the builds and the Thai table are derived from your copy and **must stay private**
(do not publish them or put them in a public repository).

## Route
Uplink's source is only licensed to buyers of the Developer CD, so this is a **static recompilation** of
`uplink.exe` (MSVC 2008, x86) to C, using the game-web-port kit:

- analysis: Ghidra 10.4 headless (`tools/ghidra/GhidraExport.java`) exporting the IDA-style JSON, completed by
  `tools/fix_export.py` (imports from the PE, unresolved jump tables, ordinal-import thunks);
- `tools/recomp.py` → `gen/rec_*.c` (554k instructions); patched here for VS2005+ C++ EH tables
  (`__CxxFrameHandler3`), hook resume addresses past a split loop head, and call targets in code the analysis
  never reached;
- platform layer (`src/`): SDL 1.2 on SDL2 (`sdl12.c`), OpenGL 1.x immediate mode / `glBitmap` / `glDrawPixels`
  on GLES2/WebGL into a persistent FBO (`gl1.c`; the game never calls `glClear`), SDL_mixer with libmikmod for
  the `.uni` music (`mixer.c`), Win32/CRT pieces (`kernel32.c`, `user32.c`, `misc.c`), the network DLLs stubbed
  (Uplink's optional client/server mode is not offered);
- main loop: `GciMainLoop` (0x4ee800) is split at its loop head so the browser drives one pass per frame
  (`src/game.c`, hooks in `recomp_config.json`).

## Thai
`src/lang.c` wraps `GciDrawText` (0x4ea0b0), `GciTextWidth` (0x4ea120) and `wordwraptext` (0x490690):
each string is looked up whole, then line by line (exact, `%s/%d` templates from the game's own format strings
with reordering `%1$s`, fragments glued around names in news/mission text, typewriter prefixes); Thai runs are
drawn with stb_truetype from Noto Sans Thai (OFL, `lang/th/OFL.txt`) with simple mark stacking, ASCII runs still
with the game's own Dungeon font; translated text is re-wrapped by pixel width.

- `analysis/strings_en.tsv` / `lines_en.tsv`: `tools/lang/extract_strings.py`, `filter_strings.py` (≈3900 lines)
- `lang/th/src/*.tsv`: the translation, ≈3300 entries (UI, help, tutorial, emails, missions, news, storyline);
  software/hardware product names, people, companies and file names stay in English
- `python tools/lang/build_lang.py lang/th` → `lang/th/strings.tsv` (checks the format conversions)
- language choice: `--lang th` (native), `PORT_LANG=th`, or the ภาษาไทย / English buttons on the web page

## Build
Requirements: TDM-GCC/MinGW-w64, GNU make, Python 3 with `pefile` + `capstone`, emsdk (here
`D:/AI/hb4/tools/emsdk`). SDL2 MinGW dev files, libmikmod 3.3.11.1 and stb_truetype are in `thirdparty/`.

```bash
python tools/fix_export.py ../uplink.exe analysis/ghidra_export.json analysis/export.json
python tools/recomp.py ../uplink.exe analysis/export.json gen --config recomp_config.json
python tools/lang/build_lang.py lang/th
make -j12                                                     # native: build/uplink_port.exe
make -f Makefile.web -j12 EMSDK=D:/AI/hb4/tools/emsdk         # web: build_web/out/
```
(Re-running the Ghidra analysis: see the comment at the top of `tools/ghidra/GhidraExport.java`.)

## Run
Native (from `port/run`, game folder two levels up, saves in `run/save`):
```bash
../build/uplink_port.exe ../.. save --lang th
```
Web — must be served over HTTP, and stays private because `index.data` contains your game files:
```bash
python -m http.server 8080 --directory build_web/out
```
then open http://localhost:8080. Saves live in the browser's IndexedDB (`/save`).

## Debugging switches (environment, or `?PORT_X=...` in the page address)
`PORT_SHOT=100,500` screenshots · `PORT_INPUT="900:click:320,380;960:text:neo;1000:key:13"` scripted input
(game coordinates; `click rclick move key down up char text`) · `PORT_WATCHDOG=30` print where the guest is and
quit · `PORT_FILELOG=1` file opens/reads · `PORT_LANGLOG=1` English strings that found no translation ·
`PORT_INPUTLOG=1` mouse events · `PORT_GLLOG=1` draw calls · `PORT_THAI_SCALE=2.45` Thai size vs. Latin ·
`PORT_NOSOUND=1`.

## Status
Verified natively (Windows, 60 fps): intro, public access server, registration (typing name/password),
gateway location map, gateway boot, tutorial, link list, welcome email — all in Thai; music and sound effects;
saves written. Verified in Chrome-based browser: loads, renders, Thai text, mouse clicks and keyboard input,
reaches the registration form.
Not verified yet: a full hacking mission end to end, the later storyline screens, save/load after a page reload,
performance in a normal (non-embedded) browser tab, the options screen's video-mode changes.

Known limits: network/multi-monitor mode disabled; very long Thai lines in fixed-width UI boxes may be clipped
where the game does not wrap; strings the game assembles in ways the table does not cover stay English
(`PORT_LANGLOG=1` lists them — add entries to `lang/th/src/` and rebuild).
