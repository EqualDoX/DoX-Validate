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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/cos.h"
#include "core/lexer.h"
#include "fonts/font.h"
#include "util/chunked_vector.h"
#include "util/text.h"

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

// An optional string of a marked-content sequence, 8 bytes instead of the 40 of std::optional<std::string>: nearly
// every sequence has none. It reads like the optional it replaces.
class OptString {
public:
    OptString() = default;
    OptString(std::nullopt_t) {}
    OptString(std::string v) : p_(new std::string(std::move(v))) {}
    OptString(std::optional<std::string>&& v) : p_(v ? new std::string(std::move(*v)) : nullptr) {}
    OptString(const std::optional<std::string>& v) : p_(v ? new std::string(*v) : nullptr) {}
    OptString(const OptString& o) : p_(o.p_ ? new std::string(*o.p_) : nullptr) {}
    OptString(OptString&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    OptString& operator=(OptString o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }
    ~OptString() { delete p_; }

    explicit operator bool() const noexcept { return p_ != nullptr; }
    bool has_value() const noexcept { return p_ != nullptr; }
    const std::string& operator*() const { return *p_; }
    const std::string* operator->() const { return p_; }
    const std::string& value() const {
        if (!p_) throw std::bad_optional_access();
        return *p_;
    }
    std::string value_or(std::string fallback) const { return p_ ? *p_ : std::move(fallback); }
    operator std::optional<std::string>() const { return p_ ? std::optional<std::string>(*p_) : std::nullopt; }

private:
    std::string* p_ = nullptr;
};

// One BMC/BDC ... EMC sequence of a stream instance.
struct McFrame {
    int stream = -1;
    int parent = -1;               // enclosing frame in the same stream instance (-1: none)
    std::uint32_t begin_op = 0;
    std::uint32_t end_op = 0xffffffffu;          // EMC operator index (unclosed: 0xffffffff)
    bool bdc = false;
    std::optional<std::int64_t> mcid;            // own /MCID (integer)
    std::string tag;               // tag operand ("" when not a name)
    OptString lang, actual_text, alt, e;         // raw string bytes of these keys
    Obj own_struct_elem;           // structure element of the own MCID via the ParentTree (null if none)
    Obj struct_elem;               // own_struct_elem, else the enclosing frame's (veraPDF getParentStructElem)
};

// Text of a text run: its code points in order. The model stores each run with the narrowest of 1, 2 or 4 bytes
// per code point that holds all of them; this view reads them back unchanged.
class RunText {
public:
    using value_type = char32_t;
    using size_type = std::size_t;

    RunText() = default;
    RunText(const unsigned char* data, std::size_t size, unsigned shift) : p_(data), n_(size), shift_(shift) {}

    bool empty() const { return n_ == 0; }
    std::size_t size() const { return n_; }
    std::size_t length() const { return n_; }
    char32_t operator[](std::size_t i) const { return load(p_ + (i << shift_), shift_); }
    char32_t front() const { return (*this)[0]; }
    char32_t back() const { return (*this)[n_ - 1]; }

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = char32_t;
        using difference_type = std::ptrdiff_t;
        using pointer = const char32_t*;
        using reference = char32_t;

        const_iterator() = default;
        const_iterator(const unsigned char* p, unsigned shift) : p_(p), shift_(shift) {}
        char32_t operator*() const { return load(p_, shift_); }
        const_iterator& operator++() {
            p_ += std::size_t(1) << shift_;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator t = *this;
            ++*this;
            return t;
        }
        bool operator==(const const_iterator& o) const { return p_ == o.p_; }
        bool operator!=(const const_iterator& o) const { return p_ != o.p_; }

    private:
        const unsigned char* p_ = nullptr;
        unsigned shift_ = 0;
    };
    using iterator = const_iterator;
    const_iterator begin() const { return {p_, shift_}; }
    const_iterator end() const { return {p_ + (n_ << shift_), shift_}; }

