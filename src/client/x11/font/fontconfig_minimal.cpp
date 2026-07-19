#include "font_engine.hpp"

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using xmin::client::font::EmbeddedFace;
using xmin::client::font::FontEngine;
using xmin::client::font::select_embedded_face;

namespace {

struct BoolValue {
    FcBool value;
};

struct CharsetValue {
    std::shared_ptr<_FcCharSet> value;
};

using PropertyValue = std::variant<
    int, double, std::string, BoolValue, FcMatrix, CharsetValue>;

std::string lower_copy(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(
                character >= 'A' && character <= 'Z'
                    ? character - 'A' + 'a'
                    : character);
        });
    return value;
}

bool family_is_monospace(const std::string &family)
{
    const std::string lowered = lower_copy(family);
    return lowered.find("mono") != std::string::npos ||
           lowered.find("fixed") != std::string::npos ||
           lowered.find("courier") != std::string::npos ||
           lowered.find("console") != std::string::npos ||
           lowered.find("typewriter") != std::string::npos;
}

template <typename T>
const T *property(
    const _FcPattern *pattern, const char *object, int id) noexcept;

EmbeddedFace pattern_face(const _FcPattern *pattern) noexcept;

std::shared_ptr<_FcCharSet> make_charset(EmbeddedFace face);

const FontEngine &coverage_font(EmbeddedFace face)
{
    switch (face) {
    case EmbeddedFace::sans_bold: {
        static const FontEngine font(face, 12.0F);
        return font;
    }
    case EmbeddedFace::sans_italic: {
        static const FontEngine font(face, 12.0F);
        return font;
    }
    case EmbeddedFace::sans_bold_italic: {
        static const FontEngine font(face, 12.0F);
        return font;
    }
    case EmbeddedFace::mono_regular: {
        static const FontEngine font(face, 12.0F);
        return font;
    }
    case EmbeddedFace::mono_bold: {
        static const FontEngine font(face, 12.0F);
        return font;
    }
    case EmbeddedFace::mono_italic: {
        static const FontEngine font(face, 12.0F);
        return font;
    }
    case EmbeddedFace::mono_bold_italic: {
        static const FontEngine font(face, 12.0F);
        return font;
    }
    case EmbeddedFace::sans_regular:
        break;
    }
    static const FontEngine font(EmbeddedFace::sans_regular, 12.0F);
    return font;
}

FcPattern *catalog_pattern(
    const char *family, const char *style, bool monospace, bool bold,
    bool italic);

FcFontSet *make_font_set(std::vector<FcPattern *> patterns)
{
    auto set = std::unique_ptr<FcFontSet>(new (std::nothrow) FcFontSet{});
    if (!set) {
        for (FcPattern *pattern : patterns) {
            FcPatternDestroy(pattern);
        }
        return nullptr;
    }
    if (!patterns.empty()) {
        set->fonts = static_cast<FcPattern **>(
            std::calloc(patterns.size(), sizeof(FcPattern *)));
        if (set->fonts == nullptr) {
            for (FcPattern *pattern : patterns) {
                FcPatternDestroy(pattern);
            }
            return nullptr;
        }
        std::copy(patterns.begin(), patterns.end(), set->fonts);
    }
    set->nfont = static_cast<int>(patterns.size());
    set->sfont = set->nfont;
    return set.release();
}

template <typename T>
FcBool add_property(FcPattern *pattern, const char *object, T value);

template <typename T>
FcResult get_property(
    const FcPattern *pattern, const char *object, int id, T **value);

} // namespace

struct _FcCharSet {
    EmbeddedFace face = EmbeddedFace::sans_regular;
};

struct _FcPattern {
    std::unordered_map<std::string, std::vector<PropertyValue>> properties;
};

