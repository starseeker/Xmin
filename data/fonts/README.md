# Embedded core fonts

`xmin_builtin_fonts.c` contains the core X11 `fixed` and `cursor` PCFs as
uncompressed C arrays. It is generated from libXfont2 2.0.8's pinned built-in
gzip arrays by:

```sh
python3 tools/generate-builtin-fonts.py \
  /path/to/libXfont2-2.0.8/src/builtins/fonts.c \
  data/fonts/xmin_builtin_fonts.c
```

Generation uses only the Python standard library. Neither Python nor a
compression library is required to configure, build, or run Xmin. The source
file records the decompressed PCF sizes and SHA-256 digests.
