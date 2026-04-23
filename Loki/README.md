Loki is a text-as-pixels toolkit for Nuke. It's a suite of 7 C++ NDK plugins plus 2 Python scripts, all designed around a clever idea: encode text strings into 128×128 RGB images so they can flow through Nuke's node graph like any other image data.

The encoding scheme: A 128×128×3 image gives you 49,152 bytes. The first 4 bytes store the string length as a little-endian uint32, then the raw string bytes are packed sequentially into R→G→B across pixels, row by row top-down. Max capacity is 49,148 bytes of text per frame.

The nodes:

- LokiEncode — Generator (no inputs). Has a text knob (supports TCL expressions) and outputs the encoded pixel image. This is the on-ramp.
- LokiDecode — Terminal/inspector (1 input). Passes pixels through unchanged, but decodes the text into a hidden prompt_text knob accessible via Python. This is the off-ramp.
- LokiShuffle — Strips punctuation, lowercases, splits on whitespace, and shuffles word order. Has a seed slider and a Shuffle button.
- LokiMerge — Two-input node (A/B). Two modes: append (concatenate) or lerp (per-word random pick from A or B weighted by a mix slider).
- LokiRemove — Removes a fraction of words at random. Rate slider + seed.
- LokiNoise — Replaces a fraction of characters (not words) with random ASCII letters. Rate slider + seed.
- LokiSort — Sorts words alphabetically, ascending or descending, case-insensitive.
- LokiFilter - Remove listed words from a list of words

The Python side:

- loki_panel.py — Auto-attaches a PyCustom_Knob Qt widget to LokiDecode nodes that polls prompt_text and displays it live in the properties panel. Registers via nuke.addOnUserCreate and nuke.addOnScriptLoad.
- loki_watcher.py — A standalone floating Qt window that can watch any node/knob combination by name, with configurable poll interval. More general-purpose.