namespace {

template <typename T>
const T *property(
    const _FcPattern *pattern, const char *object, int id) noexcept
{
    if (pattern == nullptr || object == nullptr || id < 0) {
        return nullptr;
    }
    const auto found = pattern->properties.find(object);
    if (found == pattern->properties.end() ||
        static_cast<std::size_t>(id) >= found->second.size()) {
        return nullptr;
    }
    return std::get_if<T>(&found->second[static_cast<std::size_t>(id)]);
}

EmbeddedFace pattern_face(const _FcPattern *pattern) noexcept
{
    const std::string *family = property<std::string>(pattern, FC_FAMILY, 0);
    const int *weight = property<int>(pattern, FC_WEIGHT, 0);
    const int *slant = property<int>(pattern, FC_SLANT, 0);
    const int *spacing = property<int>(pattern, FC_SPACING, 0);
    const bool monospace =
        (spacing != nullptr && *spacing >= FC_MONO) ||
        (family != nullptr && family_is_monospace(*family));
    return select_embedded_face(
        monospace, weight != nullptr && *weight >= FC_WEIGHT_DEMIBOLD,
        slant != nullptr && *slant >= FC_SLANT_ITALIC);
}

std::shared_ptr<_FcCharSet> make_charset(EmbeddedFace face)
{
    auto result = std::make_shared<_FcCharSet>();
    result->face = face;
    return result;
}

FcPattern *catalog_pattern(
    const char *family, const char *style, bool monospace, bool bold,
    bool italic)
{
    std::unique_ptr<FcPattern> pattern(FcPatternCreate());
    if (!pattern ||
        !FcPatternAddString(
            pattern.get(), FC_FAMILY,
            reinterpret_cast<const FcChar8 *>(family)) ||
        !FcPatternAddString(
            pattern.get(), FC_STYLE,
            reinterpret_cast<const FcChar8 *>(style)) ||
        !FcPatternAddInteger(
            pattern.get(), FC_WEIGHT,
            bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR) ||
        !FcPatternAddInteger(
            pattern.get(), FC_SLANT,
            italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN) ||
        !FcPatternAddInteger(
            pattern.get(), FC_SPACING,
            monospace ? FC_MONO : FC_PROPORTIONAL) ||
        !FcPatternAddDouble(pattern.get(), FC_PIXEL_SIZE, 12.0)) {
        return nullptr;
    }
    const auto charset = make_charset(
        select_embedded_face(monospace, bold, italic));
    if (!FcPatternAddCharSet(
            pattern.get(), FC_CHARSET, charset.get())) {
        return nullptr;
    }
    return pattern.release();
}

template <typename T>
FcBool add_property(FcPattern *pattern, const char *object, T value)
{
    if (pattern == nullptr || object == nullptr) {
        return FcFalse;
    }
    try {
        pattern->properties[object].emplace_back(std::move(value));
        return FcTrue;
    }
    catch (...) {
        return FcFalse;
    }
}

template <typename T>
FcResult get_property(
    const FcPattern *pattern, const char *object, int id, T **value)
{
    if (value == nullptr) {
        return FcResultNoMatch;
    }
    const T *found = property<T>(pattern, object, id);
    if (found == nullptr) {
        return FcResultNoMatch;
    }
    *value = const_cast<T *>(found);
    return FcResultMatch;
}

} // namespace

