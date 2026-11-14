// Pure text helpers of the pac_quality module (PAC 26.1 Quality-tab semantics). No PDF access here, so
// every helper is unit-tested in tests/unit/pac_quality_text_test.cpp.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eqx::pacq {

// A text of code points kept with 1, 2 or 4 bytes each, the narrowest that holds all of them (most marked content is
// Latin text, and short: up to 15 bytes need no allocation). Any 32-bit value is kept as it is.
class PackedText {
public:
    bool empty() const { return bytes_.empty(); }
    std::size_t size() const { return bytes_.size() >> shift_; }
    char32_t operator[](std::size_t i) const {
        const char* p = bytes_.data() + (i << shift_);
        if (shift_ == 0) return static_cast<unsigned char>(*p);
        if (shift_ == 1) {
            std::uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    char32_t front() const { return (*this)[0]; }
    char32_t back() const { return (*this)[size() - 1]; }

    void push_back(char32_t c) {
        const unsigned need = c < 0x100 ? 0 : c < 0x10000 ? 1 : 2;
        if (need > shift_) widen(need);
        if (shift_ == 0) {
            bytes_.push_back(static_cast<char>(c));
        } else if (shift_ == 1) {
            const std::uint16_t v = static_cast<std::uint16_t>(c);
            bytes_.append(reinterpret_cast<const char*>(&v), 2);
        } else {
            const std::uint32_t v = c;
            bytes_.append(reinterpret_cast<const char*>(&v), 4);
        }
    }
    // Appends the code points of a range (a string, a view, a text run).
    template <class Range>
    void append(const Range& r) {
        for (char32_t c : r) push_back(c);
    }
    // Appends `n` code points stored with (1 << shift) bytes each, the narrowest width that holds them all (the
    // form a text run keeps its text in).
    void append_packed(const unsigned char* data, std::size_t n, unsigned shift) {
        if (n == 0) return;
        if (shift > shift_) widen(shift);
        if (shift == shift_) {
            bytes_.append(reinterpret_cast<const char*>(data), n << shift);
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {   // narrower than this text
            char32_t c;
            if (shift == 0) {
                c = data[i];
            } else {
                std::uint16_t v;
                std::memcpy(&v, data + 2 * i, 2);
                c = v;
            }
            push_back(c);
        }
    }
    void append_to(std::u32string& out) const {
        const std::size_t n = size(), at = out.size();
        out.resize(at + n);
        char32_t* o = out.data() + at;
        if (shift_ == 0) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes_.data());
            for (std::size_t i = 0; i < n; ++i) o[i] = p[i];
        } else if (shift_ == 1) {
            for (std::size_t i = 0; i < n; ++i) {
                std::uint16_t v;
                std::memcpy(&v, bytes_.data() + 2 * i, 2);
                o[i] = v;
            }
        } else if (n) {
            std::memcpy(o, bytes_.data(), n * 4);
        }
    }
    std::u32string str() const {
        std::u32string out;
        append_to(out);
        return out;
    }
    // True when pred holds for every code point (also for an empty text).
    template <class Pred>
    bool all_of(Pred pred) const {
        for (std::size_t i = 0, n = size(); i < n; ++i)
            if (!pred((*this)[i])) return false;
        return true;
    }

private:
    void widen(unsigned shift) {
        const std::size_t n = size();
        std::string wide(n << shift, '\0');
        for (std::size_t i = 0; i < n; ++i) {
            const char32_t c = (*this)[i];
            if (shift == 1) {
                const std::uint16_t v = static_cast<std::uint16_t>(c);
                std::memcpy(wide.data() + 2 * i, &v, 2);
            } else {
                const std::uint32_t v = c;
                std::memcpy(wide.data() + 4 * i, &v, 4);
            }
        }
        bytes_.swap(wide);
        shift_ = static_cast<unsigned char>(shift);
    }

    std::string bytes_;
    unsigned char shift_ = 0;   // log2 of the bytes per code point
};

// .NET Char.IsWhiteSpace, which PAC documents for TextTagContainsOnlyWhitespace: Unicode
// categories Zs/Zl/Zp plus U+0009-U+000D, U+0085 (identical to the Unicode White_Space property).
bool is_whitespace(char32_t c);
// True when every code point is white space; an empty string counts as whitespace-only.
bool whitespace_only(std::u32string_view s);

// First "linkable string" PAC's MissingLinkTags check could mean, restricted to unambiguous
// forms (PAC: "URLs, email addresses"): an http:// or https:// URL with a host character after the
// scheme, a www.<label>.<tld> host, or an e-mail address local@domain.tld. Returns the match (UTF-8) or
// nullopt.
std::optional<std::string> find_linkable(std::u32string_view s);

// PAC AltTextsAreNotGenerated: alternative text that is a local path or a file name (PAC's
// example "C:\image.jpg"). Recognised (after trimming white space): drive paths (C:\..., C:/...), UNC
// paths (\\server\...), file: URLs, absolute or relative POSIX paths with a directory separator, and
// file names ending in a known image / document extension.
bool looks_like_path_or_file_name(std::string_view utf8);

// Normalisation used to match a TOC entry's text with a heading's text: lower case (ASCII and Latin-1),
// every run of white space collapsed to one space, leading/trailing white space, leader dots, page
// numbers and punctuation removed.
std::u32string normalize_toc_text(std::u32string_view s);
// True when the normalised texts are both non-empty and one contains the other.
bool toc_text_matches(std::u32string_view toc_entry, std::u32string_view heading);
// The forms of a text that toc_text_matches compares, for a text matched against many others (every heading of a
// document against every TOC entry): made once, then toc_text_matches(TocText, TocText) gives the same answer as the
// function above on the texts they were made from.
struct TocText {
    std::u32string normalized;     // normalize_toc_text
    std::u32string unhyphenated;   // ... without hyphens and soft hyphens (and the space after one)
};
TocText toc_text(std::u32string_view s);
bool toc_text_matches(const TocText& toc_entry, const TocText& heading);

// A text item for the ascent top of its sequence: top = ury - 0.8 * size + ascent(font) * size, the highest one wins
// (the first one, and after it every one that is greater than the top so far).
struct AscentTop {
    double size = 0;      // size_user of the run
    double ury = 0;       // top of the item's box
    std::uint32_t font = 0;   // AscentFonts index
};

// The text items of one sequence, reduced to those that can decide its ascent top. While every item has moderate
// coordinates (below 1e100: then every top is finite, for any ascent a font can have), only the highest box top
// per font and size is kept: for one font and size the top grows with the box top, and the highest of finite
// tops does not depend on their order. From the first item with other coordinates on, the items are kept in
// their order (the comparison with a NaN or infinite top depends on it); an item equal to the one before it adds
// nothing either way.
struct AscentTops {
    AscentTop first;
    bool any = false;
    bool in_order = false;
    std::vector<AscentTop> more;

