// Content model: every content stream reachable the way veraPDF 1.30.2 reaches it, interpreted once.
//
// Stream instances (veraPDF GFPDContentStream / GFPDSemanticContentStream):
//   Page        page /Contents, semantic (real content)
//   Form        Form XObject invoked by Do; inherits the invoker's semantic flag, marked-content tags and
//               structure parent (veraPDF OperatorParser case DO, GFPDXForm)
//   Annotation  /AP N, D and R appearance streams (all states), never semantic, default language = the
//               annotation's /Lang (GFPDAnnot.addAppearance)
//   Pattern     tiling-pattern content streams, never semantic
//   Type3Glyph  Type 3 CharProcs, never semantic
//
// Semantic streams yield content items (veraPDF SESimpleContentItem); all streams yield glyph events.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/cos.h"
#include "core/lexer.h"
#include "fonts/font.h"
#include "util/chunked_vector.h"

namespace eqx {

class Document;
class StructTree;

struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    // Product this × m (apply this first, then m), the PDF convention for "cm" (new CTM = M × CTM).
    Matrix operator*(const Matrix& m) const {
        return {a * m.a + b * m.c,       a * m.b + b * m.d,       c * m.a + d * m.c,
                c * m.b + d * m.d,       e * m.a + f * m.c + m.e, e * m.b + f * m.d + m.f};
    }
    void apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
};

enum class ColorFamily : std::uint8_t { Unknown, Gray, RGB, CMYK, Pattern };

struct Color {
    ColorFamily family = ColorFamily::Gray;   // DeviceGray black is the initial colour
    float rgb[3] = {0, 0, 0};                 // valid when family is Gray/RGB/CMYK
    bool known() const { return family == ColorFamily::Gray || family == ColorFamily::RGB || family == ColorFamily::CMYK; }
};

struct GState {
    Matrix ctm;
    Obj font_dict;
    const Font* font = nullptr;
    double font_size = 0;
    double tc = 0, tw = 0, th = 1, tl = 0, ts = 0;
    int tr = 0;
    double line_width = 1;
    Color fill, stroke;
    Obj fill_cs, stroke_cs;     // colour space operands (name or array); null = DeviceGray
    double fill_alpha = 1, stroke_alpha = 1;
};

struct StreamInstance {
    enum class Kind : std::uint8_t { Page, Form, Annotation, Pattern, Type3Glyph };
    Kind kind = Kind::Page;
    Obj stream;                    // stream, or the page's /Contents array
    ObjRef ref;                    // reporting object: the stream (page: /Contents object, else the page)
    int page = -1;                 // 0-based page index, -1 when not on a page
    int parent = -1;               // invoking stream instance (Form)
    int parent_frame = -1;         // marked-content frame active at the invoking Do
    std::uint32_t parent_op = 0;   // operator index of the invoking Do
    bool semantic = false;         // veraPDF semantic stream = isRealContent
    Obj parent_struct_elem;        // stream-level structure parent (veraPDF GFPDContentStream.parentStructElem)
    bool inherited_artifact = false;                 // an invoking stream's open marked content is tagged Artifact
    std::optional<std::int64_t> struct_parent;       // StructureElementAccessObject of the stream's owner
    std::optional<std::int64_t> struct_parents;
    std::optional<std::string> default_lang;         // annotation appearance: the annotation's /Lang
    ObjRef annot;                  // Annotation streams and the forms they invoke: the annotation
    Obj annot_dict;
    bool signature_appearance = false;   // appearance of a signature field's widget (and the forms it invokes)
    Matrix base_ctm;               // CTM at the start of the stream
    std::uint32_t op_count = 0;
    std::vector<LexError> errors;  // lexical / syntax problems
    std::string decode_error;      // stream could not be decoded
    bool loop_skipped = false;     // not interpreted: recursive XObject use
    // veraPDF validates every model object once (BaseValidator idSet, id "N G obj <type>"): a content
    // stream object met again with the same semantic kind - a reused Form XObject, a content stream shared
    // by pages - is skipped there with everything nested in it. `repeat` marks such instances (and all
    // instances below them) so rules can reproduce that behaviour in reference-dedup mode.
    bool repeat = false;
};

// One BMC/BDC ... EMC sequence of a stream instance.
struct McFrame {
    int stream = -1;
    int parent = -1;               // enclosing frame in the same stream instance (-1: none)
    int depth = 0;
    bool bdc = false;
    std::string tag;               // tag operand ("" when not a name)
    Obj props;                     // property list from /Properties resources (null for inline dictionaries)
    bool has_props = false;        // a property list (inline or resource) was found
    std::optional<std::int64_t> mcid;            // own /MCID (integer)
    std::optional<std::string> lang, actual_text, alt, e;  // raw string bytes of these keys
    std::optional<std::string> type, subtype;    // /Type, /Subtype names (artifacts)
    std::optional<Rect> artifact_bbox;           // /BBox of an artifact property list
    std::uint32_t begin_op = 0;
    std::uint32_t end_op = 0xffffffffu;          // EMC operator index (unclosed: 0xffffffff)
    Obj own_struct_elem;           // structure element of the own MCID via the ParentTree (null if none)
    Obj struct_elem;               // own_struct_elem, else the enclosing frame's (veraPDF getParentStructElem)
};