    void append_to(std::u32string& out) const {
        const std::size_t at = out.size();
        out.resize(at + n_);
        char32_t* o = out.data() + at;
        if (shift_ == 2) {
            if (n_) std::memcpy(o, p_, n_ * 4);
        } else {
            for (std::size_t i = 0; i < n_; ++i) o[i] = load(p_ + (i << shift_), shift_);
        }
    }
    std::u32string str() const {
        std::u32string out;
        append_to(out);
        return out;
    }
    operator std::u32string() const { return str(); }

private:
    static char32_t load(const unsigned char* p, unsigned shift) {
        if (shift == 0) return *p;
        if (shift == 1) {
            std::uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }

    const unsigned char* p_ = nullptr;
    std::size_t n_ = 0;
    unsigned shift_ = 0;   // log2 of the bytes per code point
};

inline std::u32string& operator+=(std::u32string& a, const RunText& b) {
    b.append_to(a);
    return a;
}
inline bool operator==(const RunText& a, std::u32string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < b.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}
// The helpers of util/text.h for a run's text, with the results they have for the equal std::u32string.
inline std::string to_utf8(const RunText& t) {
    std::string out;
    out.reserve(t.size());
    for (char32_t c : t) append_utf8(out, c);
    return out;
}
inline bool is_whitespace_only(const RunText& t) {
    for (char32_t c : t)
        if (!is_unicode_space(c)) return false;
    return true;
}

// Content items and text runs are read as views: `cm.items[i]` and `cm.texts[i]` build them from the compact
// records the model keeps. Bind the result to a const reference or copy it; the view itself has no address in the
// model. Its boxes, colours and text refer to the model, so they stay valid for as long as the model does.
struct ContentItem {
    enum class Kind : std::uint8_t { Text, LineArt, Shading, InlineImage, ImageXObject };
    Kind kind;
    int stream;
    int frame;                     // innermost enclosing frame (-1 = unmarked content, veraPDF semantics)
    std::uint32_t op_index;
    const Rect& bbox;              // default user space of the page
    bool bbox_valid;
    int text;                      // TextRun index for Text items, else -1
    ObjRef xobject;                // ImageXObject: the image
    // Painting attributes (LineArt) for colour-dependent checks.
    const Color& fill;             // fill colour at the painting operator
    float fill_alpha;
    std::uint8_t paint;            // LineArt: bit 1 = filled, bit 2 = stroked
    bool exact_rect;               // LineArt: one axis-aligned rectangle, so bbox is the painted area
};

struct TextRun {
    int item;
    const Font* font;
    double size;                   // Tf size
    double size_user;              // effective vertical size in user space
    int tr;
    RunText text;                  // Unicode of the shown codes (codes without mapping are skipped)
    std::uint32_t glyphs;
    std::uint32_t unmapped;        // glyphs without Unicode
    const Color& fill;
    const Color& stroke;
    double fill_alpha;
    const Rect& bbox;
};

// What the model stores. A text-showing operator costs one ItemRecord and one TextRecord (72 bytes) plus its text.
// The attributes that repeat from run to run (font, sizes, rendering mode, colours, alpha) are kept once per distinct
// combination; the painting attributes of line art and the image of an image item are kept in side tables.
namespace content_detail {

struct ItemRecord {
    Rect bbox;
    std::int32_t stream = -1;
    std::int32_t frame = -1;
    std::uint32_t op_index = 0;
    std::uint32_t aux = 0;         // Text: text run, LineArt: paint record, ImageXObject: image reference
    // Text: the counts of the run - glyphs shown, glyphs without Unicode, code points of the text. A run with a
    // count above kSmallCount has flag bit 4 set and its counts in ContentTexts' table of large runs.
    std::uint16_t glyphs = 0;
    std::uint16_t unmapped = 0;
    std::uint16_t len = 0;
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;        // bit 0 bbox_valid, bits 1-2 paint, bit 3 exact_rect, bit 4 large run, bit 5 has a text run
};
static_assert(sizeof(ItemRecord) == 56);
constexpr std::uint32_t kSmallCount = 0xfffe;

struct TextRecord {
    const unsigned char* text = nullptr;
    std::uint32_t style_shift = 0;   // bits 0-29 style, bits 30-31 log2 of the bytes per code point
    std::uint32_t item = 0;
};
static_assert(sizeof(TextRecord) == 16);

struct RunCounts {
    std::uint32_t glyphs = 0, unmapped = 0, len = 0;
};

struct TextStyle {
    const Font* font = nullptr;
    double size = 0;
    double size_user = 0;
    double fill_alpha = 1;
    Color fill, stroke;
    int tr = 0;
};

struct PaintRecord {
    Color fill;
    float fill_alpha = 1;
};
inline const PaintRecord kNoPaint{};   // what an item that is not line art reads as its painting attributes

}  // namespace content_detail

class ContentItems {
public:
    using value_type = ContentItem;
    using PaintRecord = content_detail::PaintRecord;

