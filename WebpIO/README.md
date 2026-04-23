# WebpIO

Two Foundry Nuke plugins:

- **webpReader** — reads `.webp` images. Extracts metadata (XMP
  `dc:description`, EXIF `ImageDescription`, EXIF `UserComment`) into Nuke's
  metadata tree as `webp/description`, `exif/ImageDescription`, and
  `webp/usercomment`.
- **webpWriter** — writes `.webp` images. Default mode is **near-lossless**
  (lossless encoder with preprocessing); flip the `lossy` knob for VP8
  lossy encoding. Optionally embeds a description into both XMP and EXIF.

Both plugins are RGB only; alpha is currently ignored.

## Install

Drop both binaries into your `.nuke` directory:

- Windows: `%USERPROFILE%\.nuke\webpReader.dll` + `webpWriter.dll`
- macOS:   `~/.nuke/webpReader.dylib` + `~/.nuke/webpWriter.dylib`

Restart Nuke. WebP files now read/write through the standard Read/Write nodes.

### macOS Gatekeeper

If macOS refuses to load unsigned plugins:

```
codesign --force --sign - ~/.nuke/webpReader.dylib
codesign --force --sign - ~/.nuke/webpWriter.dylib
```

## webpWriter knobs

- `quality` (0..100) — In default near-lossless mode, this is the
  preprocessing level (0 = most preprocessing/smallest file, 100 = truly
  lossless). In lossy mode, this is perceptual quality.
- `lossy` — Switch from near-lossless to VP8 lossy encoding.
- `description` — Optional text embedded as both XMP `dc:description` and
  EXIF `ImageDescription`.

## Technical notes

- Built against libwebp v1.4.0 (statically linked, no runtime dependency
  on system libwebp).
- webpReader uses `libwebp` (decoder) + `libwebpdemux`.
- webpWriter uses `libwebp` (encoder) + `libwebpmux`.

## License

Your own code: pick one. libwebp is BSD.
