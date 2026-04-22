# BaseSixFour

Reads the input image, encodes to WebP, JPEG, or PNG, and outputs the
encoded bytes as a base64 string on a knob. Useful for piping renders to
HTTP APIs, copying image data to the clipboard, or embedding small
previews into text-based workflows.

## Install

Download the latest release from the repo's Releases page, drop the file
into your `.nuke` directory:

- Windows: `%USERPROFILE%\.nuke\BaseSixFour.dll`
- macOS: `~/.nuke/BaseSixFour.dylib`

Restart Nuke. The node appears under **Image > BaseSixFour**.

### macOS Gatekeeper

If macOS refuses to load an unsigned plugin:

```
codesign --force --sign - ~/.nuke/BaseSixFour.dylib
# or
xattr -dr com.apple.quarantine ~/.nuke/BaseSixFour.dylib
```

## Usage

```python
node = nuke.createNode("BaseSixFour")
node.setInput(0, some_read_node)
nuke.execute(node, nuke.frame(), nuke.frame())
b64 = node["_output"].value()
```

Buttons:

- **Render** — executes at the current frame
- **Copy** — copies the base64 string to the system clipboard

Knobs:

- `format` — webp / jpeg / png
- `quality` — 0..100. For WebP near-lossless this is the preprocessing
  level (lower = smaller, 100 = truly lossless). For WebP lossy and JPEG
  this is perceptual quality. Ignored for PNG.
- `lossy` — switches WebP from near-lossless to VP8 lossy.

## Technical notes

- Input is read as linear float and converted to sRGB 8-bit before
  encoding (standard gamma for WebP/JPEG/PNG).
- Alpha channel is currently ignored (RGB only).
- WebP is produced via libwebp (statically linked, v1.4.0).
- JPEG and PNG are produced via `stb_image_write.h` (no external deps).

## License

Your own code: pick one. stb_image_write is public domain.
libwebp is BSD.