extern "C" {

FcBool FcInit(void)
{
    return FcTrue;
}

FcPattern *FcPatternCreate(void)
{
    return new (std::nothrow) FcPattern;
}

FcPattern *FcPatternDuplicate(const FcPattern *pattern)
{
    if (pattern == nullptr) {
        return nullptr;
    }
    try {
        return new FcPattern(*pattern);
    }
    catch (...) {
        return nullptr;
    }
}

void FcPatternDestroy(FcPattern *pattern)
{
    delete pattern;
}

FcBool FcPatternDel(FcPattern *pattern, const char *object)
{
    if (pattern == nullptr || object == nullptr) {
        return FcFalse;
    }
    return pattern->properties.erase(object) != 0 ? FcTrue : FcFalse;
}

FcBool FcPatternAddInteger(
    FcPattern *pattern, const char *object, int value)
{
    return add_property(pattern, object, value);
}

FcBool FcPatternAddDouble(
    FcPattern *pattern, const char *object, double value)
{
    return add_property(pattern, object, value);
}

FcBool FcPatternAddString(
    FcPattern *pattern, const char *object, const FcChar8 *value)
{
    if (value == nullptr) {
        return FcFalse;
    }
    try {
        return add_property(
            pattern, object,
            std::string(reinterpret_cast<const char *>(value)));
    }
    catch (...) {
        return FcFalse;
    }
}

FcBool FcPatternAddBool(
    FcPattern *pattern, const char *object, FcBool value)
{
    return add_property(pattern, object, BoolValue{value});
}

FcBool FcPatternAddMatrix(
    FcPattern *pattern, const char *object, const FcMatrix *value)
{
    return value == nullptr ? FcFalse : add_property(pattern, object, *value);
}

FcBool FcPatternAddCharSet(
    FcPattern *pattern, const char *object, const FcCharSet *value)
{
    if (value == nullptr) {
        return FcFalse;
    }
    try {
        return add_property(
            pattern, object,
            CharsetValue{std::make_shared<FcCharSet>(*value)});
    }
    catch (...) {
        return FcFalse;
    }
}

FcResult FcPatternGetInteger(
    const FcPattern *pattern, const char *object, int id, int *value)
{
    const int *found = property<int>(pattern, object, id);
    if (found == nullptr || value == nullptr) {
        return FcResultNoMatch;
    }
    *value = *found;
    return FcResultMatch;
}

FcResult FcPatternGetDouble(
    const FcPattern *pattern, const char *object, int id, double *value)
{
    const double *found = property<double>(pattern, object, id);
    if (found == nullptr || value == nullptr) {
        return FcResultNoMatch;
    }
    *value = *found;
    return FcResultMatch;
}

FcResult FcPatternGetString(
    const FcPattern *pattern, const char *object, int id, FcChar8 **value)
{
    std::string *found = nullptr;
    const FcResult result = get_property(pattern, object, id, &found);
    if (result == FcResultMatch && value != nullptr) {
        *value = reinterpret_cast<FcChar8 *>(found->data());
    }
    return value == nullptr ? FcResultNoMatch : result;
}

FcResult FcPatternGetBool(
    const FcPattern *pattern, const char *object, int id, FcBool *value)
{
    BoolValue *found = nullptr;
    const FcResult result = get_property(pattern, object, id, &found);
    if (result == FcResultMatch && value != nullptr) {
        *value = found->value;
    }
    return value == nullptr ? FcResultNoMatch : result;
}

FcResult FcPatternGetMatrix(
    const FcPattern *pattern, const char *object, int id, FcMatrix **value)
{
    return get_property(pattern, object, id, value);
}

FcResult FcPatternGetCharSet(
    const FcPattern *pattern, const char *object, int id, FcCharSet **value)
{
    if (value == nullptr) {
        return FcResultNoMatch;
    }
    CharsetValue *found = nullptr;
    const FcResult result = get_property(pattern, object, id, &found);
    if (result == FcResultMatch) {
        *value = found->value.get();
    }
    return result;
}

FcPattern *FcNameParse(const FcChar8 *name)
{
    if (name == nullptr) {
        return nullptr;
    }
    try {
        const std::string description(reinterpret_cast<const char *>(name));
        const std::string lowered = lower_copy(description);
        const std::size_t separator = description.find(':');
        const std::string family = description.substr(0, separator);
        const bool bold = lowered.find("bold") != std::string::npos;
        const bool italic = lowered.find("italic") != std::string::npos ||
                            lowered.find("oblique") != std::string::npos;
        return catalog_pattern(
            family.empty() ? "Go" : family.c_str(),
            bold ? (italic ? "Bold Italic" : "Bold")
                 : (italic ? "Italic" : "Regular"),
            family_is_monospace(family), bold, italic);
    }
    catch (...) {
        return nullptr;
    }
}

FcChar8 *FcNameUnparse(FcPattern *pattern)
{
    FcChar8 *family = nullptr;
    FcChar8 *style = nullptr;
    if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) != FcResultMatch) {
        family = reinterpret_cast<FcChar8 *>(const_cast<char *>("Go"));
    }
    if (FcPatternGetString(pattern, FC_STYLE, 0, &style) != FcResultMatch) {
        style = reinterpret_cast<FcChar8 *>(const_cast<char *>("Regular"));
    }
    try {
        const std::string result =
            reinterpret_cast<const char *>(family) + std::string(":style=") +
            reinterpret_cast<const char *>(style);
        auto *copy = static_cast<FcChar8 *>(std::malloc(result.size() + 1));
        if (copy != nullptr) {
            std::memcpy(copy, result.c_str(), result.size() + 1);
        }
        return copy;
    }
    catch (...) {
        return nullptr;
    }
}

