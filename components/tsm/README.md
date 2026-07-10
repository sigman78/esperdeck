Terminal State Machine
======================

Minimalistic VT100 like terminal emulator, consist of:

- VT parser
- Term state manager

Optimized for minimal memory footprint. Keeps own term-buffer.

Supported features:
- Near full VT100 support
- UTF8 capable, but capped to 0xffff (mostly limited by bitmap font subset and footprint)
- 256 Color
- Alt screen
- Cursor save / restore
- Terminal reporting

Upcoming features:
- True color support
- Mouse support
- Configurable scrollback
- Double-width cells
- Combining characters & grapheme clusters