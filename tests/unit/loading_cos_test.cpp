// The cos:: accessors against their original implementation (kept here as the reference): the same answer for
// every kind of object and key - null, indirect null, dangling reference, scalar, array, stream, missing key, key
// with a null value, long key - and the same qpdf warnings afterwards.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>

#include <optional>
#include <string>
#include <vector>

#include "core/cos.h"
#include "unit.h"
#include "util/text.h"

using namespace eqx;

namespace {

// ---- reference: the implementation before the accessors were made cheaper -----------------------------------

namespace ref {

Obj dict_of(const Obj& h) {
    if (h.isDictionary()) return h;
    if (h.isStream()) return const_cast<Obj&>(h).getDict();
    return Obj::newNull();
}

Obj get(const Obj& h, std::string_view key) {
    Obj d = dict_of(h);
    if (!d.isDictionary()) return Obj::newNull();
    std::string k;
    k.reserve(key.size() + 1);
    k.push_back('/');
    k.append(key);
    if (!d.hasKey(k)) return Obj::newNull();
    return d.getKey(k);
}

bool has(const Obj& h, std::string_view key) {
    Obj d = dict_of(h);
    if (!d.isDictionary()) return false;
    std::string k = "/" + std::string(key);
    return d.hasKey(k) && !d.getKey(k).isNull();
}

std::optional<std::string> name(const Obj& h) {
    if (!h.isName()) return std::nullopt;
    std::string n = h.getName();
    if (!n.empty() && n[0] == '/') n.erase(0, 1);
    return n;
}

std::optional<std::string> name(const Obj& h, std::string_view key) { return name(get(h, key)); }

bool name_is(const Obj& h, std::string_view key, std::string_view value) {
    auto n = name(h, key);
    return n && *n == value;
}

std::optional<std::int64_t> integer(const Obj& h) {
    if (h.isInteger()) return h.getIntValue();
    return std::nullopt;
}

std::optional<double> number(const Obj& h) {
    if (h.isInteger()) return static_cast<double>(h.getIntValue());
    if (h.isReal()) return h.getNumericValue();
    return std::nullopt;
}

std::optional<bool> boolean(const Obj& h, std::string_view key) {
    Obj v = get(h, key);
    if (v.isBool()) return v.getBoolValue();
    return std::nullopt;
}

std::optional<std::string> bytes(const Obj& h) {
    if (h.isString()) return h.getStringValue();
    return std::nullopt;
}

std::optional<std::string> text(const Obj& h) {
    if (!h.isString()) return std::nullopt;
    return pdf_text_to_utf8(h.getStringValue());
}

std::vector<Obj> array_items(const Obj& h) {
    std::vector<Obj> out;
    if (!h.isArray()) return out;
    int n = const_cast<Obj&>(h).getArrayNItems();
    out.reserve(n);
    for (int i = 0; i < n; ++i) out.push_back(const_cast<Obj&>(h).getArrayItem(i));
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

std::optional<std::string> stream_data(const Obj& stream, std::string* error) {
    if (!stream.isStream()) return std::nullopt;
    try {
        auto buf = const_cast<Obj&>(stream).getStreamData(qpdf_dl_specialized);
        return std::string(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

}  // namespace ref

// ---- a document with every case -----------------------------------------------------------------------------

std::string test_pdf() {
    const std::vector<std::string> objects = {
        "",   // 0
        "<< /Type /Catalog /Pages 2 0 R /A 5 0 R /N null /Dangling 99 0 R /IndNull 6 0 R "
        "/Arr [1 2.5 /Nm (str) null 5 0 R 99 0 R 6 0 R [7 8] true] /Str 7 0 R /DirStr (direct \\(string\\)) "
        "/Name /Foo#20Bar /LongName /AVeryLongNameThatDoesNotFitTheSmallBuffer /Empty / /Bool true /Int 42 /IndInt 9 0 R "
        "/Real 1.5 /Nested << /X 1 /Y null /Z 6 0 R /Type /Inner /D << /Deep 3 >> >> /Stream 8 0 R "
        "/AVeryLongDictionaryKeyName 7 /Box [0 1 2.5 3] /BadBox [0 1 (x) 3] /RevBox [10 20 0 5] /IndBox 10 0 R "
        "/U16 <FEFF00480069> /Lang (de-DE) >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> >>",
        "",   // 4: free
        "<< /K 1 /Ref 3 0 R /S /P /Self 5 0 R >>",
        "null",
        "(hello)",
        "STREAM",
        "17",
        "[1 2 3 4]",
    };
    std::string out = "%PDF-1.7\n";
    std::vector<std::size_t> offsets(objects.size(), 0);
    for (std::size_t i = 1; i < objects.size(); ++i) {
        if (objects[i].empty()) continue;
        offsets[i] = out.size();
        out += std::to_string(i) + " 0 obj\n";
        if (objects[i] == "STREAM")
            out += "<< /Length 5 /Sub /Thing /NullKey null /Ind 6 0 R /Type /XObject >>\nstream\nhello\nendstream";
        else
            out += objects[i];
        out += "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size()) + "\n";
    for (std::size_t i = 0; i < objects.size(); ++i) {
        char line[32];
        if (i == 0 || objects[i].empty()) std::snprintf(line, sizeof line, "%010d %05d f \n", 0, i == 0 ? 65535 : 0);
        else std::snprintf(line, sizeof line, "%010zu %05d n \n", offsets[i], 0);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size()) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

struct Doc {
    std::string bytes = test_pdf();
    QPDF q;
    Doc() {
        q.setSuppressWarnings(true);
        q.processMemoryFile("cos-test", bytes.data(), bytes.size());
    }
    // The objects every accessor is tried on, in the same order for both documents.
    std::vector<Obj> subjects() {
        Obj cat = q.getRoot();
        std::vector<Obj> v = {cat, q.getTrailer(), Obj::newNull(), Obj()};
        for (const char* k : {"/Pages", "/A", "/N", "/Dangling", "/IndNull", "/Arr", "/Str", "/DirStr", "/Name", "/LongName", "/Empty",
                              "/Bool", "/Int", "/IndInt", "/Real", "/Nested", "/Stream", "/Box", "/BadBox", "/RevBox", "/IndBox",
                              "/U16", "/Lang", "/Missing"})
            v.push_back(cat.getKey(k));
        v.push_back(cat.getKey("/Nested").getKey("/D"));
        v.push_back(cat.getKey("/Stream").getDict());
        Obj arr = cat.getKey("/Arr");
        for (int i = 0; i < arr.getArrayNItems(); ++i) v.push_back(arr.getArrayItem(i));
        v.push_back(q.getObjectByID(3, 0));
        v.push_back(q.getObjectByID(4, 0));    // free object
        v.push_back(q.getObjectByID(50, 0));   // beyond the table
        return v;
    }
};

const char* const kKeys[] = {"Type", "Pages", "A", "N", "Dangling", "IndNull", "Arr", "Str", "DirStr", "Name", "LongName", "Empty",
                             "Bool", "Int", "IndInt", "Real", "Nested", "Stream", "AVeryLongDictionaryKeyName", "Box", "Missing", "",
                             "K", "Ref", "S", "Self", "Length", "Sub", "NullKey", "Ind", "X", "Y", "Z", "D", "Deep", "Root", "Size",
                             "Kids", "Count", "Parent", "MediaBox", "Resources", "U16", "Lang", "type", "/Type"};

// One line that says everything observable about a handle.
std::string describe(const Obj& h) {
    if (!h) return "uninitialized";
    std::string s = std::string(h.getTypeName()) + (h.isIndirect() ? " indirect " + ref_of(h).str() : " direct");
    return s + " " + const_cast<Obj&>(h).unparse();
}

template <class T>
std::string describe(const std::optional<T>& v) {
    if (!v) return "nullopt";
    std::ostringstream o;
    o << *v;
    return o.str();
}

std::string describe(const std::optional<Rect>& r) {
    if (!r) return "nullopt";
    std::ostringstream o;
    o << r->llx << " " << r->lly << " " << r->urx << " " << r->ury;
    return o.str();
}

std::string describe(const std::vector<Obj>& v) {
    std::string s = std::to_string(v.size()) + ":";
    for (const Obj& o : v) s += " [" + describe(o) + "]";
    return s;
}

std::string warnings(QPDF& q) {
    std::string s;
    for (auto& w : q.getWarnings()) s += std::string(w.what()) + "\n";
    return s;
}

}  // namespace

#define SAME(expr_new, expr_ref, what)                                                                             \
    do {                                                                                                           \
        const std::string got = describe(expr_new), want = describe(expr_ref);                                    \
        if (got != want) {                                                                                         \
            eqx_test::fail(__FILE__, __LINE__, std::string(what) + ": got {" + got + "} want {" + want + "}");     \
        }                                                                                                          \
    } while (0)

EQX_TEST(cos_accessors_match_reference) {
    Doc a, b;   // a: the accessors under test; b: the reference. Separate documents: no shared resolution state.
    std::vector<Obj> sa = a.subjects(), sb = b.subjects();
    EXPECT_EQ(sa.size(), sb.size());
    for (std::size_t i = 0; i < sa.size(); ++i) {
        const std::string subject = "subject " + std::to_string(i) + " ";
        SAME(sa[i], sb[i], subject + "itself");
        SAME(cos::dict_of(sa[i]), ref::dict_of(sb[i]), subject + "dict_of");
        SAME(cos::name(sa[i]), ref::name(sb[i]), subject + "name");
        SAME(cos::integer(sa[i]), ref::integer(sb[i]), subject + "integer");
        SAME(cos::number(sa[i]), ref::number(sb[i]), subject + "number");
        SAME(cos::bytes(sa[i]), ref::bytes(sb[i]), subject + "bytes");
        SAME(cos::text(sa[i]), ref::text(sb[i]), subject + "text");
        SAME(cos::array_items(sa[i]), ref::array_items(sb[i]), subject + "array_items");
        SAME(cos::rect(sa[i]), ref::rect(sb[i]), subject + "rect");
        std::string ea, eb;
        SAME(cos::stream_data(sa[i], &ea), ref::stream_data(sb[i], &eb), subject + "stream_data");
        EXPECT_EQ(ea, eb);
        for (const char* key : kKeys) {
            const std::string at = subject + "key '" + key + "' ";
            SAME(cos::get(sa[i], key), ref::get(sb[i], key), at + "get");
            EXPECT_EQ(cos::has(sa[i], key), ref::has(sb[i], key));
            SAME(cos::name(sa[i], key), ref::name(sb[i], key), at + "name");
            SAME(cos::integer(sa[i], key), ref::integer(ref::get(sb[i], key)), at + "integer");
            SAME(cos::number(sa[i], key), ref::number(ref::get(sb[i], key)), at + "number");
            SAME(cos::boolean(sa[i], key), ref::boolean(sb[i], key), at + "boolean");
            SAME(cos::bytes(sa[i], key), ref::bytes(ref::get(sb[i], key)), at + "bytes");
            SAME(cos::text(sa[i], key), ref::text(ref::get(sb[i], key)), at + "text");
            for (const char* value : {"Catalog", "Foo Bar", "Thing", "", "Pages", "P", "Inner", "XObject", "/Catalog",
                                      "AVeryLongNameThatDoesNotFitTheSmallBuffer"})
                if (cos::name_is(sa[i], key, value) != ref::name_is(sb[i], key, value))
                    eqx_test::fail(__FILE__, __LINE__, at + "name_is " + value);
            // what a lookup returns is the object itself, not a copy: a present value keeps its identity
            Obj va = cos::get(sa[i], key), vb = ref::get(sb[i], key);
            if (!vb.isNull()) EXPECT(va.isSameObjectAs(cos::get(sa[i], key)));
            // a missing or null value is always a direct null, never the dangling reference
            if (vb.isNull()) EXPECT(va.isNull() && !va.isIndirect());
        }
    }
    EXPECT_EQ(warnings(a.q), warnings(b.q));
}

EQX_TEST(cos_names_without_slash) {
    // qpdf lets a program build a name without the leading slash: it is returned as it is
    Obj d = QPDFObjectHandle::newDictionary();
    d.replaceKey("/K", QPDFObjectHandle::newName("NoSlash"));
    d.replaceKey("/L", QPDFObjectHandle::newName(""));
    EXPECT(cos::name(d, "K") == ref::name(d, "K"));
    EXPECT(cos::name(d, "L") == ref::name(d, "L"));
    for (const char* v : {"NoSlash", "/NoSlash", "oSlash", ""}) {
        EXPECT_EQ(cos::name_is(d, "K", v), ref::name_is(d, "K", v));
        EXPECT_EQ(cos::name_is(d, "L", v), ref::name_is(d, "L", v));
    }
}

EQX_TEST(cos_sparse_and_nested_arrays) {
    Obj arr = QPDFObjectHandle::parse("[1 null null null null null null null null null null null null null null null null "
                                      "null null null null null null null null null null null null null null null null null 2]");
    SAME(cos::array_items(arr), ref::array_items(arr), "sparse array");
    Obj empty = QPDFObjectHandle::parse("[]");
    SAME(cos::array_items(empty), ref::array_items(empty), "empty array");
    Obj nested = QPDFObjectHandle::parse("[[1 2] << /A [3] >> (s) /N 1.5 true null]");
    SAME(cos::array_items(nested), ref::array_items(nested), "nested array");
}

EQX_TEST(cos_array_items_keep_identity_and_nulls) {
    // The items are the array's own objects, and nulls are what getArrayItem makes of them: the same pattern of
    // "is the same object" among the items and against getArrayItem, for every kind of array.
    Doc a;
    Obj shared = QPDFObjectHandle::newInteger(7);
    Obj null_object = QPDFObjectHandle::newNull();
    Obj built = QPDFObjectHandle::newArray({shared, null_object, shared, null_object, QPDFObjectHandle::newName("/N")});
    const std::vector<Obj> arrays = {
        QPDFObjectHandle::parse("[1 2 3]"),
        QPDFObjectHandle::parse("[1 null 3 null]"),
        QPDFObjectHandle::parse("[null]"),
        QPDFObjectHandle::parse("[null null null null null null null null null null null null null null null null null null "
                                "null null null null null null null null null null null null null null null null 5 null]"),
        QPDFObjectHandle::parse("[[1 null] << /A null >> null (s)]"),
        built,
        QPDFObjectHandle::newArray(),
        a.q.getRoot().getKey("/Arr"),      // direct numbers, a name, a string, null, references (also dangling), an array
        a.q.getRoot().getKey("/IndBox"),   // an indirect array
        a.q.getRoot().getKey("/Int"),      // no array
    };
    for (std::size_t k = 0; k < arrays.size(); ++k) {
        const Obj& arr = arrays[k];
        const std::vector<Obj> items = cos::array_items(arr), expected = ref::array_items(arr);
        SAME(items, expected, "array " + std::to_string(k));
        if (items.size() != expected.size()) continue;
        for (std::size_t i = 0; i < items.size(); ++i) {
            EXPECT_EQ(items[i].isSameObjectAs(const_cast<Obj&>(arr).getArrayItem(static_cast<int>(i))),
                      expected[i].isSameObjectAs(const_cast<Obj&>(arr).getArrayItem(static_cast<int>(i))));
            EXPECT_EQ(items[i].isIndirect(), expected[i].isIndirect());
            for (std::size_t j = 0; j < items.size(); ++j)
                EXPECT_EQ(items[i].isSameObjectAs(items[j]), expected[i].isSameObjectAs(expected[j]));
        }
    }
    // nothing was resolved by listing the items: the dangling reference is still only a reference
    EXPECT(warnings(a.q).empty());
}
