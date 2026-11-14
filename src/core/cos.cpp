#include "core/cos.h"

#include <qpdf/Buffer.hh>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDFExc.hh>

#include <algorithm>
#include <unordered_set>

#include "util/text.h"
#include "util/unread.h"

namespace eqx::cos {

Obj dict_of(const Obj& h) {
    if (h.isDictionary()) return h;
    if (h.isStream()) return const_cast<Obj&>(h).getDict();
    return Obj::newNull();
}

namespace {

// qpdf's spelling of a key: "/" + key. Keys of up to 14 characters stay in the string's own buffer.
inline std::string slash_key(std::string_view key) {
    std::string k(key.size() + 1, '/');
    std::memcpy(k.data() + 1, key.data(), key.size());
    return k;
}

// The value of key k ("/Key") in h, a dictionary or a stream (its dictionary): a pointer to the value, or nullptr
// when h is neither, the key is missing, or the value is null (also an indirect reference to nothing). `hold` keeps
// alive what the pointer may refer to. No null handle is created and no handle copied for the caller to inspect.
#ifdef DOX_QPDF_PINNED
// With the pinned, statically linked qpdf (CMake DOX_STATIC_DEPS) the dictionary is searched once, through the lookup
// qpdf uses itself: BaseHandle::find returns the stored value, or an uninitialized handle when the key is missing
// or the object is no dictionary. It is declared in qpdf's installed ObjectHandle.hh but not exported by the shared
// library, which offers hasKey() and getKey() only: two searches for a present key.
inline const Obj* value_of(const Obj& h, const std::string& k, Obj& hold) {
    const Obj* v = &h.find(k);
    if (!v->isInitialized()) {   // no such key in a dictionary, or h is not a dictionary
        if (!h.isStream()) return nullptr;
        hold = const_cast<Obj&>(h).getDict();
        v = &hold.find(k);       // refers into the stream's dictionary, which the stream keeps
        if (!v->isInitialized()) return nullptr;
    }
    return v->isNull() ? nullptr : v;
}
#else
inline const Obj* value_of(const Obj& h, const std::string& k, Obj& hold) {
    const Obj* d = &h;
    if (!h.isDictionary()) {
        if (!h.isStream()) return nullptr;
        hold = const_cast<Obj&>(h).getDict();
        if (!hold.isDictionary()) return nullptr;
        d = &hold;
    }
    if (!d->hasKey(k)) return nullptr;   // false for a missing key and for a null value
    hold = d->getKey(k);
    return &hold;
}
#endif

}  // namespace

// h[key]. A missing key and a null value both give a direct null.
Obj get(const Obj& h, std::string_view key) {
    Obj hold;
    const Obj* v = value_of(h, slash_key(key), hold);
    if (!v) return Obj::newNull();
    return v == &hold ? std::move(hold) : *v;
}

bool has(const Obj& h, std::string_view key) {
    Obj hold;
    return value_of(h, slash_key(key), hold) != nullptr;
}

std::optional<std::string> name(const Obj& h) {
    if (!h.isName()) return std::nullopt;
    std::string n = h.getName();
    if (!n.empty() && n[0] == '/') n.erase(0, 1);
    return n;
}

std::optional<std::string> name(const Obj& h, std::string_view key) {
    Obj hold;
    const Obj* v = value_of(h, slash_key(key), hold);
    return v ? name(*v) : std::nullopt;
}

bool name_is(const Obj& h, std::string_view key, std::string_view value) {
    Obj hold;
    const Obj* v = value_of(h, slash_key(key), hold);
    if (!v || !v->isName()) return false;
    const std::string n = v->getName();
    std::string_view s = n;
    if (!s.empty() && s[0] == '/') s.remove_prefix(1);
    return s == value;
}

std::optional<std::int64_t> integer(const Obj& h) {
    if (h.isInteger()) return h.getIntValue();
    return std::nullopt;
}

std::optional<std::int64_t> integer(const Obj& h, std::string_view key) {
    Obj hold;
    const Obj* v = value_of(h, slash_key(key), hold);
    return v ? integer(*v) : std::nullopt;
}

std::optional<double> number(const Obj& h) {
    if (h.isInteger()) return static_cast<double>(h.getIntValue());
    if (h.isReal()) return h.getNumericValue();
    return std::nullopt;
}

std::optional<double> number(const Obj& h, std::string_view key) {
    Obj hold;
    const Obj* v = value_of(h, slash_key(key), hold);
    return v ? number(*v) : std::nullopt;
}

std::optional<bool> boolean(const Obj& h, std::string_view key) {
    Obj hold;
    const Obj* v = value_of(h, slash_key(key), hold);
    if (v && v->isBool()) return v->getBoolValue();
    return std::nullopt;
}

std::optional<std::string> bytes(const Obj& h) {
    if (h.isString()) return h.getStringValue();
    return std::nullopt;
}

std::optional<std::string> bytes(const Obj& h, std::string_view key) {
    Obj hold;
    const Obj* v = value_of(h, slash_key(key), hold);
    return v ? bytes(*v) : std::nullopt;
}

std::optional<std::string> text(const Obj& h) {
    if (!h.isString()) return std::nullopt;
    return pdf_text_to_utf8(h.getStringValue());
}

std::optional<std::string> text(const Obj& h, std::string_view key) {
    Obj hold;
    const Obj* v = value_of(h, slash_key(key), hold);
    return v ? text(*v) : std::nullopt;
}

std::vector<Obj> array_items(const Obj& h) {
    std::vector<Obj> out;
    if (!h.isArray()) return out;
    // The handles in one call: getArrayItem costs about 125 instructions per item (it builds an array handle and checks
    // the bounds each time). What the two differ in is nulls, which stay exactly as getArrayItem gives them:
    //   - a null the parser left as an empty slot comes back uninitialized here; getArrayItem makes a null object;
    //   - a null object stored in the array (a sparse array, an array built in memory): the items are fetched one
    //     by one as before, so each keeps the identity getArrayItem gives it.
    // Indirect items are not looked at: asking an indirect object for its type would resolve it.
    out = const_cast<Obj&>(h).getArrayAsVector();
    for (Obj& item : out) {
        if (!item.isInitialized()) {
            item = Obj::newNull();
        } else if (!item.isIndirect() && item.isNull()) {
            const int n = const_cast<Obj&>(h).getArrayNItems();
            out.clear();
            out.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) out.push_back(const_cast<Obj&>(h).getArrayItem(i));
            break;
        }
    }
    return out;
}

