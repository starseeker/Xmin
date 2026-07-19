#ifndef XMIN_CLIENT_FONTCONFIG_H
#define XMIN_CLIENT_FONTCONFIG_H

/*
 * Focused fontconfig-compatible declarations for Xmin's deterministic,
 * embedded-font catalog.  This is not a host font discovery interface.
 */

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char FcChar8;
typedef unsigned short FcChar16;
typedef unsigned int FcChar32;
typedef int FcBool;

#define FcFalse 0
#define FcTrue 1

#define FC_FAMILY "family"
#define FC_STYLE "style"
#define FC_SLANT "slant"
#define FC_WEIGHT "weight"
#define FC_SIZE "size"
#define FC_PIXEL_SIZE "pixelsize"
#define FC_SPACING "spacing"
#define FC_FOUNDRY "foundry"
#define FC_ANTIALIAS "antialias"
#define FC_HINTING "hinting"
#define FC_FILE "file"
#define FC_INDEX "index"
#define FC_RASTERIZER "rasterizer"
#define FC_OUTLINE "outline"
#define FC_SCALABLE "scalable"
#define FC_RGBA "rgba"
#define FC_SCALE "scale"
#define FC_MINSPACE "minspace"
#define FC_DPI "dpi"
#define FC_CHAR_WIDTH "charwidth"
#define FC_CHAR_HEIGHT "charheight"
#define FC_MATRIX "matrix"
#define FC_CHARSET "charset"

#define FC_WEIGHT_LIGHT 50
#define FC_WEIGHT_MEDIUM 100
#define FC_WEIGHT_DEMIBOLD 180
#define FC_WEIGHT_BOLD 200
#define FC_WEIGHT_BLACK 210
#define FC_WEIGHT_REGULAR 80
#define FC_WEIGHT_NORMAL FC_WEIGHT_REGULAR

#define FC_SLANT_ROMAN 0
#define FC_SLANT_ITALIC 100
#define FC_SLANT_OBLIQUE 110

#define FC_PROPORTIONAL 0
#define FC_MONO 100
#define FC_CHARCELL 110

#define FC_RGBA_UNKNOWN 0
#define FC_RGBA_RGB 1
#define FC_RGBA_BGR 2
#define FC_RGBA_VRGB 3
#define FC_RGBA_VBGR 4
#define FC_RGBA_NONE 5

typedef enum _FcType {
    FcTypeUnknown = -1,
    FcTypeVoid,
    FcTypeInteger,
    FcTypeDouble,
    FcTypeString,
    FcTypeBool,
    FcTypeMatrix,
    FcTypeCharSet,
} FcType;

typedef struct _FcMatrix {
    double xx, xy, yx, yy;
} FcMatrix;

typedef struct _FcCharSet FcCharSet;
typedef struct _FcPattern FcPattern;
typedef struct _FcConfig FcConfig;

typedef enum _FcResult {
    FcResultMatch,
    FcResultNoMatch,
    FcResultTypeMismatch,
    FcResultNoId,
    FcResultOutOfMemory,
} FcResult;

typedef enum _FcMatchKind {
    FcMatchPattern,
    FcMatchFont,
    FcMatchScan,
} FcMatchKind;

typedef enum _FcEndian {
    FcEndianBig,
    FcEndianLittle,
} FcEndian;

typedef struct _FcValue {
    FcType type;
    union {
        const FcChar8 *s;
        int i;
        FcBool b;
        double d;
        const FcMatrix *m;
        const FcCharSet *c;
        const void *f;
    } u;
} FcValue;

typedef struct _FcFontSet {
    int nfont;
    int sfont;
    FcPattern **fonts;
} FcFontSet;

typedef struct _FcObjectSet {
    int nobject;
    int sobject;
    char **objects;
} FcObjectSet;

FcBool FcInit(void);