FcObjectSet *FcObjectSetCreate(void)
{
    return static_cast<FcObjectSet *>(std::calloc(1, sizeof(FcObjectSet)));
}

FcBool FcObjectSetAdd(FcObjectSet *set, const char *object)
{
    if (set == nullptr || object == nullptr) {
        return FcFalse;
    }
    char **revised = static_cast<char **>(
        std::realloc(set->objects, sizeof(char *) * (set->nobject + 1)));
    if (revised == nullptr) {
        return FcFalse;
    }
    set->objects = revised;
    set->objects[set->nobject] = ::strdup(object);
    if (set->objects[set->nobject] == nullptr) {
        return FcFalse;
    }
    ++set->nobject;
    set->sobject = set->nobject;
    return FcTrue;
}

void FcObjectSetDestroy(FcObjectSet *set)
{
    if (set == nullptr) {
        return;
    }
    for (int index = 0; index < set->nobject; ++index) {
        std::free(set->objects[index]);
    }
    std::free(set->objects);
    std::free(set);
}

FcObjectSet *FcObjectSetVaBuild(const char *first, va_list arguments)
{
    std::unique_ptr<FcObjectSet, decltype(&FcObjectSetDestroy)> set(
        FcObjectSetCreate(), FcObjectSetDestroy);
    for (const char *object = first; object != nullptr;
         object = va_arg(arguments, const char *)) {
        if (!FcObjectSetAdd(set.get(), object)) {
            return nullptr;
        }
    }
    return set.release();
}

FcObjectSet *FcObjectSetBuild(const char *first, ...)
{
    va_list arguments;
    va_start(arguments, first);
    FcObjectSet *result = FcObjectSetVaBuild(first, arguments);
    va_end(arguments);
    return result;
}

FcFontSet *FcFontList(FcConfig *, FcPattern *, FcObjectSet *)
{
    std::vector<FcPattern *> patterns;
    try {
        constexpr const char *families[]{"Go", "Go Mono"};
        constexpr const char *styles[]{
            "Regular", "Bold", "Italic", "Bold Italic"};
        for (int family = 0; family < 2; ++family) {
            for (int style = 0; style < 4; ++style) {
                FcPattern *pattern = catalog_pattern(
                    families[family], styles[style], family == 1,
                    style == 1 || style == 3, style >= 2);
                if (pattern == nullptr) {
                    for (FcPattern *existing : patterns) {
                        FcPatternDestroy(existing);
                    }
                    return nullptr;
                }
                patterns.push_back(pattern);
            }
        }
        return make_font_set(std::move(patterns));
    }
    catch (...) {
        for (FcPattern *pattern : patterns) {
            FcPatternDestroy(pattern);
        }
        return nullptr;
    }
}

