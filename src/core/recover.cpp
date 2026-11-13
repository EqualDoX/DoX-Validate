#include "core/recover.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDFExc.hh>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/cos.h"

namespace eqx {

namespace {

struct Entry {
    std::uint64_t pos = 0;    // file order of the definition
    std::uint64_t offset = 0;
    int gen = 0;
    std::uint32_t stream = 0;  // object stream, 0 = top level
    std::uint32_t index = 0;
};

bool ws(unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0; }

// "N G obj" before position `at` of the keyword. Returns false when the bytes before it are no such header.
bool header_before(const std::string& b, std::size_t at, std::uint64_t base, std::uint32_t* num, int* gen, std::uint64_t* offset) {
    std::size_t i = at;
    auto back_ws = [&]() {
        std::size_t s = i;
        while (i > 0 && ws(static_cast<unsigned char>(b[i - 1]))) --i;
        return s != i;
    };
    auto back_int = [&](std::uint64_t* v) {
        std::size_t e = i;
        while (i > 0 && std::isdigit(static_cast<unsigned char>(b[i - 1])) && e - i < 11) --i;
        if (i == e) return false;
        std::uint64_t x = 0;
        for (std::size_t k = i; k < e; ++k) x = x * 10 + static_cast<unsigned>(b[k] - '0');
        *v = x;
        return true;
    };
    std::uint64_t g = 0, n = 0;
    if (!back_ws() || !back_int(&g) || !back_ws() || !back_int(&n)) return false;
    if (n == 0 || n > 8388607 || g > 65535) return false;
    if (i > 0 && std::isdigit(static_cast<unsigned char>(b[i - 1]))) return false;  // "endobj12 0 obj" exists
    if (i < base) return false;
    *num = static_cast<std::uint32_t>(n);
    *gen = static_cast<int>(g);
    *offset = i - base;
    return true;
}

void put_be(std::string& out, std::uint64_t v, int width) {
    for (int k = width - 1; k >= 0; --k) out.push_back(static_cast<char>((v >> (8 * k)) & 0xff));
}

std::string xref_stream(std::uint32_t self, std::uint64_t self_offset, const std::map<std::uint32_t, Entry>& table,
                        const std::string& keep) {
    std::string data;
    data.reserve((self + 1) * 11);
    for (std::uint32_t n = 0; n <= self; ++n) {
        auto it = table.find(n);
        if (n == self) {
            data.push_back(1);
            put_be(data, self_offset, 8);
            put_be(data, 0, 2);
        } else if (it == table.end()) {
            data.push_back(0);
            put_be(data, 0, 8);
            put_be(data, n == 0 ? 65535 : 0, 2);
        } else if (it->second.stream) {
            data.push_back(2);
            put_be(data, it->second.stream, 8);
            put_be(data, it->second.index, 2);
        } else {
            data.push_back(1);
            put_be(data, it->second.offset, 8);
            put_be(data, static_cast<std::uint64_t>(it->second.gen), 2);
        }
    }
    std::string s = std::to_string(self) + " 0 obj\n<</Type/XRef/Size " + std::to_string(self + 1) + "/W[1 8 2]" + keep +
                    "/Length " + std::to_string(data.size()) + ">>\nstream\n";
    s += data;
    s += "\nendstream\nendobj\n";
    return s;
}

std::string with_table(const std::string& bytes, std::uint64_t base, const std::map<std::uint32_t, Entry>& table,
                       const std::string& keep) {
    std::uint32_t self = 1;
    if (!table.empty()) self = table.rbegin()->first + 1;
    std::string image = bytes;
    image += "\n";
    const std::uint64_t at = image.size() - base;
    image += xref_stream(self, at, table, keep);
    image += "startxref\n" + std::to_string(at) + "\n%%EOF\n";
    return image;
}

// A value in dictionary text: "N G R", a balanced array or dictionary (strings respected). Empty when it is none of these.
std::string value_at(const std::string& t, std::size_t p) {
    while (p < t.size() && ws(static_cast<unsigned char>(t[p]))) ++p;
    if (p >= t.size()) return {};
    if (std::isdigit(static_cast<unsigned char>(t[p]))) {
        const std::size_t r = t.find('R', p);
        if (r == std::string::npos || r - p > 24) return {};
        for (std::size_t i = p; i < r; ++i)
            if (!std::isdigit(static_cast<unsigned char>(t[i])) && !ws(static_cast<unsigned char>(t[i]))) return {};
        return t.substr(p, r + 1 - p);
    }
    if (t[p] != '[' && t[p] != '<') return {};
    int depth = 0, paren = 0;
    for (std::size_t i = p; i < t.size(); ++i) {
        const char c = t[i];
        if (paren) {
            if (c == '\\') ++i;
            else if (c == '(') ++paren;
            else if (c == ')') --paren;
        } else if (c == '(') {
            paren = 1;
        } else if (c == '[' || c == '<') {
            ++depth;
        } else if (c == ']' || c == '>') {
            if (--depth == 0) return t.substr(p, i + 1 - p);
        }
    }
    return {};
}

// The entries of the last trailer dictionary or cross-reference stream dictionary with a /Root, as text.
std::string last_trailer_keys(const std::string& bytes) {
    for (std::size_t at = bytes.rfind("/Root"); at != std::string::npos && at > 0; at = bytes.rfind("/Root", at - 1)) {
        const std::string root = value_at(bytes, at + 5);
        if (root.empty() || !std::isdigit(static_cast<unsigned char>(root[0]))) continue;
        const std::size_t from = at > 8192 ? at - 8192 : 0;
        const std::size_t tr = bytes.rfind("trailer", at);
        const std::size_t ob = bytes.rfind(" obj", at);
        std::size_t open = std::string::npos;
        if (tr != std::string::npos && tr >= from && (ob == std::string::npos || ob < tr)) open = bytes.find("<<", tr);
        else if (ob != std::string::npos && ob >= from) open = bytes.find("<<", ob);
        if (open == std::string::npos || open > at) continue;
        const std::string dict = value_at(bytes, open);
        if (dict.empty()) continue;
        std::string keep;
        for (const char* k : {"/Info", "/ID", "/Encrypt"}) {
            const std::size_t kp = dict.find(k);
            if (kp == std::string::npos) continue;
            const std::string v = value_at(dict, kp + std::strlen(k));
            if (!v.empty()) keep += std::string(k) + " " + v;
        }
        return keep + "/Root " + root;
    }
    return {};
}

std::optional<std::int64_t> int_key(const std::string& dict, const char* key) {
    const std::size_t k = dict.find(key);
    if (k == std::string::npos) return std::nullopt;
    std::size_t p = k + std::strlen(key);
    while (p < dict.size() && ws(static_cast<unsigned char>(dict[p]))) ++p;
    const std::size_t s = p;
    std::int64_t v = 0;
    while (p < dict.size() && std::isdigit(static_cast<unsigned char>(dict[p])) && p - s < 18) v = v * 10 + (dict[p++] - '0');
    if (p == s) return std::nullopt;
    return v;
}

std::vector<std::int64_t> int_array(const std::string& dict, const char* key) {
    std::vector<std::int64_t> out;
    const std::size_t k = dict.find(key);
    if (k == std::string::npos) return out;
    std::size_t p = dict.find('[', k);
    const std::size_t end = dict.find(']', k);
    if (p == std::string::npos || end == std::string::npos || p > end) return out;
    while (++p < end) {
        if (!std::isdigit(static_cast<unsigned char>(dict[p]))) continue;
        std::int64_t v = 0;
        while (p < end && std::isdigit(static_cast<unsigned char>(dict[p]))) v = v * 10 + (dict[p++] - '0');
        out.push_back(v);
    }
    return out;
}

struct Row {
    int type = 0;
    std::uint64_t a = 0, b = 0;
};

// The rows of an unencrypted, Flate or unfiltered cross-reference stream (these are never encrypted, 7.6.1). false: not read.
bool xref_stream_rows(const std::string& bytes, std::size_t at, std::map<std::uint32_t, Row>* rows) {
    const std::size_t ob = bytes.find("obj", at);
    if (ob == std::string::npos || ob > at + 32) return false;
    const std::size_t open = bytes.find("<<", ob);
    if (open == std::string::npos || open > ob + 16) return false;
    const std::string dict = value_at(bytes, open);
    if (dict.find("/XRef") == std::string::npos) return false;
    std::size_t st = open + dict.size();
    st = bytes.find("stream", st);
    if (st == std::string::npos || st > open + dict.size() + 8) return false;
    st += 6;
    if (st < bytes.size() && bytes[st] == '\r') ++st;
    if (st < bytes.size() && bytes[st] == '\n') ++st;
    auto len = int_key(dict, "/Length");
    const std::size_t close = bytes.find("endstream", st);
    if (close == std::string::npos) return false;
    if (!len || st + static_cast<std::size_t>(*len) > close) len = static_cast<std::int64_t>(close - st);
    std::string data = bytes.substr(st, static_cast<std::size_t>(*len));
    if (dict.find("/Filter") != std::string::npos) {
        if (dict.find("/FlateDecode") == std::string::npos && dict.find("/Fl") == std::string::npos) return false;
        data = cos::inflate_prefix(data);
    }
    if (auto pr = int_key(dict, "/Predictor"); pr && *pr > 1) {
        auto col = int_key(dict, "/Columns");
        if (!cos::undo_predictor(data, *pr, 1, 8, col.value_or(1))) return false;
    }
    const std::vector<std::int64_t> w = int_array(dict, "/W");
    if (w.size() != 3 || w[0] > 8 || w[1] > 8 || w[2] > 8) return false;
    std::vector<std::int64_t> index = int_array(dict, "/Index");
    if (index.empty()) index = {0, int_key(dict, "/Size").value_or(0)};
    const std::size_t row = static_cast<std::size_t>(w[0] + w[1] + w[2]);
    if (row == 0) return false;
    std::size_t p = 0;
    for (std::size_t i = 0; i + 1 < index.size(); i += 2)
        for (std::int64_t k = 0; k < index[i + 1] && p + row <= data.size(); ++k, p += row) {
            auto field = [&](std::size_t from, std::int64_t width) {
                std::uint64_t v = 0;
                for (std::int64_t j = 0; j < width; ++j) v = (v << 8) | static_cast<unsigned char>(data[from + static_cast<std::size_t>(j)]);
                return v;
            };
            Row r;
            r.type = w[0] == 0 ? 1 : static_cast<int>(field(p, w[0]));
            r.a = field(p + static_cast<std::size_t>(w[0]), w[1]);
            r.b = field(p + static_cast<std::size_t>(w[0] + w[1]), w[2]);
            (*rows)[static_cast<std::uint32_t>(index[i] + k)] = r;
        }
    return true;
}

}  // namespace

std::string hidden_objects_image(const std::string& bytes, QPDFObjectHandle trailer, const std::map<QPDFObjGen, QPDFXRefEntry>& known) {
    const std::size_t hdr = bytes.find("%PDF-");
    const std::uint64_t base = (hdr != std::string::npos && hdr < 1024) ? hdr : 0;
    const std::size_t sx = bytes.rfind("startxref");
    if (sx == std::string::npos) return {};
    auto top = int_key(bytes.substr(sx, 40), "startxref");
    if (!top) return {};
    std::unordered_map<std::uint32_t, bool> found;
    for (const auto& kv : known) found[static_cast<std::uint32_t>(kv.first.getObj())] = true;
    std::map<std::uint32_t, Row> revive;
    std::unordered_map<std::uint32_t, bool> newer;
    std::vector<std::int64_t> seen;
    for (std::optional<std::int64_t> off = top; off && seen.size() < 4096;) {
        if (std::find(seen.begin(), seen.end(), *off) != seen.end()) break;
        seen.push_back(*off);
        std::size_t at = static_cast<std::size_t>(*off) + base;
        if (at + 4 >= bytes.size() || bytes.compare(at, 4, "xref") != 0) break;  // a pure cross-reference stream section: not hybrid
        const std::size_t tr = bytes.find("trailer", at);
        if (tr == std::string::npos) break;
        // The table: 20-byte grid, but entries are read by their tokens: this reads a valid file only.
        std::unordered_map<std::uint32_t, bool> free_here, used_here;
        std::size_t p = at + 4;
        auto token = [&]() {
            while (p < tr && ws(static_cast<unsigned char>(bytes[p]))) ++p;
            const std::size_t s = p;
            while (p < tr && !ws(static_cast<unsigned char>(bytes[p]))) ++p;
            return bytes.substr(s, p - s);
        };
        bool ok = true;
        while (ok) {
            std::string a = token();
            if (a.empty()) break;
            std::string b = token();
            if (b.empty() || !std::isdigit(static_cast<unsigned char>(a[0])) || !std::isdigit(static_cast<unsigned char>(b[0]))) { ok = false; break; }
            const auto first = static_cast<std::uint32_t>(std::strtoul(a.c_str(), nullptr, 10));
            const auto count = static_cast<std::uint32_t>(std::strtoul(b.c_str(), nullptr, 10));
            for (std::uint32_t i = 0; i < count; ++i) {
                token();
                token();
                std::string t = token();
                if (t == "n") used_here[first + i] = true;
                else if (t == "f") free_here[first + i] = true;
                else { ok = false; break; }
            }
        }
        if (!ok) return {};
        const std::string dict = value_at(bytes, bytes.find("<<", tr));
        if (auto xs = int_key(dict, "/XRefStm")) {
            std::map<std::uint32_t, Row> rows;
            const std::size_t xat = static_cast<std::size_t>(*xs) + base;
            if (xat < bytes.size() && xref_stream_rows(bytes, xat, &rows))
                for (const auto& [num, r] : rows) {
                    if (r.type == 0 || newer.count(num) || used_here.count(num)) continue;
                    if (free_here.count(num) && !found.count(num)) revive[num] = r;
                }
            for (const auto& [num, r] : rows)
                if (r.type != 0) newer[num] = true;
        }
        for (const auto& kv : used_here) newer[kv.first] = true;
        for (const auto& kv : free_here) newer[kv.first] = true;
        off = int_key(dict, "/Prev");
    }
    if (revive.empty()) return {};
    std::string keep;
    for (const char* k : {"/Info", "/ID", "/Encrypt", "/Root"})
        if (trailer.hasKey(k)) keep += std::string(k) + " " + trailer.getKey(k).unparse();
    std::uint32_t self = revive.rbegin()->first + 1;
    for (const auto& kv : known) self = std::max(self, static_cast<std::uint32_t>(kv.first.getObj()) + 1);
    std::string image = bytes + "\n";
    const std::uint64_t at = image.size() - base;
    std::string data, index;
    revive[self] = Row{1, at, 0};
    for (const auto& [num, r] : revive) {
        index += std::to_string(num) + " 1 ";
        data.push_back(static_cast<char>(r.type));
        put_be(data, r.a, 8);
        put_be(data, r.b, 2);
    }
    image += std::to_string(self) + " 0 obj\n<</Type/XRef/Size " + std::to_string(self + 1) + "/W[1 8 2]/Index[" + index + "]/Prev " + std::to_string(*top) + keep +
             "/Length " + std::to_string(data.size()) + ">>\nstream\n" + data + "\nendstream\nendobj\nstartxref\n" + std::to_string(at) + "\n%%EOF\n";
    return image;
}

std::string recovered_image(const std::string& bytes, QPDFObjectHandle trailer) {
    const std::size_t hdr = bytes.find("%PDF-");
    const std::uint64_t base = (hdr != std::string::npos && hdr < 1024) ? hdr : 0;
    std::string keep, root;
    if (trailer.isDictionary() && trailer.hasKey("/Root")) {
        for (const char* k : {"/Info", "/ID", "/Encrypt"})
            if (trailer.hasKey(k)) keep += std::string(k) + " " + trailer.getKey(k).unparse();
        root = "/Root " + trailer.getKey("/Root").unparse();
    } else {
        keep = last_trailer_keys(bytes);
        const std::size_t r = keep.rfind("/Root ");
        if (r == std::string::npos) return {};
        root = keep.substr(r);
        keep.resize(r);
    }
    std::map<std::uint32_t, Entry> table;
    for (std::size_t at = bytes.find("obj"); at != std::string::npos; at = bytes.find("obj", at + 3)) {
        if (at >= 3 && std::memcmp(bytes.data() + at - 3, "end", 3) == 0) continue;
        std::uint32_t num = 0;
        int gen = 0;
        std::uint64_t off = 0;
        if (!header_before(bytes, at, base, &num, &gen, &off)) continue;
        Entry e;
        e.pos = off;
        e.offset = off;
        e.gen = gen;
        table[num] = e;  // later in the file: later definition
        // A stream body can hold anything, embedded files with their own objects too: go on behind it.
        const std::size_t end = bytes.find("endobj", at);
        for (std::size_t st = bytes.find("stream", at); st != std::string::npos && (end == std::string::npos || st < end);
             st = bytes.find("stream", st + 6)) {
            std::size_t before = st;
            while (before > at && ws(static_cast<unsigned char>(bytes[before - 1]))) --before;
            const bool opens = before >= at + 2 && bytes[before - 1] == '>' && bytes[before - 2] == '>' && st + 6 < bytes.size() &&
                               (bytes[st + 6] == '\r' || bytes[st + 6] == '\n');
            if (!opens) continue;
            const std::size_t close = bytes.find("endstream", st);
            if (close != std::string::npos) at = close;
            break;
        }
    }
    if (table.empty()) return {};
    // First pass: top-level objects only, which finds the object streams with a working qpdf (decryption included). The
    // catalog can itself lie in an object stream and qpdf wants a page tree on opening: a placeholder stands in here.
    const std::uint32_t top = table.rbegin()->first;
    std::string stub = "\n";
    const auto stub_at = [&](std::uint32_t num, const std::string& body) {
        Entry e;
        e.pos = e.offset = bytes.size() + stub.size() - base;
        stub += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
        return e;
    };
    const std::uint32_t cat = top + 1, tree = top + 2, leaf = top + 3;
    std::map<std::uint32_t, Entry> probe = table;
    probe[cat] = stub_at(cat, "<</Type/Catalog/Pages " + std::to_string(tree) + " 0 R>>");
    probe[tree] = stub_at(tree, "<</Type/Pages/Count 1/Kids[" + std::to_string(leaf) + " 0 R]>>");
    probe[leaf] = stub_at(leaf, "<</Type/Page/MediaBox[0 0 1 1]/Parent " + std::to_string(tree) + " 0 R>>");
    std::string first = with_table(bytes + stub, base, probe, keep + "/Root " + std::to_string(cat) + " 0 R");
    std::map<std::uint32_t, Entry> full = table;
    try {
        QPDF q;
        q.setSuppressWarnings(true);
        q.processMemoryFile("eqx recovery", first.data(), first.size());
        std::vector<std::pair<std::uint64_t, std::uint32_t>> streams;
        for (const auto& [num, e] : table) streams.emplace_back(e.pos, num);
        std::sort(streams.begin(), streams.end());
        for (const auto& [pos, num] : streams) {
            QPDFObjectHandle o = q.getObjectByID(static_cast<int>(num), table[num].gen);
            if (!o.isStream()) continue;
            QPDFObjectHandle d = o.getDict();
            auto type = cos::name(d, "Type");
            if (!type || *type != "ObjStm") continue;
            auto n = cos::integer(d, "N");
            if (!n || *n <= 0 || *n > 8388607) continue;
            std::string data;
            try {
                auto buf = o.getStreamData(qpdf_dl_generalized);
                data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
            } catch (std::exception&) {
                continue;  // a damaged object stream: its objects stay unread, as qpdf would refuse them too
            }
            std::size_t p = 0;
            for (std::int64_t i = 0; i < *n; ++i) {
                std::uint64_t vals[2] = {0, 0};
                bool ok = true;
                for (auto& v : vals) {
                    while (p < data.size() && ws(static_cast<unsigned char>(data[p]))) ++p;
                    std::size_t s = p;
                    while (p < data.size() && std::isdigit(static_cast<unsigned char>(data[p])) && p - s < 12)
                        v = v * 10 + static_cast<unsigned>(data[p++] - '0');
                    ok = ok && p != s;
                }
                if (!ok) break;
                if (vals[0] == 0 || vals[0] > 8388607 || i > 65535) continue;
                auto cur = full.find(static_cast<std::uint32_t>(vals[0]));
                if (cur != full.end() && cur->second.pos > pos) continue;
                Entry e;
                e.pos = pos;
                e.stream = num;
                e.index = static_cast<std::uint32_t>(i);
                full[static_cast<std::uint32_t>(vals[0])] = e;
            }
        }
    } catch (std::exception&) {
        return {};
    }
    return with_table(bytes, base, full, keep + root);
}

}  // namespace eqx