    void add(std::uint32_t font, double size, double ury) {
        const AscentTop t{size, ury, font};
        if (!any) {
            first = t;
            any = true;
            in_order = !moderate(t);
            return;
        }
        if (!in_order && moderate(t)) {
            if (same_kind(first, t)) {
                if (t.ury > first.ury) first.ury = t.ury;
                return;
            }
            for (AscentTop& m : more)
                if (same_kind(m, t)) {
                    if (t.ury > m.ury) m.ury = t.ury;
                    return;
                }
            more.push_back(t);
            return;
        }
        in_order = true;
        const AscentTop& before = more.empty() ? first : more.back();
        if (same_kind(before, t) && bits(before.ury) == bits(t.ury)) return;
        more.push_back(t);
    }
    // The ascent top, with the ascent of each font from `ascent` (by AscentFonts index).
    bool resolve(const std::vector<double>& ascent, double& top_out) const {
        if (!any) return false;
        bool valid = false;
        double best = 0;
        const auto take = [&](const AscentTop& t) {
            const double top = t.ury - 0.8 * t.size + ascent[t.font] * t.size;
            if (!valid || top > best) best = top;
            valid = true;
        };
        take(first);
        for (const AscentTop& t : more) take(t);
        top_out = best;
        return true;
    }

private:
    static bool moderate(const AscentTop& t) { return std::fabs(t.ury) < 1e100 && t.size < 1e100; }
    static std::uint64_t bits(double v) {
        std::uint64_t b;
        std::memcpy(&b, &v, sizeof b);
        return b;
    }
    static bool same_kind(const AscentTop& a, const AscentTop& b) { return a.font == b.font && bits(a.size) == bits(b.size); }
};

}  // namespace eqx::pacq