std::optional<Rect> rect(const Obj& h) {
    if (!h.isArray() || const_cast<Obj&>(h).getArrayNItems() != 4) return std::nullopt;
    double v[4];
    for (int i = 0; i < 4; ++i) {
        auto x = number(const_cast<Obj&>(h).getArrayItem(i));
        if (!x) return std::nullopt;
        v[i] = *x;
    }
    Rect r{std::min(v[0], v[2]), std::min(v[1], v[3]), std::max(v[0], v[2]), std::max(v[1], v[3])};
    return r;
}

namespace {

// The bytes of a buffer that qpdf has just produced for us alone. qpdf 12 decodes into a std::string and wraps it in
// the Buffer: Buffer::move() hands that string over, where the qpdf at hand has it; otherwise the bytes are copied.
template <class B>
std::string take_bytes(B& buf) {
    if constexpr (requires { { buf.move() } -> std::same_as<std::string>; }) {
        return buf.move();
    } else {
        return std::string(reinterpret_cast<const char*>(buf.getBuffer()), buf.getSize());
    }
}

}  // namespace

std::optional<std::string> stream_data(const Obj& stream, std::string* error) {
    if (!stream.isStream()) return std::nullopt;
    try {
        // specialized: also RunLengthDecode, which the content rules need (at the generalized level such a stream is "unfilterable")
        auto buf = const_cast<Obj&>(stream).getStreamData(qpdf_dl_specialized);
        return take_bytes(*buf);
    } catch (std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

namespace {

constexpr std::size_t kPrefixCap = std::size_t(1) << 30;

// Inflates until the data fails; what came before stays. (qpdf drops the output of the Flate call that fails.)
std::string inflate_until_error(const std::string& raw) {
    std::string out;
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return out;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data()));
    zs.avail_in = static_cast<uInt>(raw.size());
    char buf[16384];
    int rc = Z_OK;
    while (rc == Z_OK && out.size() < kPrefixCap) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_SYNC_FLUSH);
        out.append(buf, sizeof buf - zs.avail_out);
        if (zs.avail_in == 0 && zs.avail_out != 0) break;
    }
    inflateEnd(&zs);
    return out;
}

