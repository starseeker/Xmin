#!/usr/bin/env python3
"""Generate compact constexpr glyph tables from Xmin's pinned PCF arrays."""

from __future__ import annotations

import argparse
import ast
import hashlib
import pathlib
import re
import struct


PCF_METRICS = 1 << 2
PCF_BITMAPS = 1 << 3
PCF_BDF_ENCODINGS = 1 << 5
PCF_COMPRESSED_METRICS = 1 << 8
PCF_BYTE_MASK = 1 << 2
PCF_BIT_MASK = 1 << 3


def extract_array(source: str, name: str) -> bytes:
    match = re.search(
        rf"static const char file_{re.escape(name)}\[\] = \{{(.*?)\n\}};",
        source,
        re.DOTALL,
    )
    if match is None:
        raise SystemExit(f"cannot find embedded {name!r} PCF array")
    tokens = re.findall(r"'(?:\\.|[^'\\])*'", match.group(1))
    values = []
    for token in tokens:
        value = ast.literal_eval(token)
        if not isinstance(value, str) or len(value) != 1:
            raise SystemExit(f"invalid byte literal in {name!r}: {token}")
        values.append(ord(value))
    return bytes(values)


def table_of(data: bytes, table_type: int) -> tuple[int, bytes]:
    if data[:4] != b"\x01fcp":
        raise SystemExit("invalid PCF magic")
    count = struct.unpack_from("<I", data, 4)[0]
    for index in range(count):
        kind, table_format, size, offset = struct.unpack_from(
            "<IIII", data, 8 + index * 16
        )
        if kind == table_type:
            if offset + size > len(data):
                raise SystemExit("PCF table exceeds its input")
            return table_format, data[offset:offset + size]
    raise SystemExit(f"required PCF table {table_type:#x} is missing")


def byte_order(table_format: int) -> str:
    return ">" if table_format & PCF_BYTE_MASK else "<"


def metrics(data: bytes) -> list[tuple[int, int, int, int, int]]:
    table_format, table = table_of(data, PCF_METRICS)
    encoded_format = struct.unpack_from("<I", table, 0)[0]
    if encoded_format != table_format:
        raise SystemExit("PCF metrics format disagrees with its TOC")
    order = byte_order(table_format)
    result = []
    if table_format & PCF_COMPRESSED_METRICS:
        count = struct.unpack_from(order + "H", table, 4)[0]
        offset = 6
        for index in range(count):
            values = table[offset + index * 5:offset + index * 5 + 5]
            if len(values) != 5:
                raise SystemExit("truncated compressed PCF metrics")
            result.append(tuple(value - 0x80 for value in values))
    else:
        count = struct.unpack_from(order + "I", table, 4)[0]
        offset = 8
        for index in range(count):
            values = struct.unpack_from(order + "hhhhhh", table,
                                        offset + index * 12)
            result.append(values[:5])
    return result


def encoding(data: bytes, glyph_count: int) \
        -> tuple[list[int], list[int], int, int]:
    table_format, table = table_of(data, PCF_BDF_ENCODINGS)
    encoded_format = struct.unpack_from("<I", table, 0)[0]
    if encoded_format != table_format:
        raise SystemExit("PCF encoding format disagrees with its TOC")
    order = byte_order(table_format)
    minimum, maximum, minimum_byte, maximum_byte, default = \
        struct.unpack_from(order + "HHHHH", table, 4)
    if minimum_byte != 0 or maximum_byte != 0 or maximum > 255:
        raise SystemExit("only linear eight-bit embedded fonts are supported")
    count = maximum - minimum + 1
    indices = list(struct.unpack_from(order + f"{count}H", table, 14))
    default_index = 0
    if minimum <= default <= maximum:
        candidate = indices[default - minimum]
        if candidate != 0xFFFF:
            default_index = candidate
    if default_index >= glyph_count:
        raise SystemExit("invalid default PCF glyph")
    mapped = [default_index] * 256
    defined = [0] * 256
    for code, glyph in enumerate(indices, start=minimum):
        if glyph == 0xFFFF:
            continue
        if glyph >= glyph_count:
            raise SystemExit("PCF encoding references a missing glyph")
        mapped[code] = glyph
        defined[code] = 1
    return mapped, defined, minimum, maximum