FcFontSet *FcFontSort(
    FcConfig *, FcPattern *pattern, FcBool, FcCharSet **coverage,
    FcResult *result)
{
    const EmbeddedFace face = pattern_face(pattern);
    const bool mono = face >= EmbeddedFace::mono_regular;
    const bool bold = face == EmbeddedFace::sans_bold ||
                      face == EmbeddedFace::sans_bold_italic ||
                      face == EmbeddedFace::mono_bold ||
                      face == EmbeddedFace::mono_bold_italic;
    const bool italic = face == EmbeddedFace::sans_italic ||
                        face == EmbeddedFace::sans_bold_italic ||
                        face == EmbeddedFace::mono_italic ||
                        face == EmbeddedFace::mono_bold_italic;
    FcPattern *matched = FcPatternDuplicate(pattern);
    if (matched != nullptr) {
        FcPatternDel(matched, FC_FAMILY);
        FcPatternDel(matched, FC_STYLE);
        FcPatternDel(matched, FC_SPACING);
        FcPatternDel(matched, FC_CHARSET);
        const char *style = bold ? (italic ? "Bold Italic" : "Bold")
                                 : (italic ? "Italic" : "Regular");
        const auto charset = make_charset(face);
        if (!FcPatternAddString(
                matched, FC_FAMILY,
                reinterpret_cast<const FcChar8 *>(mono ? "Go Mono" : "Go")) ||
            !FcPatternAddString(
                matched, FC_STYLE,
                reinterpret_cast<const FcChar8 *>(style)) ||
            !FcPatternAddInteger(
                matched, FC_SPACING, mono ? FC_MONO : FC_PROPORTIONAL) ||
            !FcPatternAddCharSet(matched, FC_CHARSET, charset.get())) {
            FcPatternDestroy(matched);
            matched = nullptr;
        }
    }
    if (coverage != nullptr) {
        const auto *charset =
            matched == nullptr
                ? nullptr
                : property<CharsetValue>(matched, FC_CHARSET, 0);
        *coverage = charset == nullptr
            ? nullptr
            : FcCharSetCopy(charset->value.get());
    }
    if (result != nullptr) {
        *result = matched == nullptr ? FcResultOutOfMemory : FcResultMatch;
    }
    return matched == nullptr
        ? nullptr
        : make_font_set(std::vector<FcPattern *>{matched});
}

FcPattern *FcFontRenderPrepare(
    FcConfig *, FcPattern *pattern, FcPattern *)
{
    FcResult result = FcResultNoMatch;
    FcFontSet *set = FcFontSort(nullptr, pattern, FcTrue, nullptr, &result);
    if (set == nullptr || set->nfont == 0) {
        FcFontSetDestroy(set);
        return nullptr;
    }
    FcPattern *prepared = set->fonts[0];
    set->fonts[0] = nullptr;
    FcFontSetDestroy(set);
    return prepared;
}

FcPattern *FcFontSetMatch(
    FcConfig *, FcFontSet **, int, FcPattern *pattern, FcResult *result)
{
    FcPattern *matched = FcFontRenderPrepare(nullptr, pattern, nullptr);
    if (result != nullptr) {
        *result = matched == nullptr ? FcResultNoMatch : FcResultMatch;
    }
    return matched;
}

void FcFontSetDestroy(FcFontSet *set)
{
    if (set == nullptr) {
        return;
    }
    for (int index = 0; index < set->nfont; ++index) {
        FcPatternDestroy(set->fonts[index]);
    }
    std::free(set->fonts);
    delete set;
}

FcPattern *FcFontMatch(
    FcConfig *, FcPattern *pattern, FcResult *result)
{
    FcPattern *matched = FcFontRenderPrepare(nullptr, pattern, nullptr);
    if (result != nullptr)
        *result = matched == nullptr ? FcResultNoMatch : FcResultMatch;
    return matched;
}

void FcDefaultSubstitute(FcPattern *)
{
}

FcCharSet *FcCharSetCreate(void)
{
    return new (std::nothrow) FcCharSet;
}

FcCharSet *FcCharSetCopy(FcCharSet *source)
{
    return source == nullptr ? nullptr : new (std::nothrow) FcCharSet(*source);
}

FcBool FcCharSetAddChar(FcCharSet *charset, FcChar32 codepoint)
{
    return charset != nullptr && codepoint <= 0x10ffffU ? FcTrue : FcFalse;
}