std::string lzw_prefix(const std::string& in, const Obj& parms) {
    const int early = static_cast<int>(integer(parms, "EarlyChange").value_or(1));
    std::string out;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> table;  // (offset in out, length)
    table.reserve(4096);
    std::uint64_t bits = 0;
    int nbits = 0, width = 9;
    std::int64_t prev = -1;
    const auto emit_of = [&](std::int64_t code, std::uint32_t* off, std::uint32_t* len) {
        if (code < 256) return false;
        const auto& e = table[static_cast<std::size_t>(code - 258)];
        *off = e.first;
        *len = e.second;
        return true;
    };
    for (unsigned char byte : in) {
        bits = (bits << 8) | byte;
        nbits += 8;
        while (nbits >= width) {
            const auto code = static_cast<std::int64_t>((bits >> (nbits - width)) & ((1u << width) - 1));
            nbits -= width;
            if (code == 256) {
                table.clear();
                width = 9;
                prev = -1;
                continue;
            }
            if (code == 257) return out;
            const std::uint32_t start = static_cast<std::uint32_t>(out.size());
            const std::int64_t next = static_cast<std::int64_t>(table.size()) + 258;
            if (code < 256) {
                out.push_back(static_cast<char>(code));
            } else if (code < next) {
                std::uint32_t off = 0, len = 0;
                emit_of(code, &off, &len);
                for (std::uint32_t i = 0; i < len; ++i) out.push_back(out[off + i]);
            } else if (code == next && prev >= 0) {
                std::uint32_t off = start, len = 1;
                if (prev >= 256) emit_of(prev, &off, &len);
                else off = start - 1;
                for (std::uint32_t i = 0; i < len; ++i) out.push_back(out[off + i]);
                out.push_back(out[off]);
            } else {
                return out;  // the data fails here
            }
            if (prev >= 0 && table.size() < 4096 - 258) {
                std::uint32_t plen = 1;
                if (prev >= 256) plen = table[static_cast<std::size_t>(prev - 258)].second;
                table.emplace_back(start - plen, plen + 1);
            }
            const std::int64_t count = static_cast<std::int64_t>(table.size()) + 258 + early;
            width = count >= 2048 ? 12 : count >= 1024 ? 11 : count >= 512 ? 10 : 9;
            prev = code;
            if (out.size() >= kPrefixCap) return out;
        }
    }
    return out;
}

std::string ascii85_prefix(const std::string& in) {
    std::string out;
    std::uint32_t v = 0;
    int n = 0;
    for (unsigned char c : in) {
        if (std::isspace(c) || c == 0) continue;
        if (c == '~') break;
        if (c == 'z' && n == 0) {
            out.append(4, '\0');
            continue;
        }
        if (c < '!' || c > 'u') break;
        v = v * 85 + (c - '!');
        if (++n == 5) {
            for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((v >> (8 * k)) & 0xff));
            v = 0;
            n = 0;
        }
    }
    return out;
}

std::string asciihex_prefix(const std::string& in) {
    std::string out;
    int hi = -1;
    for (unsigned char c : in) {
        if (std::isspace(c) || c == 0) continue;
        if (c == '>') break;
        if (!std::isxdigit(c)) break;
        const int d = std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10;
        if (hi < 0) {
            hi = d;
        } else {
            out.push_back(static_cast<char>(hi * 16 + d));
            hi = -1;
        }
    }
    return out;
}