def bitmaps(data: bytes, glyph_metrics: list[tuple[int, int, int, int, int]]) \
        -> tuple[list[tuple[int, int, int, int, int, int, int]], list[int]]:
    table_format, table = table_of(data, PCF_BITMAPS)
    encoded_format = struct.unpack_from("<I", table, 0)[0]
    if encoded_format != table_format:
        raise SystemExit("PCF bitmap format disagrees with its TOC")
    if bool(table_format & PCF_BYTE_MASK) != bool(table_format & PCF_BIT_MASK):
        raise SystemExit("mixed byte/bit-order PCF data is unsupported")
    order = byte_order(table_format)
    count = struct.unpack_from(order + "I", table, 4)[0]
    if count != len(glyph_metrics):
        raise SystemExit("PCF bitmap and metrics counts differ")
    offsets = struct.unpack_from(order + f"{count}I", table, 8)
    bitmap_start = 8 + count * 4 + 16
    glyph_pad = 1 << (table_format & 3)
    most_significant_first = bool(table_format & PCF_BIT_MASK)
    rows: list[int] = []
    glyphs = []
    for index, (left, right, width, ascent, descent) in enumerate(glyph_metrics):
        bitmap_width = right - left
        row_count = ascent + descent
        if bitmap_width < 0 or bitmap_width > 32 or row_count < 0:
            raise SystemExit("embedded glyph exceeds the compact table limits")
        stride = ((bitmap_width + 7) // 8 + glyph_pad - 1) \
            & ~(glyph_pad - 1)
        start = bitmap_start + offsets[index]
        end = start + stride * row_count
        if end > len(table):
            raise SystemExit("truncated PCF glyph bitmap")
        row_offset = len(rows)
        for row in range(row_count):
            row_data = table[start + row * stride:start + (row + 1) * stride]
            mask = 0
            for column in range(bitmap_width):
                bit = 7 - (column & 7) if most_significant_first \
                    else column & 7
                if row_data[column // 8] & (1 << bit):
                    mask |= 1 << column
            rows.append(mask)
        glyphs.append((left, right, width, ascent, descent,
                       row_offset, row_count))
    return glyphs, rows


def font_ascent_descent(data: bytes) -> tuple[int, int]:
    table_format, table = table_of(data, 1 << 1)
    encoded_format = struct.unpack_from("<I", table, 0)[0]
    if encoded_format != table_format:
        raise SystemExit("PCF accelerator format disagrees with its TOC")
    return struct.unpack_from(byte_order(table_format) + "ii", table, 12)


def emit_array(output: list[str], value_type: str, name: str,
               values: list[str], columns: int = 1) -> None:
    output.append(
        f"inline constexpr std::array<{value_type}, {len(values)}> {name}{{{{"
    )
    for index in range(0, len(values), columns):
        output.append("    " + ", ".join(values[index:index + columns]) + ",")
    output.append("}};")
    output.append("")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    source_bytes = args.input.read_bytes()
    source = source_bytes.decode("utf-8")
    fonts = []
    for source_name, symbol, canonical, alias in (
        ("6x13", "fixed", "-misc-fixed-medium-r-semicondensed--13-120-75-75-c-60-iso8859-1", "fixed"),
        ("cursor", "cursor", "cursor", "cursor"),
    ):
        data = extract_array(source, source_name)
        glyph_metrics = metrics(data)
        glyphs, rows = bitmaps(data, glyph_metrics)
        mapped, defined, minimum, maximum = encoding(data, len(glyphs))
        ascent, descent = font_ascent_descent(data)
        fonts.append((symbol, canonical, alias, minimum, maximum,
                      ascent, descent, glyphs, rows, mapped, defined))

    digest = hashlib.sha256(source_bytes).hexdigest()
    output = [
        "#ifndef XMIN_SERVER_GENERATED_CORE_FONTS_HPP",
        "#define XMIN_SERVER_GENERATED_CORE_FONTS_HPP",
        "",
        f"// Generated from xmin_builtin_fonts.c; sha256: {digest}",
        "// Run tools/generate-core-fonts.py; do not edit.",
        "",
        '#include "xmin/server/font.hpp"',
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace xmin::server::generated {",
        "",
    ]
    for (symbol, canonical, alias, minimum, maximum, ascent, descent,
         glyphs, rows, mapped, defined) in fonts:
        emit_array(output, "EmbeddedGlyph", f"{symbol}_glyphs", [
            "EmbeddedGlyph{" + ", ".join(str(value) for value in glyph) + "}"
            for glyph in glyphs
        ])
        emit_array(output, "std::uint32_t", f"{symbol}_rows",
                   [f"0x{row:08x}U" for row in rows], 6)
        emit_array(output, "std::uint16_t", f"{symbol}_encoding",
                   [str(value) for value in mapped], 12)
        emit_array(output, "std::uint8_t", f"{symbol}_defined",
                   [str(value) for value in defined], 24)
        all_characters_exist = len(glyphs) == maximum - minimum + 1
        output.extend([
            f"inline constexpr EmbeddedFont {symbol}_font{{",
            f"    {canonical!r}, {alias!r}, {minimum}, {maximum},",
            f"    0, {str(all_characters_exist).lower()}, {ascent}, {descent},",
            f"    {symbol}_glyphs.data(),",
            f"    {symbol}_glyphs.size(), {symbol}_rows.data(),",
            f"    {symbol}_encoding.data(), {symbol}_defined.data()",
            "};",
            "",
        ])
    # Python repr uses the same quoting for these ASCII names except that C++
    # requires double quotes.
    text = "\n".join(output).replace("'", '"')
    text += "} // namespace xmin::server::generated\n\n#endif\n"
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