    std::size_t size() const { return recs_.size(); }
    bool empty() const { return recs_.empty(); }

    [[gnu::always_inline]] inline ContentItem operator[](std::size_t i) const {   // inlined: only the fields that are read get built
        const content_detail::ItemRecord& r = recs_[i];
        const ContentItem::Kind kind = static_cast<ContentItem::Kind>(r.kind);
        const PaintRecord& p = kind == ContentItem::Kind::LineArt ? paints_[r.aux] : content_detail::kNoPaint;
        return ContentItem{kind,
                           r.stream,
                           r.frame,
                           r.op_index,
                           r.bbox,
                           (r.flags & 1) != 0,
                           (r.flags & 32) != 0 ? static_cast<int>(r.aux) : -1,
                           kind == ContentItem::Kind::ImageXObject ? xobjects_[r.aux] : ObjRef{},
                           p.fill,
                           p.fill_alpha,
                           static_cast<std::uint8_t>((r.flags >> 1) & 3),
                           (r.flags & 8) != 0};
    }
    ContentItem back() const { return (*this)[size() - 1]; }

    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = ContentItem;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = ContentItem;

        const_iterator() = default;
        const_iterator(const ContentItems* v, std::size_t i) : v_(v), i_(i) {}
        ContentItem operator*() const { return (*v_)[i_]; }
        const_iterator& operator++() {
            ++i_;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator t = *this;
            ++i_;
            return t;
        }
        bool operator==(const const_iterator& o) const { return i_ == o.i_; }
        bool operator!=(const const_iterator& o) const { return i_ != o.i_; }

    private:
        const ContentItems* v_ = nullptr;
        std::size_t i_ = 0;
    };
    using iterator = const_iterator;
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size()}; }

    // --- for the interpreter ---
    // A LineArt item must get its painting attributes with set_paint, an ImageXObject item its image with
    // set_xobject, before the item is read.
    int add(ContentItem::Kind kind, int stream, int frame, std::uint32_t op_index, const Rect* bbox) {
        content_detail::ItemRecord& r = recs_.emplace_back();
        r.kind = static_cast<std::uint8_t>(kind);
        r.stream = stream;
        r.frame = frame;
        r.op_index = op_index;
        if (bbox) {
            r.bbox = *bbox;
            r.flags = 1;
        }
        return static_cast<int>(recs_.size()) - 1;
    }
    void set_paint(int item, std::uint8_t paint, const Color& fill, float fill_alpha, bool exact_rect) {
        content_detail::ItemRecord& r = recs_[static_cast<std::size_t>(item)];
        r.flags = static_cast<std::uint8_t>((r.flags & 1) | ((paint & 3) << 1) | (exact_rect ? 8 : 0));
        r.aux = paint_index(PaintRecord{fill, fill_alpha});
    }
    void set_xobject(int item, ObjRef ref) {
        recs_[static_cast<std::size_t>(item)].aux = static_cast<std::uint32_t>(xobjects_.size());
        xobjects_.push_back(ref);
    }
    content_detail::ItemRecord& record(std::size_t i) { return recs_[i]; }
    const content_detail::ItemRecord& record(std::size_t i) const { return recs_[i]; }

private:
    // Consecutive paths mostly share their fill: a paint record equal to the last one is not stored again.
    std::uint32_t paint_index(const PaintRecord& p) {
        if (!paints_.empty()) {
            const PaintRecord& l = paints_.back();
            if (l.fill.family == p.fill.family && std::memcmp(l.fill.rgb, p.fill.rgb, sizeof p.fill.rgb) == 0 &&
                std::memcmp(&l.fill_alpha, &p.fill_alpha, sizeof p.fill_alpha) == 0)
                return static_cast<std::uint32_t>(paints_.size() - 1);
        }
        paints_.push_back(p);
        return static_cast<std::uint32_t>(paints_.size() - 1);
    }

    ChunkedVector<content_detail::ItemRecord> recs_;   // append-only: chunked, so growing never copies
    ChunkedVector<PaintRecord, 256> paints_;
    ChunkedVector<ObjRef, 256> xobjects_;
};

class ContentTexts {
public:
    using value_type = TextRun;

    ContentTexts() = default;
    ContentTexts(const ContentTexts&) = delete;
    ContentTexts& operator=(const ContentTexts&) = delete;
    ~ContentTexts();