struct ContentItem {
    enum class Kind : std::uint8_t { Text, LineArt, Shading, InlineImage, ImageXObject };
    Kind kind = Kind::Text;
    int stream = -1;
    int frame = -1;                // innermost enclosing frame (-1 = unmarked content, veraPDF semantics)
    std::uint32_t op_index = 0;
    Rect bbox;                     // default user space of the page
    bool bbox_valid = false;
    int text = -1;                 // TextRun index for Text items
    ObjRef xobject;                // ImageXObject: the image
    // Painting attributes (LineArt) for colour-dependent checks.
    Color fill;                    // fill colour at the painting operator
    float fill_alpha = 1;
    std::uint8_t paint = 0;        // LineArt: bit 1 = filled, bit 2 = stroked
    bool exact_rect = false;       // LineArt: one axis-aligned rectangle, so bbox is the painted area
};

struct TextRun {
    int item = -1;
    const Font* font = nullptr;
    double size = 0;               // Tf size
    double size_user = 0;          // effective vertical size in user space
    int tr = 0;
    std::u32string text;           // Unicode of the shown codes (codes without mapping are skipped)
    std::uint32_t glyphs = 0;
    std::uint32_t unmapped = 0;    // glyphs without Unicode
    Color fill, stroke;
    double fill_alpha = 1;
    Rect bbox;
};

// A graphic painted by the appearance of a signature field (ISO 14289-2 8.10.3.5).
struct SignatureGraphic {
    int stream = -1;
    int frame = -1;
    const char* what = "";
};

struct GlyphEvent {
    const Font* font = nullptr;
    CharCode code;
    int tr = 0;                    // rendering mode
    int stream = -1;
    int frame = -1;                // innermost frame (as recorded during parsing, also for unclosed frames)
    std::uint32_t op_index = 0;    // text-showing operator
    std::uint32_t glyph_index = 0; // index among the operator's glyphs (veraPDF usedGlyphs[n])
    bool real_content = false;     // the stream is semantic (veraPDF isRealContent)
    double x = 0, y = 0;           // origin in default user space
    double advance_user = 0;       // advance length in user space
    double size_user = 0;          // effective font size in user space
};

class ContentModel;

// Observers receive every glyph and operator while the interpreter runs (no glyph list is stored).
class ContentObserver {
public:
    virtual ~ContentObserver() = default;
    virtual void on_glyph(const ContentModel&, const GlyphEvent&) {}
    virtual void on_operator(const ContentModel&, int /*stream*/, std::uint32_t /*op_index*/, const RawOp&,
                             const GState&) {}
    virtual void on_stream_end(const ContentModel&, int /*stream*/) {}
};

class ContentModel {
public:
    std::vector<StreamInstance> streams;
    std::vector<McFrame> frames;
    ChunkedVector<ContentItem> items;   // append-only: chunked, so growing never copies (no reallocation peak)
    ChunkedVector<TextRun> texts;
    std::vector<SignatureGraphic> signature_graphics;
    bool budget_exceeded = false;   // operator budget hit, or a repeated stream elided: interpretation incomplete
    bool form_depth_exceeded = false;  // no longer set: forms nest without a limit (kept for the engine's check)

    // --- veraPDF content-item semantics ---------------------------------------------------------
    // Structure parent of an item's grouped content (frame's struct_elem, else the stream's).
    Obj item_struct_parent(const ContentItem& it) const;
    // parentsTags of a frame (stream-level inherited tags + tags of the frame chain).
    std::vector<std::string> parents_tags(int stream, int frame) const;   // walks the invoking streams
    bool parents_tags_contain(int stream, int frame, std::string_view tag) const;
    // getLangValue of a frame / unmarked content (own Lang, inherited Lang, stream default language).
    std::optional<std::string> lang_value(const StructTree& st, int stream, int frame) const;
    // Inherited ActualText / Alt along the frame chain (veraPDF getInheritedStringAttribute).
    std::optional<std::string> inherited_actual_text(int frame) const;
    std::optional<std::string> inherited_alt(int frame) const;
    // Inherited MCID of a frame (own, else enclosing frames').
    std::optional<std::int64_t> inherited_mcid(int frame) const;
    // Reporting location of a stream instance.
    int page_of(int stream) const { return stream >= 0 ? streams[stream].page : -1; }
};

struct InterpreterLimits {
    // Beyond either soft limit a stream already read in the same context is not read again, so reused forms (which
    // can multiply exponentially) cannot use up the budget before content not yet seen is reached; the rules over
    // content are then incomplete, but all they found stays reported.
    std::uint64_t soft_operators = 50'000'000;
    std::size_t soft_stream_instances = 1'000'000;
    std::uint64_t max_operators = 200'000'000;   // whole document: interpretation stops, incomplete
};

// Interprets every reachable content stream once and fills `model`, notifying `observers`.
void interpret_document(Document& doc, const StructTree& st, FontRegistry& fonts, ContentModel& model,
                        const std::vector<ContentObserver*>& observers, const InterpreterLimits& limits = {});

}  // namespace eqx
