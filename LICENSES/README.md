# License records

This directory contains the complete notices for every retained third-party
source or generated-data input listed in `UPSTREAM.toml`.

Xmin ships source from pixman and, when client OpenGL is enabled, OSMesa. The
optional minimalist font client ships struetype and embeds the Go proportional
and monospace families, plus focused public Xlib, Xorg protocol, XRender, and
Xft headers. X.Org's rgb database supplies the normalized named-color table.
The xcb-proto XML is maintenance input. The libXfont2 and
xkeyboard-config notices cover the fixed font and keymap data derived from
those releases; no runtime code from either project remains.
The optional Unix desktop builds the imported dash 0.5.13.4 sources as
`xmin-sh`. The upstream release contains one GPL-licensed GNU Bash signal-name
generator. Xmin's audited importer excludes that file and installs the
independent `tools/dash-mksignames.c` implementation under 0BSD in its place,
so neither the GNU helper nor output derived from it enters the repository or
`xmin-sh`. The upstream combined COPYING file is retained verbatim and still
describes the deliberately omitted file.
The interactive profile additionally pins GLFW for the host viewer, JWM for
the guest desktop, and st for the guest terminal. Their complete permissive
notices are recorded here; Xmin's st process-backend adaptation is maintained
as a separate patch.