    std::size_t size() const { return recs_.size(); }
    bool empty() const { return recs_.empty(); }

    [[gnu::always_inline]] inline TextRun operator[](std::size_t i) const {
        const content_detail::TextRecord& r = recs_[i];
        const content_detail::TextStyle& s = styles_[r.style_shift & kStyleMask];
        const content_detail::ItemRecord& it = items_->record(r.item);
        content_detail::RunCounts n{it.glyphs, it.unmapped, it.len};
        if (it.flags & 16) [[unlikely]] n = large_run(i);
        return TextRun{static_cast<int>(r.item),
                       s.font,
                       s.size,
                       s.size_user,
                       s.tr,
                       RunText(r.text, n.len, r.style_shift >> 30),
                       n.glyphs,
                       n.unmapped,
                       s.fill,
                       s.stroke,
                       s.fill_alpha,
                       it.bbox};
    }
    TextRun back() const { return (*this)[size() - 1]; }

    class const_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = TextRun;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = TextRun;

        const_iterator() = default;
        const_iterator(const ContentTexts* v, std::size_t i) : v_(v), i_(i) {}
        TextRun operator*() const { return (*v_)[i_]; }
        const_iterator& operator++() {
            ++i_;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator t = *this;
            ++i_;
            return t;
        }
        bool operator==(const const_iterator& o) const { return i_ == o.i_; }
        bool operator!=(const const_iterator& o) const { return i_ != o.i_; }

    private:
        const ContentTexts* v_ = nullptr;
        std::size_t i_ = 0;
    };
    using iterator = const_iterator;
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size()}; }

    // --- for the interpreter ---
    void attach(ContentItems& items) { items_ = &items; }
    // Opens the run of text item `item`: append() adds to its text, end_run() stores the text and the counts of the
    // run. Until end_run() the run reads as empty, so end_run() must also be called when the operator is abandoned
    // (it does not throw).
    void begin_run(int item, const content_detail::TextStyle& style);
    [[gnu::always_inline]] inline void append(const char32_t* s, std::size_t n) {
        if (n == 1 && *s < 0x100 && open_shift_ == 0 && end_ != cap_) [[likely]] {   // one narrow code point: nearly every glyph
            *end_++ = static_cast<unsigned char>(*s);
            ++open_len_;
            return;
        }
        append_any(s, n);
    }
    void end_run(std::uint32_t glyphs, std::uint32_t unmapped) noexcept;

private:
    static constexpr std::uint32_t kStyleMask = (std::uint32_t(1) << 30) - 1;
    void append_any(const char32_t* s, std::size_t n);
    void make_room(std::size_t n, unsigned shift);
    std::uint32_t intern(const content_detail::TextStyle& style);
    const content_detail::RunCounts& large_run(std::size_t run) const;

    ChunkedVector<content_detail::TextRecord> recs_;
    ChunkedVector<content_detail::TextStyle, 64> styles_;
    std::unordered_multimap<std::uint64_t, std::uint32_t> style_index_;   // hash of a style -> its index
    std::uint32_t last_style_ = 0;
    ContentItems* items_ = nullptr;
    // Text arena: blocks of bytes. The open run is the tail of the last block.
    std::vector<unsigned char*> blocks_;
    unsigned char* open_text_ = nullptr;           // the text of the open run
    std::size_t open_len_ = 0;                     // its code points
    unsigned open_shift_ = 0;                      // log2 of its bytes per code point
    bool open_ = false;                            // a run is open
    bool open_alone_ = false;                      // the open run is all that its block holds
    unsigned char* end_ = nullptr;                 // write position: the end of the open run
    unsigned char* cap_ = nullptr;                 // end of the last block
    // The counts of the runs with more than kSmallCount glyphs or code points, in the order of the runs. begin_run
    // keeps room for one more, so end_run never allocates.
    std::vector<std::pair<std::uint32_t, content_detail::RunCounts>> large_;
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
// The callbacks of this class must stay empty: the interpreter does not call a callback on an observer whose class
// does not override it.
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
    ContentModel() { texts.attach(items); }
    ContentModel(const ContentModel&) = delete;
    ContentModel& operator=(const ContentModel&) = delete;

    std::vector<StreamInstance> streams;
    ChunkedVector<McFrame, 256> frames;   // append-only: chunked, so growing never copies (no reallocation peak)
    ContentItems items;
    ContentTexts texts;
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
