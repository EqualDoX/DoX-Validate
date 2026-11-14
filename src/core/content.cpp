#include "core/content.h"

#include <qpdf/Buffer.hh>
#include <qpdf/Pipeline.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <zlib.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "core/document.h"
#include "core/structure.h"
#include "util/text.h"

namespace eqx {

// --- Text runs --------------------------------------------------------------------------------------

namespace {

using content_detail::TextRecord;
using content_detail::TextStyle;

constexpr std::size_t kTextBlock = 64 * 1024;

// Two styles are the same only when every value has the same bits (0.0 and -0.0 print differently).
std::uint32_t float_bits(float v) { return std::bit_cast<std::uint32_t>(v); }
std::uint64_t double_bits(double v) { return std::bit_cast<std::uint64_t>(v); }

bool same_color(const Color& a, const Color& b) {
    return a.family == b.family && float_bits(a.rgb[0]) == float_bits(b.rgb[0]) &&
           float_bits(a.rgb[1]) == float_bits(b.rgb[1]) && float_bits(a.rgb[2]) == float_bits(b.rgb[2]);
}

bool same_style(const TextStyle& a, const TextStyle& b) {
    return a.font == b.font && a.tr == b.tr && double_bits(a.size) == double_bits(b.size) &&
           double_bits(a.size_user) == double_bits(b.size_user) && double_bits(a.fill_alpha) == double_bits(b.fill_alpha) &&
           same_color(a.fill, b.fill) && same_color(a.stroke, b.stroke);
}

std::uint64_t style_hash(const TextStyle& s) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    const auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    };
    mix(reinterpret_cast<std::uintptr_t>(s.font));
    mix(double_bits(s.size));
    mix(double_bits(s.size_user));
    mix(double_bits(s.fill_alpha));
    mix((std::uint64_t(float_bits(s.fill.rgb[0])) << 32) | float_bits(s.fill.rgb[1]));
    mix((std::uint64_t(float_bits(s.fill.rgb[2])) << 32) | float_bits(s.stroke.rgb[0]));
    mix((std::uint64_t(float_bits(s.stroke.rgb[1])) << 32) | float_bits(s.stroke.rgb[2]));
    mix((std::uint64_t(static_cast<std::uint8_t>(s.fill.family)) << 40) |
        (std::uint64_t(static_cast<std::uint8_t>(s.stroke.family)) << 32) | static_cast<std::uint32_t>(s.tr));
    return h;
}

char32_t load_code(const unsigned char* p, unsigned shift) {
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

void store_code(unsigned char* p, unsigned shift, char32_t c) {
    if (shift == 0) {
        *p = static_cast<unsigned char>(c);
    } else if (shift == 1) {
        const std::uint16_t v = static_cast<std::uint16_t>(c);
        std::memcpy(p, &v, 2);
    } else {
        const std::uint32_t v = c;
        std::memcpy(p, &v, 4);
    }
}

}  // namespace

ContentTexts::~ContentTexts() {
    for (unsigned char* b : blocks_) delete[] b;
}

std::uint32_t ContentTexts::intern(const TextStyle& style) {
    if (!styles_.empty() && same_style(styles_[last_style_], style)) return last_style_;
    const std::uint64_t h = style_hash(style);
    const auto range = style_index_.equal_range(h);
    for (auto it = range.first; it != range.second; ++it)
        if (same_style(styles_[it->second], style)) return last_style_ = it->second;
    if (styles_.size() >= kStyleMask) throw std::length_error("content model: too many text styles");
    const std::uint32_t index = static_cast<std::uint32_t>(styles_.size());
    style_index_.emplace(h, index);
    styles_.push_back(style);
    return last_style_ = index;
}

TextRecord& ContentTexts::begin_run(int item, const TextStyle& style) {
    const std::uint32_t style_index = intern(style);
    TextRecord& r = recs_.emplace_back();
    r.text = end_;
    r.style_shift = style_index;
    r.item = static_cast<std::uint32_t>(item);
    items_->record(static_cast<std::size_t>(item)).aux = static_cast<std::uint32_t>(recs_.size() - 1);
    open_ = &r;
    open_shift_ = 0;
    open_alone_ = false;
    return r;
}

// The open run needs room for `n` more code points of width `shift` (log2 bytes): widens what it holds when the new
// code points are wider, in place when the block has the room, else in a new block.
void ContentTexts::make_room(std::size_t n, unsigned shift) {
    TextRecord& r = *open_;
    const unsigned new_shift = std::max(shift, open_shift_);
    const std::size_t have = r.len;
    if (n > 0xffffffffu - have) throw std::length_error("content model: text of one operator too long");
    const std::size_t need = (have + n) << new_shift;
    unsigned char* from = const_cast<unsigned char*>(r.text);
    unsigned char* to = from;
    if (from == nullptr || static_cast<std::size_t>(cap_ - from) < need) {
        const std::size_t size = std::max(kTextBlock, need + need / 2);
        blocks_.reserve(blocks_.size() + 1);
        to = new unsigned char[size];
        cap_ = to + size;
    }
    if (new_shift == open_shift_) {
        if (to != from && have) std::memcpy(to, from, have << new_shift);
    } else {
        // Backwards, so that widening in place never overwrites a code point not yet read.
        for (std::size_t i = have; i-- > 0;) store_code(to + (i << new_shift), new_shift, load_code(from + (i << open_shift_), open_shift_));
    }
    if (to != from) {
        if (open_alone_) {
            delete[] blocks_.back();
            blocks_.back() = to;
        } else {
            blocks_.push_back(to);
        }
        open_alone_ = true;
    }
    r.text = to;
    r.style_shift = (r.style_shift & kStyleMask) | (new_shift << 30);
    open_shift_ = new_shift;
    end_ = to + (have << new_shift);
}

// --- ContentModel helpers ---------------------------------------------------------------------------

Obj ContentModel::item_struct_parent(const ContentItem& it) const {
    if (it.frame >= 0) {
        const Obj& se = frames[it.frame].struct_elem;
        if (se.isDictionary()) return se;
    }
    return streams[it.stream].parent_struct_elem;
}

std::vector<std::string> ContentModel::parents_tags(int stream, int frame) const {
    std::vector<std::string> out;
    for (int s = stream, f = frame; s >= 0; f = streams[s].parent_frame, s = streams[s].parent)
        for (; f >= 0; f = frames[f].parent) out.push_back(frames[f].tag);
    std::reverse(out.begin(), out.end());
    return out;
}

bool ContentModel::parents_tags_contain(int stream, int frame, std::string_view tag) const {
    for (int f = frame; f >= 0; f = frames[f].parent)
        if (frames[f].tag == tag) return true;
    // The tags inherited from the invoking streams. "Artifact" is what the rules ask for and is kept per stream, so
    // that a query costs nothing on deeply nested forms.
    if (tag == "Artifact") return streams[stream].inherited_artifact;
    for (int s = stream; streams[s].parent >= 0; s = streams[s].parent)
        for (int f = streams[s].parent_frame; f >= 0; f = frames[f].parent)
            if (frames[f].tag == tag) return true;
    return false;
}

std::optional<std::string> ContentModel::lang_value(const StructTree& st, int stream, int frame) const {
    // GFSEMarkedContent.getLangValue: own Lang, else inherited Lang (structure parent of the own MCID,
    // then the enclosing frames), else the stream's default language.
    for (int f = frame; f >= 0; f = frames[f].parent) {
        const McFrame& fr = frames[f];
        if (fr.lang) return pdf_text_to_utf8(*fr.lang);
        if (fr.bdc && fr.own_struct_elem.isDictionary()) {
            Obj lang = cos::get(fr.own_struct_elem, "Lang");
            if (lang.isString()) return cos::text(lang);
            if (auto pl = st.parent_lang(fr.own_struct_elem)) return pl;
        }
    }
    return streams[stream].default_lang;
}

