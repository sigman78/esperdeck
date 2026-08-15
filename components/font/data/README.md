# Glyph data blobs

`terminusWxH[b].bin` hold the compressed glyph tables (format v1 records in
a `CDF1` container — layout documented in `tools/fontbin2c.py`, record
format in `../terminus_font.h`). The build expands each blob into a `.c`
translation unit via `tools/fontbin2c.py` (wired by `../font_data.cmake`);
nothing here is compiled directly and no generated `.c` is committed.

To regenerate the blobs, see `tools/gen_terminus.py` (`bdf` mode from
Terminus BDF sources, or `convert` mode from pre-v1 uncompressed tables;
both support `--verify`).

## License

The glyph bitmaps are derived from the **Terminus Font**,
Copyright (c) 2020 Dimitar Zhekov <dimitar.zhekov@gmail.com>,
with Reserved Font Name "Terminus Font", licensed under the
**SIL Open Font License, Version 1.1** — full text in `../LICENSE`.
These files are NOT covered by the repository's MIT license.