void FcCharSetDestroy(FcCharSet *charset)
{
    delete charset;
}

FcBool FcCharSetHasChar(const FcCharSet *charset, FcChar32 codepoint)
{
    if (charset == nullptr || codepoint > 0x10ffffU) {
        return FcFalse;
    }
    const FontEngine &font = coverage_font(charset->face);
    return font.valid() && font.glyph_index(codepoint) != 0 ? FcTrue : FcFalse;
}

FcBool FcConfigSubstitute(FcConfig *, FcPattern *, FcMatchKind)
{
    return FcTrue;
}

void FcMatrixInit(FcMatrix *matrix)
{
    if (matrix != nullptr) {
        *matrix = {1.0, 0.0, 0.0, 1.0};
    }
}

FcBool FcMatrixEqual(const FcMatrix *left, const FcMatrix *right)
{
    return left != nullptr && right != nullptr && left->xx == right->xx &&
                   left->xy == right->xy && left->yx == right->yx &&
                   left->yy == right->yy
        ? FcTrue
        : FcFalse;
}

void FcMatrixMultiply(
    FcMatrix *result, const FcMatrix *left, const FcMatrix *right)
{
    if (result == nullptr || left == nullptr || right == nullptr) {
        return;
    }
    const FcMatrix value{
        left->xx * right->xx + left->xy * right->yx,
        left->xx * right->xy + left->xy * right->yy,
        left->yx * right->xx + left->yy * right->yx,
        left->yx * right->xy + left->yy * right->yy};
    *result = value;
}

void FcMatrixRotate(FcMatrix *result, double cosine, double sine)
{
    if (result != nullptr) {
        const FcMatrix rotation{cosine, -sine, sine, cosine};
        FcMatrixMultiply(result, result, &rotation);
    }
}

void FcMatrixScale(FcMatrix *result, double x, double y)
{
    if (result != nullptr) {
        const FcMatrix scale{x, 0.0, 0.0, y};
        FcMatrixMultiply(result, result, &scale);
    }
}

void FcMatrixShear(FcMatrix *result, double x, double y)
{
    if (result != nullptr) {
        const FcMatrix shear{1.0, x, y, 1.0};
        FcMatrixMultiply(result, result, &shear);
    }
}

int FcUtf8ToUcs4(const FcChar8 *source, FcChar32 *codepoint, int length)
{
    if (source == nullptr || codepoint == nullptr || length <= 0) {
        return 0;
    }
    const std::uint8_t first = source[0];
    if (first < 0x80U) {
        *codepoint = first;
        return 1;
    }
    int count = 0;
    FcChar32 value = 0;
    FcChar32 minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
        count = 2;
        value = first & 0x1fU;
        minimum = 0x80U;
    }
    else if ((first & 0xf0U) == 0xe0U) {
        count = 3;
        value = first & 0x0fU;
        minimum = 0x800U;
    }
    else if ((first & 0xf8U) == 0xf0U) {
        count = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    }
    else {
        return 0;
    }
    if (count > length) {
        return 0;
    }
    for (int index = 1; index < count; ++index) {
        if ((source[index] & 0xc0U) != 0x80U) {
            return 0;
        }
        value = (value << 6U) | (source[index] & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) {
        return 0;
    }
    *codepoint = value;
    return count;
}

FcBool FcUtf8Len(
    const FcChar8 *source, int length, int *characters, int *width)
{
    if (source == nullptr || length < 0 || characters == nullptr ||
        width == nullptr) {
        return FcFalse;
    }
    int count = 0;
    int maximum_width = 1;
    for (int offset = 0; offset < length;) {
        FcChar32 codepoint = 0;
        const int consumed = FcUtf8ToUcs4(
            source + offset, &codepoint, length - offset);
        if (consumed == 0) {
            return FcFalse;
        }
        maximum_width = std::max(maximum_width, consumed);
        offset += consumed;
        ++count;
    }
    *characters = count;
    *width = maximum_width;
    return FcTrue;
}

} // extern "C"