std::optional<std::string> ContentModel::inherited_actual_text(int frame) const {
    for (int f = frame; f >= 0; f = frames[f].parent)
        if (frames[f].actual_text) return frames[f].actual_text;
    return std::nullopt;
}

std::optional<std::string> ContentModel::inherited_alt(int frame) const {
    for (int f = frame; f >= 0; f = frames[f].parent)
        if (frames[f].alt) return frames[f].alt;
    return std::nullopt;
}

std::optional<std::int64_t> ContentModel::inherited_mcid(int frame) const {
    for (int f = frame; f >= 0; f = frames[f].parent)
        if (frames[f].mcid) return frames[f].mcid;
    return std::nullopt;
}

// --- Interpreter ------------------------------------------------------------------------------------

namespace {

// PDResourcesHandler: the stream's own resources, then those of the page (a form never searches the forms that
// invoke it first: veraPDF, and ISO 32000-1 7.8.3 for a form without /Resources). A name defined in neither is then
// looked up in the invoking streams, where no reference tool looks but viewers do: such a font is checked as well.
struct Resources {
    Obj own;
    Obj page;
    const Resources* outer = nullptr;
    static Obj find(const Obj& r, std::string_view category, std::string_view name) {
        Obj cat = cos::get(r, category);
        if (!cat.isDictionary()) return Obj::newNull();
        return cos::get(cat, name);
    }
    Obj lookup(std::string_view category, std::string_view name) const {
        Obj v = find(own, category, name);
        if (!v.isNull()) return v;
        v = find(page, category, name);
        if (!v.isNull()) return v;
        for (const Resources* r = outer; r; r = r->outer) {
            v = find(r->own, category, name);
            if (!v.isNull()) return v;
        }
        return Obj::newNull();
    }
};

Obj resources_of(const Obj& stream_or_dict) {
    Obj r = cos::get(stream_or_dict, "Resources");
    return r.isDictionary() ? r : Obj::newNull();
}

// Everything a stream being interpreted needs between two operators. Held on the heap: forms nest without using the
// call stack, so nesting has no limit but memory (veraPDF has none either) and is safe at any stack size.
struct Exec {
    int si = -1;
    Resources res;
    GState gs;
    int depth = 0;
    std::string data;
    std::unique_ptr<ContentParser> parser;
    bool active_key = false;
    std::vector<GState> gstack;
    std::vector<int> fstack;     // open frames (indices into ContentModel::frames)
    Matrix tm, tlm;
    bool have_path = false;
    int path_rects = 0;          // "re" sub-paths of the current path
    bool path_other = false;     // any other path construction operator
    bool path_curve = false;     // a curve segment
    double px0 = 0, py0 = 0, px1 = 0, py1 = 0;   // path bbox in user space
    double cx = 0, cy = 0;       // current point (user space)
    std::uint32_t op_index = 0;
    std::size_t first_item = 0;
};

double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// hypot of the last arguments it was called with, kept: the text matrix of consecutive text-showing operators
// mostly differs in its translation only, and the function returns the same value for the same arguments.
struct HypotMemo {
    double x = 0, y = 0, value = 0;
    double operator()(double a, double b) {
        if (a == x && b == y) return value;
        x = a;
        y = b;
        return value = std::hypot(a, b);
    }
};

// Whether an observer's class implements a callback, that is overrides the empty one of ContentObserver. The
// interpreter calls a callback only on the observers that implement it: per glyph and per operator, the calls that
// would return at once are most of the dispatch. Decided from the virtual table with GCC's conversion of a bound
// member function; another compiler takes every observer to implement every callback, which is as correct.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpmf-conversions"
bool implements_on_glyph(ContentObserver* o) {
    using Fn = void (*)(ContentObserver*, const ContentModel&, const GlyphEvent&);
    return (Fn)(o->*(&ContentObserver::on_glyph)) != (Fn)(&ContentObserver::on_glyph);
}
bool implements_on_operator(ContentObserver* o) {
    using Fn = void (*)(ContentObserver*, const ContentModel&, int, std::uint32_t, const RawOp&, const GState&);
    return (Fn)(o->*(&ContentObserver::on_operator)) != (Fn)(&ContentObserver::on_operator);
}
bool implements_on_stream_end(ContentObserver* o) {
    using Fn = void (*)(ContentObserver*, const ContentModel&, int);
    return (Fn)(o->*(&ContentObserver::on_stream_end)) != (Fn)(&ContentObserver::on_stream_end);
}
#pragma GCC diagnostic pop
#else
bool implements_on_glyph(ContentObserver*) { return true; }
bool implements_on_operator(ContentObserver*) { return true; }
bool implements_on_stream_end(ContentObserver*) { return true; }
#endif

// The bytes of a buffer that qpdf filled for this caller alone: taken over where qpdf can hand them out
// (Buffer::move), copied otherwise.
template <class B>
std::string take_bytes(B& buf) {
    if constexpr (requires { std::string(buf.move()); }) {
        return buf.move();
    } else {
        return std::string(reinterpret_cast<const char*>(buf.getBuffer()), buf.getSize());
    }
}

// cos::stream_data for a content stream: the same decoding (specialized level, so RunLengthDecode too) and the same
// error, without copying the decoded stream out of qpdf's buffer.
std::optional<std::string> content_stream_data(const Obj& stream, std::string* error) {
    if (!stream.isStream()) return std::nullopt;
    try {
        auto buf = const_cast<Obj&>(stream).getStreamData(qpdf_dl_specialized);
        return take_bytes(*buf);
    } catch (std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

class Interpreter {
public:
    Interpreter(Document& doc, const StructTree& st, FontRegistry& fonts, ContentModel& m,
                const std::vector<ContentObserver*>& obs, const InterpreterLimits& lim)
        : doc_(doc), st_(st), fonts_(fonts), m_(m), lim_(lim) {
        // in the order of `obs`, so every callback reaches the observers in the order it always did
        for (ContentObserver* o : obs) {
            if (implements_on_glyph(o)) glyph_obs_.push_back(o);
            if (implements_on_operator(o)) op_obs_.push_back(o);
            if (implements_on_stream_end(o)) end_obs_.push_back(o);
        }
    }

    void run();

private:
    int add_stream(StreamInstance si) {
        if (si.parent >= 0 && m_.streams[si.parent].repeat) si.repeat = true;
        if (si.ref.valid() && (si.stream.isStream() || si.stream.isArray())) {
            std::uint64_t k = (si.ref.key() << 1) | (si.semantic ? 1u : 0u);
            if (!seen_streams_.insert(k).second) si.repeat = true;
        }
        m_.streams.push_back(std::move(si));
        return static_cast<int>(m_.streams.size()) - 1;
    }
    std::unordered_set<std::uint64_t> seen_streams_;
    void interpret(int si, const Resources& res, const GState& gs, int depth);   // runs the stream and all it invokes
    void push(int si, const Resources& res, const GState& gs, int depth);        // queues a stream on the heap stack
    bool step(Exec& x);    // runs operators; true when the stream ended, false when a nested stream was queued
    void finish(Exec& x);
    void drain(std::size_t base);
    // A stream met again in the same context is read once the budget for repeated work is used up.
    bool admit(const ObjRef& ref, std::uint64_t context);
    bool stream_bytes(StreamInstance& s, std::string& out);
    Color make_color(const Obj& cs, const std::vector<double>& comps, const Resources& res, int depth = 0);
    Color initial_color(const Obj& cs, const Resources& res);
    Obj resolve_cs(const Obj& cs, const Resources& res);
    void do_xobject(int si, std::uint32_t op_index, int frame, const std::string& name, const Resources& res,
                    const GState& gs, int depth);
    void run_pattern(int si, const Obj& pattern, const Resources& res, const GState& gs, int depth);
    void run_type3_glyphs();
    void run_annotations(const Page& page);
    struct Type3Use {
        Obj page_resources;
        GState gs;
    };

    Document& doc_;
    const StructTree& st_;
    FontRegistry& fonts_;
    ContentModel& m_;
    std::vector<ContentObserver*> glyph_obs_, op_obs_, end_obs_;   // the observers implementing each callback
    InterpreterLimits lim_;
    std::uint64_t ops_total_ = 0;
    std::unordered_set<std::uint64_t> active_;          // streams being interpreted (recursion check)
    std::unordered_set<std::uint64_t> patterns_done_;
    std::unordered_set<const Font*> type3_done_;
    std::unordered_map<const Font*, Type3Use> type3_used_;   // GFPDType3Font: built at the font's first text operator
    std::unordered_set<std::uint64_t> contexts_seen_;
    std::vector<std::unique_ptr<Exec>> exec_;
    std::vector<CharCode> codes_;
    std::vector<double> comps_;
    bool hard_stop_ = false;
    const Obj null_ = Obj::newNull();
    HypotMemo hypot_cd_, hypot_ab_, hypot_trm_;
};

bool Interpreter::stream_bytes(StreamInstance& s, std::string& out) {
    // An undecodable stream keeps decode_error (the rules over content stay incomplete); the prefix its filters
    // deliver is still read, so a violation in the readable part is reported.
    auto read_one = [&](const Obj& part, std::string& to) {
        std::string err;
        // A Flate stream whose data simply ends early decodes here with a warning (qpdf: "input stream is complete but
        // output may still be valid", about 1.5 % of real documents): no byte of the file stays unread.
        auto d = content_stream_data(part, &err);
        if (d) {
            // Decoded, but the data is not what was written: read it, and the verdict stays open.
            if (cos::flate_checksum_failed(part, *d)) s.decode_error = "Flate checksum of the content stream failed";
            to = std::move(*d);
            return;
        }
        s.decode_error = err.empty() ? "stream cannot be decoded" : err;
        to = cos::decodable_prefix(part);
    };
    if (s.stream.isStream()) {
        read_one(s.stream, out);
        return true;
    }
    if (s.stream.isArray()) {
        for (auto& part : cos::array_items(s.stream)) {
            if (!part.isStream()) continue;
            std::string one;
            read_one(part, one);
            if (out.empty()) out = std::move(one);   // the first part (often the only one) is not copied
            else out.append(one);
            out.push_back('\n');
        }
        return true;
    }
    return false;
}

bool Interpreter::admit(const ObjRef& ref, std::uint64_t context) {
    if (!ref.valid()) return true;
    const std::uint64_t key = (ref.key() * 0x9E3779B97F4A7C15ull) ^ context;
    const bool fresh = contexts_seen_.insert(key).second;
    if (fresh || (ops_total_ <= lim_.soft_operators && m_.streams.size() <= lim_.soft_stream_instances)) return true;
    m_.budget_exceeded = true;
    return false;
}

Obj Interpreter::resolve_cs(const Obj& cs, const Resources& res) {
    if (cs.isName()) {
        std::string n = *cos::name(cs);
        if (n == "DeviceGray" || n == "DeviceRGB" || n == "DeviceCMYK" || n == "Pattern" || n == "G" || n == "RGB" ||
            n == "CMYK" || n == "I")
            return cs;
        Obj r = res.lookup("ColorSpace", n);
        if (!r.isNull()) return r;
    }
    return cs;
}

Color Interpreter::make_color(const Obj& cs_in, const std::vector<double>& c, const Resources& res, int depth) {
    Color out;
    out.family = ColorFamily::Unknown;
    if (depth > 8) return out;
    Obj cs = resolve_cs(cs_in, res);
    auto set_gray = [&](double g) {
        out.family = ColorFamily::Gray;
        out.rgb[0] = out.rgb[1] = out.rgb[2] = static_cast<float>(clamp01(g));
    };
    auto set_rgb = [&](double r, double g, double b) {
        out.family = ColorFamily::RGB;
        out.rgb[0] = static_cast<float>(clamp01(r));
        out.rgb[1] = static_cast<float>(clamp01(g));
        out.rgb[2] = static_cast<float>(clamp01(b));
    };
    auto set_cmyk = [&](double C, double M, double Y, double K) {
        out.family = ColorFamily::CMYK;
        out.rgb[0] = static_cast<float>(1 - std::min(1.0, clamp01(C) + clamp01(K)));
        out.rgb[1] = static_cast<float>(1 - std::min(1.0, clamp01(M) + clamp01(K)));
        out.rgb[2] = static_cast<float>(1 - std::min(1.0, clamp01(Y) + clamp01(K)));
    };
    auto by_n = [&](std::size_t n) {
        if (n == 1 && c.size() >= 1) set_gray(c[0]);
        else if (n == 3 && c.size() >= 3) set_rgb(c[0], c[1], c[2]);
        else if (n == 4 && c.size() >= 4) set_cmyk(c[0], c[1], c[2], c[3]);
    };
    std::string family;
    Obj arr;
    if (cs.isNull()) family = "DeviceGray";
    else if (cs.isName()) family = *cos::name(cs);
    else if (cs.isArray() && cs.getArrayNItems() > 0) {
        arr = cs;
        family = cos::name(cs.getArrayItem(0)).value_or("");
    }
    if (family == "DeviceGray" || family == "G" || family == "CalGray") by_n(1);
    else if (family == "DeviceRGB" || family == "RGB" || family == "CalRGB") by_n(3);
    else if (family == "DeviceCMYK" || family == "CMYK") by_n(4);
    else if (family == "ICCBased" && arr.isArray() && arr.getArrayNItems() > 1) {
        auto n = cos::integer(arr.getArrayItem(1), "N");
        if (n) by_n(static_cast<std::size_t>(*n));
    } else if ((family == "Indexed" || family == "I") && arr.isArray() && arr.getArrayNItems() >= 4 && !c.empty()) {
        Obj base = arr.getArrayItem(1);
        Obj lookup = arr.getArrayItem(3);
        std::string table;
        if (lookup.isString()) table = lookup.getStringValue();
        else if (lookup.isStream()) table = cos::stream_data(lookup).value_or("");
        Color bc = make_color(base, {}, res, depth + 1);   // probe the base family
        Obj base_res = resolve_cs(base, res);
        std::size_t n = 0;
        auto bf = base_res.isName() ? cos::name(base_res).value_or("")
                  : base_res.isArray() && base_res.getArrayNItems() ? cos::name(base_res.getArrayItem(0)).value_or("")
                                                                     : "";
        if (bf == "DeviceGray" || bf == "G" || bf == "CalGray") n = 1;
        else if (bf == "DeviceRGB" || bf == "RGB" || bf == "CalRGB") n = 3;
        else if (bf == "DeviceCMYK" || bf == "CMYK") n = 4;
        else if (bf == "ICCBased" && base_res.isArray() && base_res.getArrayNItems() > 1)
            n = static_cast<std::size_t>(cos::integer(base_res.getArrayItem(1), "N").value_or(0));
        (void)bc;
        std::size_t idx = static_cast<std::size_t>(std::max(0.0, c[0]));
        if (n > 0 && (idx + 1) * n <= table.size()) {
            std::vector<double> bcomp;
            for (std::size_t k = 0; k < n; ++k)
                bcomp.push_back(static_cast<unsigned char>(table[idx * n + k]) / 255.0);
            return make_color(base, bcomp, res, depth + 1);
        }
    } else if (family == "Pattern") {
        out.family = ColorFamily::Pattern;
    }
    return out;
}

Color Interpreter::initial_color(const Obj& cs, const Resources& res) {
    Obj r = resolve_cs(cs, res);
    std::string family = r.isName() ? cos::name(r).value_or("")
                         : r.isArray() && r.getArrayNItems() ? cos::name(r.getArrayItem(0)).value_or("")
                                                             : "";
    if (family == "DeviceCMYK" || family == "CMYK") return make_color(r, {0, 0, 0, 1}, res);
    if (family == "DeviceRGB" || family == "RGB" || family == "CalRGB") return make_color(r, {0, 0, 0}, res);
    if (family == "ICCBased" && r.isArray() && r.getArrayNItems() > 1) {
        auto n = cos::integer(r.getArrayItem(1), "N").value_or(1);
        if (n == 4) return make_color(r, {0, 0, 0, 1}, res);
        return make_color(r, std::vector<double>(static_cast<std::size_t>(std::max<std::int64_t>(1, n)), 0.0), res);
    }
    if (family == "Indexed" || family == "I") return make_color(r, {0}, res);
    if (family == "Pattern") {
        Color c;
        c.family = ColorFamily::Pattern;
        return c;
    }
    if (family == "DeviceGray" || family == "G" || family == "CalGray" || family.empty())
        return make_color(r, {0}, res);
    Color c;
    c.family = ColorFamily::Unknown;
    return c;
}

void Interpreter::run() {
    const auto& pages = doc_.pages();
    for (const Page& page : pages) {
        if (hard_stop_) break;
        // veraPDF validates a page's annotation appearances before its content stream (probe fq_annot_first:
        // a font first used in an appearance is "first used" there); first-use semantics follow that order.
        run_annotations(page);
        StreamInstance si;
        si.kind = StreamInstance::Kind::Page;
        si.page = page.index;
        si.semantic = true;
        Obj contents = cos::get(page.dict, "Contents");
        si.stream = contents;
        si.ref = ref_of(contents).valid() ? ref_of(contents) : page.ref;
        si.struct_parents = cos::integer(page.dict, "StructParents");
        si.struct_parent = cos::integer(page.dict, "StructParent");
        // Pages have no stream-level structure parent (the access object is queried with MCID null).
        si.parent_struct_elem = st_.struct_elem_for_mcid(si.struct_parent, si.struct_parents, std::nullopt);
        if (!si.struct_parent) si.parent_struct_elem = Obj::newNull();
        if (!admit(si.ref, 1)) continue;
        int idx = add_stream(std::move(si));
        if (contents.isStream() || contents.isArray()) {
            Resources res;
            res.page = page.resources.isDictionary() ? page.resources : Obj::newNull();
            interpret(idx, res, GState{}, 0);
        }
    }
    run_type3_glyphs();
}

void Interpreter::run_annotations(const Page& page) {
    for (const Obj& annot : cos::array_items(cos::get(page.dict, "Annots"))) {
        if (!annot.isDictionary()) continue;
        Obj ap = cos::get(annot, "AP");
        if (!ap.isDictionary()) continue;
        std::optional<std::string> lang;
        if (auto l = cos::text(annot, "Lang")) lang = l;
        Obj parent = Obj::newNull();
        if (auto sp = cos::integer(annot, "StructParent")) {
            Obj e = st_.parent_tree_entry(*sp);
            if (e.isDictionary()) parent = e;
        }
        // PDAnnotation.getFT: the first /FT of the annotation and its /Parent chain.
        bool signature = false;
        if (cos::name_is(annot, "Subtype", "Widget")) {
            std::unordered_set<std::uint64_t> seen;
            for (Obj cur = annot; cur.isDictionary() || cur.isStream(); cur = cos::get(cur, "Parent")) {
                if (cur.isIndirect() && !seen.insert(ref_of(cur).key()).second) break;
                Obj ft = cos::get(cur, "FT");
                if (!ft.isNull()) {
                    signature = cos::name(ft).value_or("") == "Sig";
                    break;
                }
            }
        }
        auto run_one = [&](const Obj& stm) {
            if (!stm.isStream() || !admit(ref_of(stm), 2)) return;
            StreamInstance si;
            si.kind = StreamInstance::Kind::Annotation;
            si.stream = stm;
            si.ref = ref_of(stm);
            si.page = page.index;
            si.semantic = false;
            si.parent_struct_elem = parent;
            si.default_lang = lang;
            si.annot = ref_of(annot);
            si.annot_dict = annot;
            si.signature_appearance = signature;
            si.struct_parent = cos::integer(stm, "StructParent");
            si.struct_parents = cos::integer(stm, "StructParents");
            int idx = add_stream(std::move(si));
            Resources res;
            res.own = resources_of(stm);
            res.page = page.resources.isDictionary() ? page.resources : Obj::newNull();
            interpret(idx, res, GState{}, 1);
        };
        for (const char* key : {"N", "D", "R"}) {
            Obj e = cos::get(ap, key);
            if (e.isStream()) run_one(e);
            else if (e.isDictionary())
                for (auto& kv : e.getDictAsMap()) run_one(kv.second);
        }
    }
}

void Interpreter::run_type3_glyphs() {
    bool more = true;
    while (more && !hard_stop_) {
        more = false;
        // Fonts actually shown (GFOpTextShow.getFont -> FontFactory.parseFont), in load order. A copy: interpreting
        // glyph procedures may show further fonts.
        std::vector<const Font*> fonts;
        for (auto& f : fonts_.all()) fonts.push_back(f.get());
        for (const Font* f : fonts) {
            if (f->kind != FontKind::Type3 || type3_done_.count(f)) continue;
            auto used = type3_used_.find(f);
            if (used == type3_used_.end()) continue;
            type3_done_.insert(f);
            more = true;
            Obj procs = cos::get(f->dict, "CharProcs");
            if (!procs.isDictionary()) continue;
            // FontFactory.parseFont: the font's resources over those of the page of its first use.
            Resources font_res;
            font_res.own = resources_of(f->dict);
            font_res.page = used->second.page_resources;
            const GState gs = used->second.gs;
            auto run_proc = [&](const Obj& proc, const Resources& res) {
                StreamInstance si;
                si.kind = StreamInstance::Kind::Type3Glyph;
                si.stream = proc;
                si.ref = ref_of(proc);
                si.semantic = false;
                int idx = add_stream(std::move(si));
                interpret(idx, res, gs, 1);
            };
            for (auto& kv : procs.getDictAsMap()) {
                if (!kv.second.isStream()) continue;
                // GFPDType3Font.getResourcesFromCharProcs: a glyph's own /Resources count in PDF/UA-1 (ISO 32000-1) and
                // not in the ISO 32000-2 flavours, so such a glyph is read both ways.
                Obj own = resources_of(kv.second);
                if (own.isDictionary()) {
                    Resources glyph_res;
                    glyph_res.own = own;
                    glyph_res.page = font_res.page;
                    run_proc(kv.second, glyph_res);
                }
                run_proc(kv.second, font_res);
            }
        }
    }
}

void Interpreter::run_pattern(int si, const Obj& pattern, const Resources& outer, const GState& gs, int depth) {
    if (!pattern.isStream() || cos::integer(pattern, "PatternType").value_or(0) != 1) return;
    ObjRef r = ref_of(pattern);
    if (r.valid() && !patterns_done_.insert(r.key()).second) return;
    StreamInstance s;
    s.kind = StreamInstance::Kind::Pattern;
    s.stream = pattern;
    s.ref = r;
    s.page = m_.streams[si].page;
    s.semantic = false;
    int idx = add_stream(std::move(s));
    // ColorSpaceFactory.getPattern: the pattern's resources over those of the page.
    Resources res;
    res.own = resources_of(pattern);
    res.page = outer.page;
    res.outer = &outer;
    push(idx, res, gs, depth + 1);
}

void Interpreter::do_xobject(int si, std::uint32_t op_index, int frame, const std::string& name, const Resources& res,
                             const GState& gs, int depth) {
    Obj xo = res.lookup("XObject", name);
    if (!xo.isStream()) return;
    auto subtype = cos::name(xo, "Subtype").value_or("");
    StreamInstance& parent = m_.streams[si];
    if (subtype == "Image") {
        if (parent.semantic) {
            double xs[4], ys[4];
            gs.ctm.apply(0, 0, xs[0], ys[0]);
            gs.ctm.apply(1, 0, xs[1], ys[1]);
            gs.ctm.apply(0, 1, xs[2], ys[2]);
            gs.ctm.apply(1, 1, xs[3], ys[3]);
            const Rect bbox{*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                            *std::max_element(ys, ys + 4)};
            const ObjRef image = ref_of(xo);
            m_.items.set_xobject(m_.items.add(ContentItem::Kind::ImageXObject, si, frame, op_index, &bbox), image);
        } else if (parent.signature_appearance) {
            m_.signature_graphics.push_back({si, frame, "image"});
        }
        return;
    }
    if (subtype != "Form") return;
    ObjRef xr = ref_of(xo);
    // OperatorParser case DO: inherited MCID of the top frame, structure element through the invoking
    // stream's access object, else the invoking stream's structure parent.
    std::optional<std::int64_t> mcid = frame >= 0 ? m_.inherited_mcid(frame) : std::nullopt;
    Obj pse = Obj::newNull();
    if (mcid || parent.struct_parent) pse = st_.struct_elem_for_mcid(parent.struct_parent, parent.struct_parents, mcid);
    if (!pse.isDictionary()) pse = parent.parent_struct_elem;
    const bool in_artifact = m_.parents_tags_contain(si, frame, "Artifact");
    // Same stream, same structure parent, same tags and text state: another reading can only repeat the findings.
    {
        const ObjRef pr = ref_of(pse);
        std::uint64_t ctx = 3 + (parent.semantic ? 8u : 0u) + (in_artifact ? 16u : 0u) + (static_cast<std::uint64_t>(gs.tr & 7) << 5);
        ctx ^= (pr.valid() ? pr.key() : 0x5bd1e995u) * 0xC2B2AE3D27D4EB4Full;
        ctx ^= reinterpret_cast<std::uintptr_t>(gs.font) * 0x165667B19E3779F9ull;
        if (!admit(xr, ctx)) return;
    }
    StreamInstance s;
    s.kind = StreamInstance::Kind::Form;
    s.stream = xo;
    s.ref = xr;
    s.page = parent.page;
    s.parent = si;
    s.parent_frame = frame;
    s.parent_op = op_index;
    s.semantic = parent.semantic;
    s.signature_appearance = parent.signature_appearance;
    s.annot = parent.annot;
    s.annot_dict = parent.annot_dict;
    s.default_lang = std::nullopt;
    s.inherited_artifact = in_artifact;
    s.struct_parent = cos::integer(xo, "StructParent");
    s.struct_parents = cos::integer(xo, "StructParents");
    // GFPDContentStream: own access object (StructParent, MCID null) wins over the inherited parent.
    Obj own = s.struct_parent ? st_.struct_elem_for_mcid(s.struct_parent, s.struct_parents, std::nullopt)
                              : Obj::newNull();
    s.parent_struct_elem = own.isDictionary() ? own : pse;
    int idx = add_stream(std::move(s));
    if (xr.valid() && active_.count(xr.key())) {
        m_.streams[idx].loop_skipped = true;
        return;
    }
    Resources r2;
    r2.own = resources_of(xo);
    r2.page = res.page;
    r2.outer = &res;
    GState g = gs;
    auto mtx = cos::array_items(cos::get(xo, "Matrix"));
    if (mtx.size() == 6) {
        Matrix fm{cos::number(mtx[0]).value_or(1), cos::number(mtx[1]).value_or(0), cos::number(mtx[2]).value_or(0),
                  cos::number(mtx[3]).value_or(1), cos::number(mtx[4]).value_or(0), cos::number(mtx[5]).value_or(0)};
        g.ctm = fm * g.ctm;
    }
    push(idx, r2, g, depth + 1);
}

void Interpreter::interpret(int si, const Resources& res, const GState& gs, int depth) {
    const std::size_t base = exec_.size();
    push(si, res, gs, depth);
    drain(base);
}

void Interpreter::drain(std::size_t base) {
    while (exec_.size() > base) {
        Exec& x = *exec_.back();
        if (step(x)) {
            finish(x);
            exec_.pop_back();
        }
    }
}

void Interpreter::push(int si, const Resources& res, const GState& gs, int depth) {
    auto x = std::make_unique<Exec>();
    StreamInstance& s = m_.streams[si];
    if (!stream_bytes(s, x->data)) return;
    x->si = si;
    x->res = res;
    x->gs = gs;
    x->depth = depth;
    const ObjRef key = s.stream.isStream() ? ref_of(s.stream) : ObjRef{};
    if (key.valid()) x->active_key = active_.insert(key.key()).second;
    s.base_ctm = gs.ctm;
    x->parser = std::make_unique<ContentParser>(x->data);
    x->first_item = m_.items.size();
    exec_.push_back(std::move(x));
}

void Interpreter::finish(Exec& x) {
    const int si = x.si;
    StreamInstance& s = m_.streams[si];
    s.op_count = x.op_index;
    for (const LexError& e : x.parser->errors()) s.errors.push_back(e);
    // veraPDF splits a semantic stream into top-level marked / unmarked sequences: content inside a
    // top-level BMC/BDC that is never closed belongs to the trailing unmarked sequence.
    if (s.semantic && !x.fstack.empty()) {
        for (std::size_t i = x.first_item; i < m_.items.size(); ++i) {
            content_detail::ItemRecord& it = m_.items.record(i);
            if (it.stream != si || it.frame < 0) continue;
            int root = it.frame;
            while (m_.frames[root].parent >= 0) root = m_.frames[root].parent;
            if (m_.frames[root].end_op == 0xffffffffu) it.frame = -1;
        }
    }
    for (ContentObserver* ob : end_obs_) ob->on_stream_end(m_, si);
    if (x.active_key) active_.erase(ref_of(s.stream).key());
}

bool Interpreter::step(Exec& x) {
    const int si = x.si;
    const Resources& res = x.res;
    const int depth = x.depth;
    GState& gs = x.gs;
    StreamInstance* sp = &m_.streams[si];
    std::vector<GState>& gstack = x.gstack;
    std::vector<int>& fstack = x.fstack;
    Matrix& tm = x.tm;
    Matrix& tlm = x.tlm;
    bool in_text = false;
    bool& have_path = x.have_path;
    int& path_rects = x.path_rects;
    bool& path_other = x.path_other;
    bool& path_curve = x.path_curve;
    double& px0 = x.px0;
    double& py0 = x.py0;
    double& px1 = x.px1;
    double& py1 = x.py1;
    double& cx = x.cx;
    double& cy = x.cy;
    auto add_pt = [&](double x, double y) {
        double ux, uy;
        gs.ctm.apply(x, y, ux, uy);
        if (!have_path) {
            px0 = px1 = ux;
            py0 = py1 = uy;
            have_path = true;
        } else {
            px0 = std::min(px0, ux);
            px1 = std::max(px1, ux);
            py0 = std::min(py0, uy);
            py1 = std::max(py1, uy);
        }
        cx = x;
        cy = y;
    };
    std::vector<CharCode>& codes = codes_;
    std::vector<double>& comps = comps_;
    const bool semantic = sp->semantic;
    std::uint32_t& op_index = x.op_index;
    ContentParser& parser = *x.parser;

    auto add_item = [&](ContentItem::Kind kind, std::uint32_t opi, const Rect* bbox) -> int {
        if (!semantic) return -1;
        return m_.items.add(kind, si, fstack.empty() ? -1 : fstack.back(), opi, bbox);
    };

    // `run` and `item` are the records of the operator's text run and item (null outside semantic streams).
    auto show_string = [&](const std::string& bytes, std::uint32_t opi, std::uint32_t& gidx, TextRecord* run,
                           content_detail::ItemRecord* item, Rect& tb, bool& tb_valid) {
        const Font* const font = gs.font;
        if (!font) return;
        codes.clear();
        font->codes(bytes, codes);
        if (codes.empty()) return;
        // Trm = [Tfs*Th 0 0 Tfs 0 Ts] x Tm x CTM, and showing a glyph only translates Tm along its own axes:
        // glyph k's point (gx, gy) is base(cum_x + gx*Tfs*Th, cum_y + gy*Tfs + Ts) with base = Tm0 x CTM.
        // The text state cannot change while the string is shown (the observers of a glyph do not get at it): it
        // is read once.
        const double tfs = gs.font_size, ts = gs.ts, th = gs.th, tc = gs.tc, tw = gs.tw;
        const Matrix base = tm * gs.ctm;
        const double size_user = std::abs(tfs) * hypot_cd_(base.c, base.d);
        const double dir_norm = hypot_ab_(base.a, base.b);
        const double g2t = font->glyph_to_text();
        const bool vertical = font->vertical();
        ContentObserver* const* const glyph_obs = glyph_obs_.data();
        const std::size_t n_glyph_obs = glyph_obs_.size();
        double cum_x = 0, cum_y = 0;
        double span_x0 = 0, span_x1 = 0, span_y0 = 0, span_y1 = 0;
        bool span = false;
        GlyphEvent ev;
        ev.font = font;
        ev.tr = gs.tr;
        ev.stream = si;
        ev.frame = fstack.empty() ? -1 : fstack.back();
        ev.op_index = opi;
        ev.real_content = semantic;
        ev.size_user = size_user;
        for (const CharCode& c : codes) {
            const double w0 = font->advance(c.code) * g2t;
            ev.code = c;
            ev.glyph_index = gidx++;
            base.apply(cum_x, cum_y + ts, ev.x, ev.y);
            ev.advance_user = std::abs(w0 * tfs * th) * dir_norm;
            for (std::size_t k = 0; k < n_glyph_obs; ++k) glyph_obs[k]->on_glyph(m_, ev);
            if (run) {
                ++item->glyphs;
                if (const std::u32string* u = font->to_unicode(c.code)) m_.texts.append(u->data(), u->size());
                else ++run->unmapped;
                // Glyph box in text space: advance wide (0.5 em when zero), baseline-relative [-0.2, 0.8] em.
                const double wq = (w0 != 0 ? w0 : 0.5) * tfs * th;
                const double gx0 = std::min(cum_x, cum_x + wq), gx1 = std::max(cum_x, cum_x + wq);
                if (!span) {
                    span_x0 = gx0, span_x1 = gx1, span_y0 = span_y1 = cum_y;
                    span = true;
                } else {
                    span_x0 = std::min(span_x0, gx0);
                    span_x1 = std::max(span_x1, gx1);
                    span_y0 = std::min(span_y0, cum_y);
                    span_y1 = std::max(span_y1, cum_y);
                }
            }
            if (vertical) cum_y -= w0 * tfs + tc;
            else cum_x += (w0 * tfs + tc + (font->word_space(c) ? tw : 0)) * th;
        }
        tm = Matrix{1, 0, 0, 1, cum_x, cum_y} * tm;
        if (run && span) {
            const double ylo = std::min(span_y0 + ts - 0.2 * tfs, span_y0 + ts + 0.8 * tfs);
            const double yhi = std::max(span_y1 + ts - 0.2 * tfs, span_y1 + ts + 0.8 * tfs);
            double xs[4], ys[4];
            base.apply(span_x0, ylo, xs[0], ys[0]);
            base.apply(span_x1, ylo, xs[1], ys[1]);
            base.apply(span_x0, yhi, xs[2], ys[2]);
            base.apply(span_x1, yhi, xs[3], ys[3]);
            Rect g{*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                   *std::max_element(ys, ys + 4)};
            if (!tb_valid) {
                tb = g;
                tb_valid = true;
            } else {
                tb.llx = std::min(tb.llx, g.llx);
                tb.lly = std::min(tb.lly, g.lly);
                tb.urx = std::max(tb.urx, g.urx);
                tb.ury = std::max(tb.ury, g.ury);
            }
        }
    };

    // The item and the text run of a text-showing operator; the run's record, null outside semantic streams.
    auto begin_text_op = [&](std::uint32_t opi) -> std::pair<TextRecord*, content_detail::ItemRecord*> {
        if (!semantic) return {nullptr, nullptr};
        int item = add_item(ContentItem::Kind::Text, opi, nullptr);
        TextStyle style;
        style.font = gs.font;
        style.size = gs.font_size;
        style.tr = gs.tr;
        style.fill = gs.fill;
        style.stroke = gs.stroke;
        style.fill_alpha = gs.fill_alpha;
        Matrix trm = Matrix{gs.font_size * gs.th, 0, 0, gs.font_size, 0, gs.ts} * tm * gs.ctm;
        style.size_user = hypot_trm_(trm.c, trm.d);
        TextRecord& run = m_.texts.begin_run(item, style);
        return {&run, &m_.items.record(static_cast<std::size_t>(item))};
    };
    auto end_text_op = [&](content_detail::ItemRecord* item, const Rect& tb, bool tb_valid) {
        if (item && tb_valid) {
            item->bbox = tb;      // also the box of the text run
            item->flags |= 1;
        }
    };

    RawOp op;
    while (parser.next(op)) {
        if (++ops_total_ > lim_.max_operators) {
            m_.budget_exceeded = true;
            hard_stop_ = true;
            break;
        }
        const std::uint32_t opi = op_index++;
        std::vector<Operand>& a = *op.operands;
        for (ContentObserver* o : op_obs_) o->on_operator(m_, si, opi, op, gs);
        const Op oc = op.code;
        auto num = [&](std::size_t i) -> double { return i < a.size() && a[i].is_number() ? a[i].num() : 0.0; };
        auto nums_ok = [&](std::size_t n) {
            if (a.size() < n) return false;
            for (std::size_t i = a.size() - n; i < a.size(); ++i)
                if (!a[i].is_number()) return false;
            return true;
        };
        auto last_name = [&]() -> std::string { return !a.empty() && a.back().type == Operand::Name ? a.back().s : ""; };
        if (oc == Op::q) {
            gstack.push_back(gs);
        } else if (oc == Op::Q) {
            if (!gstack.empty()) {
                gs = gstack.back();
                gstack.pop_back();
            }
        } else if (oc == Op::cm) {
            if (nums_ok(6)) {
                std::size_t b = a.size() - 6;
                Matrix mm{a[b].num(), a[b + 1].num(), a[b + 2].num(), a[b + 3].num(), a[b + 4].num(), a[b + 5].num()};
                gs.ctm = mm * gs.ctm;
            }
        } else if (oc == Op::w) {
            if (nums_ok(1)) gs.line_width = a.back().num();
        } else if (oc == Op::gs) {
            Obj eg = res.lookup("ExtGState", last_name());
            if (eg.isDictionary()) {
                if (auto v = cos::number(eg, "ca")) gs.fill_alpha = *v;
                if (auto v = cos::number(eg, "CA")) gs.stroke_alpha = *v;
                if (auto v = cos::number(eg, "LW")) gs.line_width = *v;
                Obj f = cos::get(eg, "Font");
                if (f.isArray() && f.getArrayNItems() >= 2) {
                    // GraphicState.copyPropertiesFormExtGState: only a font that exists replaces the current one
                    if (const Font* fnt = fonts_.get(f.getArrayItem(0))) {
                        gs.font_dict = f.getArrayItem(0);
                        gs.font = fnt;
                    }
                    gs.font_size = cos::number(f.getArrayItem(1)).value_or(gs.font_size);
                }
            }
        } else if (oc == Op::BT) {
            in_text = true;
            tm = tlm = Matrix{};
        } else if (oc == Op::ET) {
            in_text = false;
        } else if (oc == Op::Tf) {
            // veraPDF OperatorParser: the font is the FIRST operand (no font unless it is a name), the size the
            // second operand when it is a number
            gs.font_dict = (!a.empty() && a[0].type == Operand::Name) ? res.lookup("Font", a[0].s) : Obj();
            gs.font = fonts_.get(gs.font_dict);
            if (a.size() > 1 && a[1].is_number()) gs.font_size = a[1].num();
        } else if (oc == Op::Tc) {
            if (nums_ok(1)) gs.tc = a.back().num();
        } else if (oc == Op::Tw) {
            if (nums_ok(1)) gs.tw = a.back().num();
        } else if (oc == Op::Tz) {
            if (nums_ok(1)) gs.th = a.back().num() / 100.0;
        } else if (oc == Op::TL) {
            if (nums_ok(1)) gs.tl = a.back().num();
        } else if (oc == Op::Ts) {
            if (nums_ok(1)) gs.ts = a.back().num();
        } else if (oc == Op::Tr) {
            if (!a.empty() && a[0].type == Operand::Int) {           // veraPDF: the first operand
                std::int64_t v = a[0].i;
                gs.tr = (v >= 0 && v <= 7) ? static_cast<int>(v) : 0;   // veraPDF: invalid -> FILL
            } else {
                gs.tr = 0;
            }
        } else if (oc == Op::Td || oc == Op::TD) {
            if (nums_ok(2)) {
                double tx = a[a.size() - 2].num(), ty = a[a.size() - 1].num();
                if (oc == Op::TD) gs.tl = -ty;
                tlm = Matrix{1, 0, 0, 1, tx, ty} * tlm;
                tm = tlm;
            }
        } else if (oc == Op::Tm) {
            if (nums_ok(6)) {
                std::size_t b = a.size() - 6;
                tlm = tm = Matrix{a[b].num(), a[b + 1].num(), a[b + 2].num(), a[b + 3].num(), a[b + 4].num(), a[b + 5].num()};
            }
        } else if (oc == Op::T_star) {
            tlm = Matrix{1, 0, 0, 1, 0, -gs.tl} * tlm;
            tm = tlm;
        } else if (oc == Op::Tj || oc == Op::quote || oc == Op::dquote || oc == Op::TJ) {
            if (oc == Op::dquote && a.size() >= 3) {
                gs.tw = num(a.size() - 3);
                gs.tc = num(a.size() - 2);
            }
            if (oc == Op::quote || oc == Op::dquote) {
                tlm = Matrix{1, 0, 0, 1, 0, -gs.tl} * tlm;
                tm = tlm;
            }
            if (gs.font && gs.font->kind == FontKind::Type3)
                type3_used_.try_emplace(gs.font, Type3Use{res.page, gs});
            const auto [run, item] = begin_text_op(opi);
            Rect tb;
            bool tb_valid = false;
            std::uint32_t gidx = 0;
            if (oc == Op::TJ) {
                if (!a.empty() && a.back().type == Operand::Array) {
                    for (const Operand& e : a.back().items) {
                        if (e.type == Operand::String) {
                            show_string(e.s, opi, gidx, run, item, tb, tb_valid);
                        } else if (e.is_number()) {
                            double adj = -e.num() / 1000.0 * gs.font_size;
                            if (gs.font && gs.font->vertical()) tm = Matrix{1, 0, 0, 1, 0, adj} * tm;
                            else tm = Matrix{1, 0, 0, 1, adj * gs.th, 0} * tm;
                        }
                    }
                }
            } else if (!a.empty() && a.back().type == Operand::String) {
                show_string(a.back().s, opi, gidx, run, item, tb, tb_valid);
            }
            end_text_op(item, tb, tb_valid);
        } else if (oc == Op::m) {
            path_other = true;
            if (nums_ok(2)) add_pt(num(a.size() - 2), num(a.size() - 1));
        } else if (oc == Op::l) {
            path_other = true;
            if (nums_ok(2)) add_pt(num(a.size() - 2), num(a.size() - 1));
        } else if (oc == Op::c) {
            path_other = true;
            path_curve = true;
            if (nums_ok(6)) {
                std::size_t b = a.size() - 6;
                add_pt(a[b].num(), a[b + 1].num());
                add_pt(a[b + 2].num(), a[b + 3].num());
                add_pt(a[b + 4].num(), a[b + 5].num());
            }
        } else if (oc == Op::v || oc == Op::y) {
            path_other = true;
            path_curve = true;
            if (nums_ok(4)) {
                std::size_t b = a.size() - 4;
                add_pt(a[b].num(), a[b + 1].num());
                add_pt(a[b + 2].num(), a[b + 3].num());
            }
        } else if (oc == Op::re) {
            if (nums_ok(4)) {
                std::size_t b = a.size() - 4;
                double x = a[b].num(), y = a[b + 1].num(), w = a[b + 2].num(), h = a[b + 3].num();
                add_pt(x, y);
                add_pt(x + w, y);
                add_pt(x + w, y + h);
                add_pt(x, y + h);
                cx = x;
                cy = y;
                ++path_rects;
            }
        } else if (oc == Op::h) {
        } else if (oc == Op::S || oc == Op::s || oc == Op::f || oc == Op::F || oc == Op::f_star || oc == Op::B || oc == Op::B_star ||
                   oc == Op::b || oc == Op::b_star) {
            Rect r{px0, py0, px1, py1};
            int idx = add_item(ContentItem::Kind::LineArt, opi, have_path ? &r : nullptr);
            if (idx >= 0) {
                bool fills = oc != Op::S && oc != Op::s;
                bool strokes = oc == Op::S || oc == Op::s || oc == Op::B || oc == Op::B_star || oc == Op::b || oc == Op::b_star;
                m_.items.set_paint(idx, static_cast<std::uint8_t>((fills ? 1 : 0) | (strokes ? 2 : 0)), gs.fill,
                                   static_cast<float>(gs.fill_alpha),
                                   have_path && path_rects == 1 && !path_other && gs.ctm.b == 0 && gs.ctm.c == 0);
            } else if (sp->signature_appearance && path_curve) {
                m_.signature_graphics.push_back({si, fstack.empty() ? -1 : fstack.back(), "drawn path"});
            }
            have_path = false;
            path_rects = 0;
            path_other = false;
            path_curve = false;
        } else if (oc == Op::n) {
            have_path = false;
            path_rects = 0;
            path_other = false;
            path_curve = false;
        } else if (oc == Op::W || oc == Op::W_star) {
        } else if (oc == Op::sh) {
            add_item(ContentItem::Kind::Shading, opi, nullptr);
            if (sp->signature_appearance) m_.signature_graphics.push_back({si, fstack.empty() ? -1 : fstack.back(), "shading"});
        } else if (oc == Op::EI) {
            double xs[4], ys[4];
            gs.ctm.apply(0, 0, xs[0], ys[0]);
            gs.ctm.apply(1, 0, xs[1], ys[1]);
            gs.ctm.apply(0, 1, xs[2], ys[2]);
            gs.ctm.apply(1, 1, xs[3], ys[3]);
            Rect r{*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                   *std::max_element(ys, ys + 4)};
            add_item(ContentItem::Kind::InlineImage, opi, &r);
            if (sp->signature_appearance) m_.signature_graphics.push_back({si, fstack.empty() ? -1 : fstack.back(), "inline image"});
        } else if (oc == Op::Do) {
            std::string name = last_name();
            if (!name.empty()) {
                do_xobject(si, opi, fstack.empty() ? -1 : fstack.back(), name, res, gs, depth);
                if (exec_.back().get() != &x) return false;   // the form runs now; this stream resumes after it
                sp = &m_.streams[si];   // add_stream may reallocate
            }
        } else if (oc == Op::BMC || oc == Op::BDC) {
            McFrame f;
            f.stream = si;
            f.parent = fstack.empty() ? -1 : fstack.back();
            f.bdc = oc == Op::BDC;
            f.begin_op = opi;
            if (f.bdc) {
                if (a.size() > 1 && a[a.size() - 2].type == Operand::Name) f.tag = a[a.size() - 2].s;
                if (!a.empty()) {
                    const Operand& p = a.back();
                    auto pick_str = [](const Operand* v) -> std::optional<std::string> {
                        if (v && v->type == Operand::String) return v->s;
                        return std::nullopt;
                    };
                    if (p.type == Operand::Dict) {
                        if (const Operand* v = p.get("MCID"); v && v->type == Operand::Int) f.mcid = v->i;
                        f.lang = pick_str(p.get("Lang"));
                        f.actual_text = pick_str(p.get("ActualText"));
                        f.alt = pick_str(p.get("Alt"));
                        f.e = pick_str(p.get("E"));
                    } else if (p.type == Operand::Name) {
                        Obj pd = res.lookup("Properties", p.s);
                        if (pd.isDictionary()) {
                            f.mcid = cos::integer(pd, "MCID");
                            f.lang = cos::bytes(pd, "Lang");
                            f.actual_text = cos::bytes(pd, "ActualText");
                            f.alt = cos::bytes(pd, "Alt");
                            f.e = cos::bytes(pd, "E");
                            // /Type, /Subtype and /BBox of the property list are not kept (no rule reads them), but
                            // they are still read: resolving them is where qpdf reports damage in these objects.
                            (void)cos::name(pd, "Type");
                            (void)cos::name(pd, "Subtype");
                            (void)cos::rect(cos::get(pd, "BBox"));
                        }
                    }
                }
                if (f.mcid) {
                    f.own_struct_elem = st_.struct_elem_for_mcid(sp->struct_parent, sp->struct_parents, f.mcid);
                }
            } else if (!a.empty() && a.back().type == Operand::Name) {
                f.tag = a.back().s;
            }
            // One null object for all frames: Obj::newNull() allocates an object per call.
            if (!f.own_struct_elem.isDictionary()) f.own_struct_elem = null_;
            f.struct_elem = f.own_struct_elem.isDictionary() ? f.own_struct_elem
                            : f.parent >= 0                  ? m_.frames[f.parent].struct_elem
                                                             : null_;
            m_.frames.push_back(std::move(f));
            fstack.push_back(static_cast<int>(m_.frames.size()) - 1);
        } else if (oc == Op::EMC) {
            if (!fstack.empty()) {
                m_.frames[fstack.back()].end_op = opi;
                fstack.pop_back();
            }
        } else if (oc == Op::G || oc == Op::g || oc == Op::RG || oc == Op::rg || oc == Op::K || oc == Op::k) {
            bool fill = oc == Op::g || oc == Op::rg || oc == Op::k;
            std::size_t n = (oc == Op::G || oc == Op::g) ? 1 : (oc == Op::RG || oc == Op::rg) ? 3 : 4;
            if (nums_ok(n)) {
                comps.assign(n, 0);
                for (std::size_t i = 0; i < n; ++i) comps[i] = a[a.size() - n + i].num();
                Obj cs = Obj::newName(n == 1 ? "/DeviceGray" : n == 3 ? "/DeviceRGB" : "/DeviceCMYK");
                Color c = make_color(cs, comps, res);
                if (fill) {
                    gs.fill = c;
                    gs.fill_cs = cs;
                } else {
                    gs.stroke = c;
                    gs.stroke_cs = cs;
                }
            }
        } else if (oc == Op::CS || oc == Op::cs) {
            if (!a.empty() && a.back().type == Operand::Name) {
                Obj cs = Obj::newName("/" + a.back().s);
                Obj resolved = resolve_cs(cs, res);
                Color c = initial_color(resolved, res);
                if (oc == Op::cs) {
                    gs.fill_cs = resolved;
                    gs.fill = c;
                } else {
                    gs.stroke_cs = resolved;
                    gs.stroke = c;
                }
            }
        } else if (oc == Op::SC || oc == Op::SCN || oc == Op::sc || oc == Op::scn) {
            bool fill = oc == Op::sc || oc == Op::scn;
            comps.clear();
            for (const Operand& x : a)
                if (x.is_number()) comps.push_back(x.num());
            Obj cs = fill ? gs.fill_cs : gs.stroke_cs;
            if (!a.empty() && a.back().type == Operand::Name) {
                Color c;
                c.family = ColorFamily::Pattern;
                if (fill) gs.fill = c;
                else gs.stroke = c;
                run_pattern(si, res.lookup("Pattern", a.back().s), res, gs, depth);
                if (exec_.back().get() != &x) return false;
                sp = &m_.streams[si];
            } else {
                Color c = make_color(cs, comps, res);
                if (fill) gs.fill = c;
                else gs.stroke = c;
            }
        }
        (void)in_text;
        (void)cx;
        (void)cy;
    }
    return true;
}

}  // namespace

void interpret_document(Document& doc, const StructTree& st, FontRegistry& fonts, ContentModel& model,
                        const std::vector<ContentObserver*>& observers, const InterpreterLimits& limits) {
    Interpreter in(doc, st, fonts, model, observers, limits);
    in.run();
}

}  // namespace eqx
