// Thin helpers over qpdf's object model. Every module accesses PDF objects through QPDFObjectHandle;
// these helpers centralise the null/type checks and the "N G" object reference format of the reports.
#pragma once

#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eqx {

using Obj = QPDFObjectHandle;

// Indirect object reference; num == 0 means "direct object / no object".
struct ObjRef {
    int num = 0;
    int gen = 0;
    bool valid() const { return num > 0; }
    std::string str() const { return std::to_string(num) + " " + std::to_string(gen); }
    std::uint64_t key() const { return (std::uint64_t(std::uint32_t(num)) << 16) | std::uint16_t(gen); }
    bool operator==(const ObjRef& o) const { return num == o.num && gen == o.gen; }
    bool operator!=(const ObjRef& o) const { return !(*this == o); }
    bool operator<(const ObjRef& o) const { return num < o.num || (num == o.num && gen < o.gen); }
};

inline ObjRef ref_of(const Obj& h) {
    if (h.isIndirect()) return {h.getObjectID(), h.getGeneration()};
    return {};
}

struct ObjRefHash {
    std::size_t operator()(const ObjRef& r) const { return std::hash<std::uint64_t>()(r.key()); }
};

struct Rect {
    double llx = 0, lly = 0, urx = 0, ury = 0;
    bool empty() const { return urx <= llx || ury <= lly; }
    double width() const { return urx - llx; }
    double height() const { return ury - lly; }
    bool contains(double x, double y) const { return x >= llx && x <= urx && y >= lly && y <= ury; }
    bool intersects(const Rect& o) const { return !(o.urx < llx || o.llx > urx || o.ury < lly || o.lly > ury); }
};

namespace cos {

// Dictionary of a dictionary or of a stream; a null handle otherwise.
Obj dict_of(const Obj& h);

// h[key] for dictionaries and streams (stream dictionary); null handle when absent or not a dict.
Obj get(const Obj& h, std::string_view key);
bool has(const Obj& h, std::string_view key);

std::optional<std::string> name(const Obj& h);                       // "/Foo" -> "Foo"
std::optional<std::string> name(const Obj& h, std::string_view key);
bool name_is(const Obj& h, std::string_view key, std::string_view value);
std::optional<std::int64_t> integer(const Obj& h);
std::optional<std::int64_t> integer(const Obj& h, std::string_view key);
std::optional<double> number(const Obj& h);
std::optional<double> number(const Obj& h, std::string_view key);
std::optional<bool> boolean(const Obj& h, std::string_view key);
// Raw bytes of a string object.
std::optional<std::string> bytes(const Obj& h);
std::optional<std::string> bytes(const Obj& h, std::string_view key);
// Text string decoded to UTF-8 (pdf_text_to_utf8); only for string objects.
std::optional<std::string> text(const Obj& h);
std::optional<std::string> text(const Obj& h, std::string_view key);

std::vector<Obj> array_items(const Obj& h);
std::optional<Rect> rect(const Obj& h);

// Decoded stream data (generalized filters); nullopt when the stream cannot be decoded.
std::optional<std::string> stream_data(const Obj& stream, std::string* error = nullptr);
// What the filters of an undecodable stream deliver before the data fails: each filter of the chain runs up to its first
// error (LZW, Flate with or without predictor, ASCII85, ASCIIHex, RunLength). Empty when a filter is an image codec or the chain
// delivers nothing: raw encoded bytes are no content. Output is capped at 1 GiB.
std::string decodable_prefix(const Obj& stream);
// A Flate stream that decoded although its Adler-32 is wrong (qpdf is silent): the bytes are not what was written. `decoded`
// is the result of stream_data. Checks the trailer against the data, and inflates again only when that does not settle it.
bool flate_checksum_failed(const Obj& stream, const std::string& decoded);
// Its parts, for bytes outside a stream object. inflate_prefix: up to the first error. undo_predictor: complete rows only;
// false when this is not a predictor it can apply.
std::string inflate_prefix(const std::string& raw);
bool undo_predictor(std::string& data, std::int64_t predictor, std::int64_t colors, std::int64_t bits, std::int64_t columns);

// Name-tree / number-tree flattening (no repair, loop-safe, document order of the leaves).
std::vector<std::pair<std::string, Obj>> name_tree(const Obj& root);
std::vector<std::pair<std::int64_t, Obj>> number_tree(const Obj& root);

}  // namespace cos
}  // namespace eqx