FcPattern *FcPatternCreate(void);
FcPattern *FcPatternDuplicate(const FcPattern *pattern);
void FcPatternDestroy(FcPattern *pattern);
FcBool FcPatternDel(FcPattern *pattern, const char *object);
FcBool FcPatternAddInteger(FcPattern *pattern, const char *object, int value);
FcBool FcPatternAddDouble(FcPattern *pattern, const char *object, double value);
FcBool FcPatternAddString(
    FcPattern *pattern, const char *object, const FcChar8 *value);
FcBool FcPatternAddBool(FcPattern *pattern, const char *object, FcBool value);
FcBool FcPatternAddMatrix(
    FcPattern *pattern, const char *object, const FcMatrix *value);
FcBool FcPatternAddCharSet(
    FcPattern *pattern, const char *object, const FcCharSet *value);
FcResult FcPatternGetInteger(
    const FcPattern *pattern, const char *object, int id, int *value);
FcResult FcPatternGetDouble(
    const FcPattern *pattern, const char *object, int id, double *value);
FcResult FcPatternGetString(
    const FcPattern *pattern, const char *object, int id, FcChar8 **value);
FcResult FcPatternGetBool(
    const FcPattern *pattern, const char *object, int id, FcBool *value);
FcResult FcPatternGetMatrix(
    const FcPattern *pattern, const char *object, int id, FcMatrix **value);
FcResult FcPatternGetCharSet(
    const FcPattern *pattern, const char *object, int id, FcCharSet **value);

FcPattern *FcNameParse(const FcChar8 *name);
FcChar8 *FcNameUnparse(FcPattern *pattern);

FcObjectSet *FcObjectSetCreate(void);
FcBool FcObjectSetAdd(FcObjectSet *set, const char *object);
void FcObjectSetDestroy(FcObjectSet *set);
FcObjectSet *FcObjectSetVaBuild(const char *first, va_list arguments);
FcObjectSet *FcObjectSetBuild(const char *first, ...);

FcFontSet *FcFontList(
    FcConfig *config, FcPattern *pattern, FcObjectSet *objects);
FcFontSet *FcFontSort(
    FcConfig *config, FcPattern *pattern, FcBool trim,
    FcCharSet **coverage, FcResult *result);
FcPattern *FcFontRenderPrepare(
    FcConfig *config, FcPattern *pattern, FcPattern *font);
FcPattern *FcFontSetMatch(
    FcConfig *config, FcFontSet **sets, int set_count,
    FcPattern *pattern, FcResult *result);
void FcFontSetDestroy(FcFontSet *set);
FcPattern *FcFontMatch(
    FcConfig *config, FcPattern *pattern, FcResult *result);
void FcDefaultSubstitute(FcPattern *pattern);

FcCharSet *FcCharSetCreate(void);
FcCharSet *FcCharSetCopy(FcCharSet *source);
void FcCharSetDestroy(FcCharSet *charset);
FcBool FcCharSetAddChar(FcCharSet *charset, FcChar32 codepoint);
FcBool FcCharSetHasChar(const FcCharSet *charset, FcChar32 codepoint);

FcBool FcConfigSubstitute(
    FcConfig *config, FcPattern *pattern, FcMatchKind kind);

void FcMatrixInit(FcMatrix *matrix);
FcBool FcMatrixEqual(const FcMatrix *left, const FcMatrix *right);
void FcMatrixMultiply(
    FcMatrix *result, const FcMatrix *left, const FcMatrix *right);
void FcMatrixRotate(FcMatrix *result, double cosine, double sine);
void FcMatrixScale(FcMatrix *result, double x, double y);
void FcMatrixShear(FcMatrix *result, double x, double y);

int FcUtf8ToUcs4(const FcChar8 *source, FcChar32 *codepoint, int length);
FcBool FcUtf8Len(
    const FcChar8 *source, int length, int *characters, int *width);

#ifdef __cplusplus
}
#endif

#endif