std::string runlength_prefix(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size() && out.size() < kPrefixCap;) {
        const auto l = static_cast<unsigned char>(in[i++]);
        if (l == 128) break;
        if (l < 128) {
            const std::size_t n = std::min<std::size_t>(l + 1u, in.size() - i);
            out.append(in, i, n);
            i += n;
        } else if (i < in.size()) {
            out.append(257u - l, in[i++]);
        }
    }
    return out;
}

bool apply_predictor(std::string& data, const Obj& parms) {
    return undo_predictor(data, integer(parms, "Predictor").value_or(1), integer(parms, "Colors").value_or(1), integer(parms, "BitsPerComponent").value_or(8),
                          integer(parms, "Columns").value_or(1));
}

}  // namespace

std::string inflate_prefix(const std::string& raw) { return inflate_until_error(raw); }

bool flate_checksum_failed(const Obj& stream, const std::string& decoded) {
    Obj filter = get(stream, "Filter");
    if (filter.isArray() && filter.getArrayNItems() == 1) filter = filter.getArrayItem(0);
    if (!filter.isName() || (filter.getName() != "/FlateDecode" && filter.getName() != "/Fl")) return false;
    std::shared_ptr<Buffer> raw;
    try {
        raw = const_cast<Obj&>(stream).getRawStreamData();
    } catch (std::exception&) {
        return false;
    }
    Obj parms = get(stream, "DecodeParms");
    if (parms.isArray() && parms.getArrayNItems() == 1) parms = parms.getArrayItem(0);
    const bool plain = integer(parms, "Predictor").value_or(1) <= 1;
    const unsigned char* b = raw->getBuffer();
    std::size_t n = raw->getSize();
    while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r')) --n;
    if (plain && n >= 4) {
        const uLong sum = adler32(adler32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(decoded.data()), static_cast<uInt>(decoded.size()));
        const uLong stored = (uLong(b[n - 4]) << 24) | (uLong(b[n - 3]) << 16) | (uLong(b[n - 2]) << 8) | uLong(b[n - 1]);
        if (sum == stored) return false;
    }
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(b);
    zs.avail_in = static_cast<uInt>(raw->getSize());
    unsigned char out[16384];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = out;
        zs.avail_out = sizeof out;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_BUF_ERROR) break;
    }
    const bool check = rc == Z_DATA_ERROR;
    inflateEnd(&zs);
    return check;
}

// ISO 32000-1 7.4.4.4: complete rows only. false: not a predictor this can apply.
bool undo_predictor(std::string& data, std::int64_t predictor, std::int64_t colors, std::int64_t bits, std::int64_t columns) {
    if (predictor <= 1) return true;
    if (colors < 1 || colors > 256 || columns < 1 || columns > (1 << 24) || (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16))
        return false;
    const std::size_t row = static_cast<std::size_t>((columns * colors * bits + 7) / 8);
    const std::size_t bpp = static_cast<std::size_t>(std::max<std::int64_t>(1, colors * bits / 8));
    std::string out;
    if (predictor == 2) {
        if (bits != 8) return false;
        out = data;
        out.resize(out.size() - out.size() % row);
        for (std::size_t r = 0; r + row <= out.size(); r += row)
            for (std::size_t i = bpp; i < row; ++i) out[r + i] = static_cast<char>(out[r + i] + out[r + i - bpp]);
        data = std::move(out);
        return true;
    }
    if (predictor < 10 || predictor > 15) return false;
    std::string prev(row, '\0'), cur(row, '\0');
    out.reserve(data.size());
    for (std::size_t at = 0; at + row + 1 <= data.size(); at += row + 1) {
        const auto tag = static_cast<unsigned char>(data[at]);
        if (tag > 4) break;
        for (std::size_t i = 0; i < row; ++i) {
            const int x = static_cast<unsigned char>(data[at + 1 + i]);
            const int a = i >= bpp ? static_cast<unsigned char>(cur[i - bpp]) : 0;
            const int b = static_cast<unsigned char>(prev[i]);
            const int c = i >= bpp ? static_cast<unsigned char>(prev[i - bpp]) : 0;
            int v = x;
            if (tag == 1) v = x + a;
            else if (tag == 2) v = x + b;
            else if (tag == 3) v = x + (a + b) / 2;
            else if (tag == 4) {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                v = x + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
            }
            cur[i] = static_cast<char>(v);
        }
        out.append(cur);
        prev.swap(cur);
    }
    data = std::move(out);
    return true;
}

std::string decodable_prefix(const Obj& stream) {
    Obj filter = get(stream, "Filter");
    Obj parms = get(stream, "DecodeParms");
    if (parms.isNull()) parms = get(stream, "DP");
    std::vector<Obj> filters = filter.isArray() ? array_items(filter) : filter.isName() ? std::vector<Obj>{filter} : std::vector<Obj>{};
    std::vector<Obj> params = parms.isArray() ? array_items(parms) : std::vector<Obj>{parms};
    if (filters.empty()) return {};
    std::string data;
    try {
        auto raw = const_cast<Obj&>(stream).getRawStreamData();
        data.assign(reinterpret_cast<const char*>(raw->getBuffer()), raw->getSize());
        for (std::size_t i = 0; i < filters.size(); ++i) {
            const std::string f = name(filters[i]).value_or("");
            const Obj p = i < params.size() ? params[i] : Obj::newNull();
            if (f == "FlateDecode" || f == "Fl") {
                data = inflate_until_error(data);
                if (!apply_predictor(data, p)) return {};
            } else if (f == "LZWDecode" || f == "LZW") {
                data = lzw_prefix(data, p);
                if (!apply_predictor(data, p)) return {};
            } else if (f == "ASCII85Decode" || f == "A85") {
                data = ascii85_prefix(data);
            } else if (f == "ASCIIHexDecode" || f == "AHx") {
                data = asciihex_prefix(data);
            } else if (f == "RunLengthDecode" || f == "RL") {
                data = runlength_prefix(data);
            } else if (f != "Crypt") {
                return {};
            }
            if (data.empty()) return {};
        }
    } catch (std::exception&) {
        return {};
    }
    return data;
}

namespace {

// Explicit stack: trees nest without a limit (an entry dropped here would read as absent). A node met again is
// skipped, so a cycle ends.
template <class Key, class Leaf>
void walk_tree(const Obj& root, std::string_view leaf_key, std::vector<std::pair<Key, Obj>>& out, Leaf leaf) {
    std::unordered_set<std::uint64_t> seen;
    std::vector<Obj> stack{root};
    while (!stack.empty()) {
        Obj node = stack.back();
        stack.pop_back();
        if (!node.isDictionary()) continue;
        if (node.isIndirect() && !seen.insert(ref_of(node).key()).second) continue;
        Obj leaves = get(node, leaf_key);
        if (leaves.isArray()) {
            int n = leaves.getArrayNItems();
            for (int i = 0; i + 1 < n; i += 2) {
                auto key = leaf(leaves.getArrayItem(i));
                if (key) out.emplace_back(*key, leaves.getArrayItem(i + 1));
            }
        }
        Obj kids = get(node, "Kids");
        if (kids.isArray())
            for (int i = kids.getArrayNItems() - 1; i >= 0; --i) stack.push_back(kids.getArrayItem(i));
    }
}

}  // namespace

std::vector<std::pair<std::string, Obj>> name_tree(const Obj& root) {
    std::vector<std::pair<std::string, Obj>> out;
    walk_tree<std::string>(root, "Names", out, [](const Obj& k) -> std::optional<std::string> { return bytes(k); });
    return out;
}

std::vector<std::pair<std::int64_t, Obj>> number_tree(const Obj& root) {
    std::vector<std::pair<std::int64_t, Obj>> out;
    walk_tree<std::int64_t>(root, "Nums", out, [](const Obj& k) -> std::optional<std::int64_t> { return integer(k); });
    return out;
}

}  // namespace eqx::cos
