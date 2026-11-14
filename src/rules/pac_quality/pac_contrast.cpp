#include "rules/pac_quality/pac_contrast.h"

#include <qpdf/Buffer.hh>
#include <qpdf/Pipeline.hh>
#include <qpdf/QPDF.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#if defined(__GLIBC__)
#include <malloc.h>   // malloc_trim
#endif

#include "rules/pac_quality/ccitt_decode.h"
#include "rules/pac_quality/jpx_reduce.h"
#include "rules/pac_quality/wcag_color.h"
#include "util/text.h"

namespace eqx::pacc {

namespace {

constexpr int kMaxImageSide = 512;          // reduced images (per side)
constexpr int kMaxCanvasSide = 2000;        // canvas limit (posters get fewer pixels per point)
constexpr std::size_t kMaxPathPoints = 4'000'000;   // geometry kept per page; beyond it the page is not judged
constexpr std::size_t kMaxImageCache = 32;
// Reduced images kept for later pages (least recently used first out): enough for the largest reduced image (512 x 512, colour and
// opacity, 1 MiB) and a few small ones, so a background or logo repeated on every page is decoded once; an image used on one page
// only (most) no longer stays for the rest of the document (a real-world sample: 7.3 MB held at the end with 32 slots).
constexpr std::size_t kMaxImageCacheBytes = 1'536 * 1024;
constexpr std::size_t kMaxClipMaskBytes = 32'000'000;   // pixels of cached clip masks per page (one bit each: 4 MB)
constexpr std::size_t kTrimMaskPixels = 4'000'000;      // clip-mask pixels on a page after which freed heap is given back
constexpr std::int64_t kPixelBudget = 1'500'000'000;   // pixels touched per document; beyond it nothing more is judged

double clamp01(double v) { return !(v > 0) ? 0 : v > 1 ? 1 : v; }
unsigned char to8(double v) { return static_cast<unsigned char>(std::lround(clamp01(v) * 255)); }

double channel(double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double luminance8(const unsigned char* p) {
    static const std::vector<double> table = [] {
        std::vector<double> t(256);
        for (int i = 0; i < 256; ++i) t[i] = channel(i / 255.0);
        return t;
    }();
    return 0.2126 * table[p[0]] + 0.7152 * table[p[1]] + 0.0722 * table[p[2]];
}

// --- functions and colour spaces ----------------------------------------------------------------------------

// PDF function of type 4 (ISO 32000-1 7.10.5): a small PostScript calculator, compiled to a flat token list.
class Calculator {
public:
    // false on a syntax error or an unknown operator
    bool parse(std::string_view src) {
        std::size_t at = 0;
        skip(src, at);
        if (at >= src.size() || src[at] != '{') return false;
        ++at;
        return block(src, at, 0);
    }
    bool run(std::vector<double>& st) const {
        int budget = 100000;
        return exec(0, code_.size(), st, budget);
    }

private:
    enum Kind : std::uint8_t { Num, Oper, If, IfElse };
    // In the order of `kOpNames`. Evaluated per pixel of an image in a tint-transformed colour space.
    enum class Op : std::uint8_t { abs_, add_, atan_, ceiling_, cos_, cvi_, cvr_, div_, exp_, floor_, idiv_, ln_, log_, mod_, mul_, neg_, round_, sin_, sqrt_, sub_, truncate_, and_, bitshift_, eq_, false_, ge_, gt_, le_, lt_, ne_, not_, or_, true_, xor_, copy_, dup_, exch_, index_, pop_, roll_, jump_ };
    struct Tok {
        Kind kind = Num;
        double v = 0;
        Op op = Op::jump_;
        std::size_t a = 0, b = 0, c = 0;   // If: [a, b); IfElse: [a, b), [b, c)
    };
    static void skip(std::string_view s, std::size_t& at) {
        while (at < s.size()) {
            const char c = s[at];
            if (c == '%') {
                while (at < s.size() && s[at] != '\n' && s[at] != '\r') ++at;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\0') {
                ++at;
            } else {
                break;
            }
        }
    }
    // Parses until the closing brace; operators follow their operands, so an if / ifelse closes the blocks before it.
    bool block(std::string_view s, std::size_t& at, int depth) {
        if (depth > 32) return false;
        std::vector<std::pair<std::size_t, std::size_t>> pending;   // sub-blocks not yet claimed by if / ifelse
        while (true) {
            skip(s, at);
            if (at >= s.size()) return false;
            if (s[at] == '}') {
                ++at;
                return pending.empty();
            }
            if (s[at] == '{') {
                ++at;
                // sub-blocks live in the list behind a jump
                Tok jump;
                jump.kind = Oper;
                jump.op = Op::jump_;
                const std::size_t j = code_.size();
                code_.push_back(jump);
                const std::size_t start = code_.size();
                if (!block(s, at, depth + 1)) return false;
                code_[j].a = code_.size();
                pending.push_back({start, code_.size()});
                continue;
            }
            std::size_t b = at;
            while (at < s.size() && !std::strchr(" \t\r\n\f{}%", s[at]) && s[at]) ++at;
            std::string word(s.substr(b, at - b));
            if (word.empty()) return false;
            if (word == "if") {
                if (pending.size() != 1) return false;
                Tok t;
                t.kind = If;
                t.a = pending[0].first, t.b = pending[0].second;
                code_.push_back(t);
                pending.clear();
            } else if (word == "ifelse") {
                if (pending.size() != 2) return false;
                Tok t;
                t.kind = IfElse;
                t.a = pending[0].first, t.b = pending[0].second, t.c = pending[1].second;
                t.v = double(pending[1].first);
                code_.push_back(t);
                pending.clear();
            } else if ((word[0] >= '0' && word[0] <= '9') || word[0] == '-' || word[0] == '+' || word[0] == '.') {
                char* e = nullptr;
                Tok t;
                t.v = std::strtod(word.c_str(), &e);
                if (!e || *e) return false;
                code_.push_back(t);
            } else {
                static const std::array<const char*, 40> kOpNames = {
                    "abs", "add", "atan", "ceiling", "cos", "cvi", "cvr", "div", "exp", "floor", "idiv", "ln", "log", "mod",
                    "mul", "neg", "round", "sin", "sqrt", "sub", "truncate", "and", "bitshift", "eq", "false", "ge", "gt", "le",
                    "lt", "ne", "not", "or", "true", "xor", "copy", "dup", "exch", "index", "pop", "roll"};
                const auto op = std::find_if(kOpNames.begin(), kOpNames.end(), [&](const char* k) { return word == k; });
                if (op == kOpNames.end()) return false;
                Tok t;
                t.kind = Oper;
                t.op = static_cast<Op>(op - kOpNames.begin());
                code_.push_back(std::move(t));
            }
        }
    }
    bool exec(std::size_t from, std::size_t to, std::vector<double>& st, int& budget) const {
        for (std::size_t i = from; i < to; ++i) {
            if (--budget < 0 || st.size() > 512) return false;
            const Tok& t = code_[i];
            if (t.kind == Num) {
                st.push_back(t.v);
                continue;
            }
            if (t.kind == If || t.kind == IfElse) {
                if (st.empty()) return false;
                const bool cond = st.back() != 0;
                st.pop_back();
                if (t.kind == If) {
                    if (cond && !exec(t.a, t.b, st, budget)) return false;
                } else if (cond) {
                    if (!exec(t.a, t.b, st, budget)) return false;
                } else if (!exec(std::size_t(t.v), t.c, st, budget)) {
                    return false;
                }
                continue;
            }
            if (t.op == Op::jump_) {
                i = t.a - 1;
                continue;
            }
            if (!oper(t.op, st)) return false;
        }
        return true;
    }
    static bool oper(Op o, std::vector<double>& st) {
        auto need = [&](std::size_t n) { return st.size() >= n; };
        auto pop = [&]() {
            double v = st.back();
            st.pop_back();
            return v;
        };
        constexpr double deg = 3.14159265358979323846 / 180;
        if (o == Op::true_) return st.push_back(1), true;
        if (o == Op::false_) return st.push_back(0), true;
        if (o == Op::dup_) return need(1) ? (st.push_back(st.back()), true) : false;
        if (o == Op::pop_) return need(1) ? (st.pop_back(), true) : false;
        if (o == Op::exch_) return need(2) ? (std::swap(st[st.size() - 1], st[st.size() - 2]), true) : false;
        if (o == Op::copy_ || o == Op::index_) {
            if (!need(1)) return false;
            const double n = pop();
            if (n < 0 || n >= double(st.size())) return o == Op::copy_ && n == 0;
            if (o == Op::index_) return st.push_back(st[st.size() - 1 - std::size_t(n)]), true;
            const std::size_t k = std::size_t(n), b = st.size() - k;
            for (std::size_t i = 0; i < k; ++i) st.push_back(st[b + i]);
            return true;
        }
        if (o == Op::roll_) {
            if (!need(2)) return false;
            int j = int(pop());
            const int n = int(pop());
            if (n < 0 || std::size_t(n) > st.size()) return false;
            if (n == 0) return true;
            j %= n;
            if (j < 0) j += n;
            std::rotate(st.end() - n, st.end() - j, st.end());
            return true;
        }
        if (o == Op::abs_ || o == Op::ceiling_ || o == Op::cos_ || o == Op::cvi_ || o == Op::cvr_ || o == Op::floor_ || o == Op::ln_ || o == Op::log_ || o == Op::neg_ ||
            o == Op::round_ || o == Op::sin_ || o == Op::sqrt_ || o == Op::truncate_ || o == Op::not_) {
            if (!need(1)) return false;
            const double a = pop();
            double r = a;
            if (o == Op::abs_) r = std::abs(a);
            else if (o == Op::ceiling_) r = std::ceil(a);
            else if (o == Op::cos_) r = std::cos(a * deg);
            else if (o == Op::cvi_ || o == Op::truncate_) r = std::trunc(a);
            else if (o == Op::floor_) r = std::floor(a);
            else if (o == Op::ln_) r = a > 0 ? std::log(a) : 0;
            else if (o == Op::log_) r = a > 0 ? std::log10(a) : 0;
            else if (o == Op::neg_) r = -a;
            else if (o == Op::round_) r = std::floor(a + 0.5);
            else if (o == Op::sin_) r = std::sin(a * deg);
            else if (o == Op::sqrt_) r = a >= 0 ? std::sqrt(a) : 0;
            else if (o == Op::not_) r = a == 0;
            st.push_back(r);
            return true;
        }
        if (!need(2)) return false;
        const double b = pop(), a = pop();
        double r = 0;
        if (o == Op::add_) r = a + b;
        else if (o == Op::sub_) r = a - b;
        else if (o == Op::mul_) r = a * b;
        else if (o == Op::div_) r = b != 0 ? a / b : 0;
        else if (o == Op::idiv_) r = std::trunc(b) != 0 ? std::trunc(std::trunc(a) / std::trunc(b)) : 0;
        else if (o == Op::mod_) r = std::trunc(b) != 0 ? std::fmod(std::trunc(a), std::trunc(b)) : 0;
        else if (o == Op::exp_) r = std::pow(a, b);
        else if (o == Op::atan_) {
            r = std::atan2(a, b) / deg;
            if (r < 0) r += 360;
        } else if (o == Op::eq_) r = a == b;
        else if (o == Op::ne_) r = a != b;
        else if (o == Op::gt_) r = a > b;
        else if (o == Op::ge_) r = a >= b;
        else if (o == Op::lt_) r = a < b;
        else if (o == Op::le_) r = a <= b;
        else if (o == Op::and_) r = double(std::int64_t(a) & std::int64_t(b));
        else if (o == Op::or_) r = double(std::int64_t(a) | std::int64_t(b));
        else if (o == Op::xor_) r = double(std::int64_t(a) ^ std::int64_t(b));
        else if (o == Op::bitshift_) r = b >= 0 ? double(std::int64_t(a) << std::min(62, int(b))) : double(std::int64_t(a) >> std::min(62, int(-b)));
        else return false;
        st.push_back(r);
        return true;
    }
    std::vector<Tok> code_;
};

// PDF function of type 0, 2, 3 or 4.
class Function {
public:
    static std::shared_ptr<Function> make(const Obj& f, int depth = 0) {
        if (depth > 8 || !(f.isDictionary() || f.isStream())) return nullptr;
        auto out = std::make_shared<Function>();
        Obj d = f;
        auto type = cos::integer(d, "FunctionType");
        if (!type) return nullptr;
        out->type_ = int(*type);
        for (const Obj& v : cos::array_items(cos::get(d, "Domain"))) out->domain_.push_back(cos::number(v).value_or(0));
        for (const Obj& v : cos::array_items(cos::get(d, "Range"))) out->range_.push_back(cos::number(v).value_or(0));
        if (out->domain_.size() < 2) return nullptr;
        if (*type == 2) {
            for (const Obj& v : cos::array_items(cos::get(d, "C0"))) out->c0_.push_back(cos::number(v).value_or(0));
            for (const Obj& v : cos::array_items(cos::get(d, "C1"))) out->c1_.push_back(cos::number(v).value_or(0));
            if (out->c0_.empty()) out->c0_ = {0};
            if (out->c1_.empty()) out->c1_ = {1};
            if (out->c0_.size() != out->c1_.size()) return nullptr;
            out->n_ = cos::number(d, "N").value_or(1);
            return out;
        }
        if (*type == 3) {
            for (const Obj& v : cos::array_items(cos::get(d, "Functions"))) {
                auto sub = make(v, depth + 1);
                if (!sub) return nullptr;
                out->parts_.push_back(std::move(sub));
            }
            for (const Obj& v : cos::array_items(cos::get(d, "Bounds"))) out->bounds_.push_back(cos::number(v).value_or(0));
            for (const Obj& v : cos::array_items(cos::get(d, "Encode"))) out->encode_.push_back(cos::number(v).value_or(0));
            if (out->parts_.empty() || out->bounds_.size() + 1 != out->parts_.size() ||
                out->encode_.size() < 2 * out->parts_.size())
                return nullptr;
            return out;
        }
        if (*type == 0) {
            for (const Obj& v : cos::array_items(cos::get(d, "Size"))) out->size_.push_back(int(cos::integer(v).value_or(0)));
            out->bps_ = int(cos::integer(d, "BitsPerSample").value_or(0));
            for (const Obj& v : cos::array_items(cos::get(d, "Encode"))) out->encode_.push_back(cos::number(v).value_or(0));
            for (const Obj& v : cos::array_items(cos::get(d, "Decode"))) out->decode_.push_back(cos::number(v).value_or(0));
            if (out->decode_.empty()) out->decode_ = out->range_;
            auto data = cos::stream_data(f);
            std::size_t inputs = out->domain_.size() / 2, outputs = out->range_.size() / 2;
            if (!data || out->size_.size() != inputs || outputs == 0 || out->decode_.size() < 2 * outputs ||
                (out->bps_ != 8 && out->bps_ != 16 && out->bps_ != 4 && out->bps_ != 2 && out->bps_ != 1 &&
                 out->bps_ != 12 && out->bps_ != 24 && out->bps_ != 32))
                return nullptr;
            std::size_t cells = 1;
            for (int s : out->size_) {
                if (s < 1 || cells > 1u << 24) return nullptr;
                cells *= std::size_t(s);
            }
            if (cells * outputs * std::size_t(out->bps_) / 8 > data->size()) return nullptr;
            out->samples_ = std::move(*data);
            return out;
        }
        if (*type == 4) {
            auto data = cos::stream_data(f);
            if (!data || out->range_.size() < 2) return nullptr;
            out->calc_ = std::make_shared<Calculator>();
            if (!out->calc_->parse(*data)) return nullptr;
            return out;
        }
        return nullptr;
    }

    // Only one or a few inputs occur (shading parameter, tint components).
    bool eval(const double* in, std::size_t n, std::vector<double>& out) const {
        if (n * 2 != domain_.size() && type_ != 2 && type_ != 3) return false;
        if (type_ == 4) {
            std::vector<double> st;
            for (std::size_t i = 0; i < n; ++i) st.push_back(clamp(in[i], domain_[2 * i], domain_[2 * i + 1]));
            if (!calc_->run(st)) return false;
            const std::size_t outputs = range_.size() / 2;
            if (st.size() < outputs) return false;
            out.resize(outputs);
            for (std::size_t k = 0; k < outputs; ++k)
                out[k] = clamp(st[st.size() - outputs + k], range_[2 * k], range_[2 * k + 1]);
            return true;
        }
        double x = clamp(in[0], domain_[0], domain_[1]);
        if (type_ == 2) {
            double p = n_ == 1 ? x : std::pow(x, n_);
            out.resize(c0_.size());
            for (std::size_t k = 0; k < c0_.size(); ++k) out[k] = c0_[k] + p * (c1_[k] - c0_[k]);
            return true;
        }
        if (type_ == 3) {
            std::size_t k = 0;
            while (k < bounds_.size() && x >= bounds_[k]) ++k;
            if (k >= parts_.size()) k = parts_.size() - 1;
            double lo = k == 0 ? domain_[0] : bounds_[k - 1];
            double hi = k < bounds_.size() ? bounds_[k] : domain_[1];
            double e0 = encode_[2 * k], e1 = encode_[2 * k + 1];
            double t = hi > lo ? e0 + (x - lo) * (e1 - e0) / (hi - lo) : e0;
            return parts_[k]->eval(&t, 1, out);
        }
        // Sampled: nearest sample per input.
        const std::size_t inputs = domain_.size() / 2, outputs = range_.size() / 2;
        std::size_t index = 0, stride = 1;
        for (std::size_t i = 0; i < inputs; ++i) {
            double xi = clamp(in[i], domain_[2 * i], domain_[2 * i + 1]);
            double d0 = domain_[2 * i], d1 = domain_[2 * i + 1];
            double e0 = encode_.size() >= 2 * i + 2 ? encode_[2 * i] : 0;
            double e1 = encode_.size() >= 2 * i + 2 ? encode_[2 * i + 1] : size_[i] - 1;
            double e = d1 > d0 ? e0 + (xi - d0) * (e1 - e0) / (d1 - d0) : e0;
            int s = !(e > 0) ? 0 : e >= size_[i] - 1 ? size_[i] - 1 : int(std::lround(e));
            index += std::size_t(s) * stride;
            stride *= std::size_t(size_[i]);
        }
        out.resize(outputs);
        const double maxv = std::pow(2.0, bps_) - 1;
        for (std::size_t k = 0; k < outputs; ++k) {
            std::uint64_t bit = (index * outputs + k) * std::uint64_t(bps_);
            std::uint64_t v = 0;
            for (int b = 0; b < bps_; ++b, ++bit) {
                std::size_t byte = std::size_t(bit / 8);
                if (byte >= samples_.size()) return false;
                v = (v << 1) | ((static_cast<unsigned char>(samples_[byte]) >> (7 - bit % 8)) & 1u);
            }
            double dv = decode_[2 * k] + double(v) * (decode_[2 * k + 1] - decode_[2 * k]) / maxv;
            out[k] = clamp(dv, range_[2 * k], range_[2 * k + 1]);
        }
        return true;
    }

private:
    static double clamp(double v, double lo, double hi) { return lo <= hi ? std::min(std::max(v, lo), hi) : v; }
    int type_ = -1;
    std::vector<double> domain_, range_, c0_, c1_, bounds_, encode_, decode_;
    double n_ = 1;
    std::vector<std::shared_ptr<Function>> parts_;
    std::vector<int> size_;
    int bps_ = 0;
    std::string samples_;
    std::shared_ptr<Calculator> calc_;
public:
    bool is_calculator() const { return calc_ != nullptr; }
private:
};

// CMYK paint under text: PAC's rendering matches a fit to the SWOP CMYK profile (K-only boundaries, probes b_k_*, b_w_*: black
// text passes on K 0.65 and fails on K 0.70, white text fails on K 0.65 and passes on K 0.70; the device formula of ISO 32000-1
// would put both near K 0.54). The polynomial coefficients are the ones published in pdf.js (src/core/colorspace.js, DeviceCmykCS;
// Copyright 2012 Mozilla Foundation, Apache License 2.0, LICENSES/Apache-2.0.txt).
void pac_cmyk_paint(const double* v, double* rgb) {
    const double c = clamp01(v[0]), m = clamp01(v[1]), y = clamp01(v[2]), k = clamp01(v[3]);
    const double r = 255 + c * (-4.387332384609988 * c + 54.48615194189176 * m + 18.82290502165302 * y + 212.25662451639585 * k - 285.2331026137004) +
                     m * (1.7149763477362134 * m - 5.6096736904047315 * y - 17.873870861415444 * k - 5.497006427196366) +
                     y * (-2.5217340131683033 * y - 21.248923337353073 * k + 17.5119270841813) + k * (-21.86122147463605 * k - 189.48180835922747);
    const double g = 255 + c * (8.841041422036149 * c + 60.118027045597366 * m + 6.871425592049007 * y + 31.159100130055922 * k - 79.2970844816548) +
                     m * (-15.310361306967817 * m + 17.575251261109482 * y + 131.35250912493976 * k - 190.9453302588951) +
                     y * (4.444339102852739 * y + 9.8632861493405 * k - 24.86741582555878) + k * (-20.737325471181034 * k - 187.80453709719578);
    const double b = 255 + c * (0.8842522430003296 * c + 8.078677503112928 * m + 30.89978309703729 * y - 0.23883238689178934 * k - 14.183576799673286) +
                     m * (10.49593273432072 * m + 63.02378494754052 * y + 50.606957656360734 * k - 112.23884253719248) +
                     y * (0.03296041114873217 * y + 115.60384449646641 * k - 193.58209356861505) + k * (-22.33816807309886 * k - 180.12613974708367);
    rgb[0] = clamp01(r / 255), rgb[1] = clamp01(g / 255), rgb[2] = clamp01(b / 255);
    // Full black prints darker than the fit's dark grey: gold text on K 1 passes (a real-world sample).
    if (k >= 0.95 && c == 0 && m == 0 && y == 0) rgb[0] *= 0.7, rgb[1] *= 0.7, rgb[2] *= 0.7;
}

// CMYK text as PAC judges it: darker than the conversion of its rendering. A fit to 30 single-colour PAC runs (probes k_*, kk_*, kf_*):
// black alone: K <= 0.44 fails, K >= 0.45 passes at 12 pt; cyan: K 0.1 fails, 0.2 passes; a real-world sample: C .7 M .49 on
// white passes. It is a fit, not PAC's formula.
void pac_cmyk(const double* c, double* rgb) {
    static const double primary[3][3] = {{0, 0.65, 0.94}, {0.93, 0, 0.55}, {1, 0.95, 0}};
    const double k = std::pow(1 - clamp01(c[3]), 1.31);
    for (int i = 0; i < 3; ++i) {
        double v = k;
        for (int ink = 0; ink < 3; ++ink) v *= 1 - clamp01(c[ink]) * (1 - primary[ink][i]);
        rgb[i] = v;
    }
}

// A colour space turned into "components -> sRGB-like rgb".
struct ColorSpace {
    enum Family { Bad, Gray, RGB, CMYK, Lab, Indexed, Tint } family = Bad;
    int ncomp = 1;
    std::shared_ptr<ColorSpace> base;      // Indexed, Tint
    std::string table;                     // Indexed
    int hival = 0;
    std::shared_ptr<Function> tint;        // Separation, DeviceN
    double lab_range[4] = {-100, 100, -100, 100};
    double white[3] = {0.9505, 1, 1.089};

    static std::shared_ptr<ColorSpace> make(const Obj& cs, const std::vector<Obj>& res, int depth = 0);

    void to_rgb(const double* c, double* rgb, std::vector<double>& scratch) const {
        switch (family) {
            case Gray: rgb[0] = rgb[1] = rgb[2] = clamp01(c[0]); return;
            case RGB:
                for (int k = 0; k < 3; ++k) rgb[k] = clamp01(c[k]);
                return;
            case CMYK:
                pac_cmyk_paint(c, rgb);
                return;
            case Lab: {
                double L = (c[0] + 16) / 116, a = L + c[1] / 500, b = L - c[2] / 200;
                auto g = [](double t) { return t >= 6.0 / 29 ? t * t * t : 108.0 / 841 * (t - 4.0 / 29); };
                double X = white[0] * g(a), Y = white[1] * g(L), Z = white[2] * g(b);
                double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z, gr = -0.9689 * X + 1.8758 * Y + 0.0415 * Z,
                       bl = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
                auto enc = [](double v) {
                    v = clamp01(v);
                    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
                };
                rgb[0] = enc(r), rgb[1] = enc(gr), rgb[2] = enc(bl);
                return;
            }
            case Indexed: {
                int idx = !(c[0] > 0) ? 0 : c[0] >= hival ? hival : int(c[0]);
                int n = base->ncomp;
                double v[32];
                for (int k = 0; k < n && k < 32; ++k) {
                    std::size_t at = std::size_t(idx) * std::size_t(n) + std::size_t(k);
                    v[k] = at < table.size() ? static_cast<unsigned char>(table[at]) / 255.0 : 0;
                    if (base->family == Lab) {
                        v[k] = k == 0 ? v[k] * 100
                                      : base->lab_range[2 * (k - 1)] +
                                            v[k] * (base->lab_range[2 * (k - 1) + 1] - base->lab_range[2 * (k - 1)]);
                    }
                }
                base->to_rgb(v, rgb, scratch);
                return;
            }
            case Tint: {
                std::vector<double> out;
                if (!tint || !tint->eval(c, std::size_t(ncomp), out) || int(out.size()) < base->ncomp) {
                    rgb[0] = rgb[1] = rgb[2] = 1 - clamp01(c[0]);
                    return;
                }
                base->to_rgb(out.data(), rgb, scratch);
                return;
            }
            case Bad: rgb[0] = rgb[1] = rgb[2] = 0; return;
        }
    }
};

Obj res_lookup(const std::vector<Obj>& res, const char* cat, const std::string& name) {
    for (const Obj& r : res) {
        Obj d = cos::get(r, cat);
        if (!d.isDictionary()) continue;
        Obj v = cos::get(d, name);
        if (!v.isNull()) return v;
    }
    return Obj::newNull();
}

std::shared_ptr<ColorSpace> ColorSpace::make(const Obj& cs_in, const std::vector<Obj>& res, int depth) {
    if (depth > 8) return nullptr;
    auto out = std::make_shared<ColorSpace>();
    Obj cs = cs_in;
    if (cs.isName()) {
        std::string n = *cos::name(cs);
        if (n == "DeviceGray" || n == "G" || n == "CalGray") out->family = Gray, out->ncomp = 1;
        else if (n == "DeviceRGB" || n == "RGB" || n == "CalRGB") out->family = RGB, out->ncomp = 3;
        else if (n == "DeviceCMYK" || n == "CMYK") out->family = CMYK, out->ncomp = 4;
        else {
            Obj r = res_lookup(res, "ColorSpace", n);
            if (r.isNull()) return nullptr;
            return make(r, res, depth + 1);
        }
        return out;
    }
    if (!cs.isArray() || cs.getArrayNItems() < 1) return nullptr;
    std::string f = cos::name(cs.getArrayItem(0)).value_or("");
    if (f == "CalGray" || f == "DeviceGray" || f == "G") out->family = Gray, out->ncomp = 1;
    else if (f == "CalRGB" || f == "DeviceRGB" || f == "RGB") out->family = RGB, out->ncomp = 3;
    else if (f == "DeviceCMYK" || f == "CMYK") out->family = CMYK, out->ncomp = 4;
    else if (f == "ICCBased") {
        if (cs.getArrayNItems() < 2) return nullptr;
        auto n = cos::integer(cs.getArrayItem(1), "N").value_or(0);
        if (n == 1) out->family = Gray, out->ncomp = 1;
        else if (n == 3) out->family = RGB, out->ncomp = 3;
        else if (n == 4) out->family = CMYK, out->ncomp = 4;
        else return nullptr;
    } else if (f == "Lab") {
        out->family = Lab, out->ncomp = 3;
        if (cs.getArrayNItems() > 1) {
            auto wp = cos::array_items(cos::get(cs.getArrayItem(1), "WhitePoint"));
            for (std::size_t k = 0; k < wp.size() && k < 3; ++k) out->white[k] = cos::number(wp[k]).value_or(out->white[k]);
            auto rg = cos::array_items(cos::get(cs.getArrayItem(1), "Range"));
            for (std::size_t k = 0; k < rg.size() && k < 4; ++k) out->lab_range[k] = cos::number(rg[k]).value_or(out->lab_range[k]);
        }
    } else if (f == "Indexed" || f == "I") {
        if (cs.getArrayNItems() < 4) return nullptr;
        out->family = Indexed, out->ncomp = 1;
        out->base = make(cs.getArrayItem(1), res, depth + 1);
        if (!out->base) return nullptr;
        out->hival = int(cos::integer(cs.getArrayItem(2)).value_or(0));
        Obj t = cs.getArrayItem(3);
        if (t.isString()) out->table = cos::bytes(t).value_or("");
        else if (t.isStream()) out->table = cos::stream_data(t).value_or("");
    } else if (f == "Separation" || f == "DeviceN") {
        if (cs.getArrayNItems() < 4) return nullptr;
        out->family = Tint;
        out->ncomp = f == "Separation" ? 1 : cs.getArrayItem(1).getArrayNItems();
        if (out->ncomp < 1 || out->ncomp > 32) return nullptr;
        out->base = make(cs.getArrayItem(2), res, depth + 1);
        out->tint = Function::make(cs.getArrayItem(3));
        if (!out->base || !out->tint) return nullptr;
    } else {
        return nullptr;
    }
    return out;
}

// --- image reduction ----------------------------------------------------------------------------------------

// Receives the decoded samples of an image row by row and passes every step-th row on.
class SamplePipe final : public Pipeline {
public:
    using RowFn = std::function<void(const unsigned char* row, int y)>;
    SamplePipe(std::size_t row_bytes, int step, RowFn fn)
        : Pipeline("eqx-sample", nullptr), row_bytes_(row_bytes), step_(step), fn_(std::move(fn)) {
        row_.resize(row_bytes_);
    }
    void write(unsigned char const* data, size_t len) override {
        while (len > 0) {
            if (row_bytes_ == 0) return;
            if (y_ % step_ != 0) {   // skip whole rows quickly
                std::size_t n = std::min(len, row_bytes_ - fill_);
                fill_ += n;
                data += n;
                len -= n;
                if (fill_ == row_bytes_) {
                    fill_ = 0;
                    ++y_;
                }
                continue;
            }
            std::size_t n = std::min(len, row_bytes_ - fill_);
            std::copy(data, data + n, row_.begin() + std::ptrdiff_t(fill_));
            fill_ += n;
            data += n;
            len -= n;
            if (fill_ == row_bytes_) {
                fn_(row_.data(), y_);
                fill_ = 0;
                ++y_;
            }
        }
    }
    void finish() override {}
    int rows() const { return y_; }

private:
    std::vector<unsigned char> row_;
    std::size_t row_bytes_, fill_ = 0;
    int step_, y_ = 0;
    RowFn fn_;
};

// Last filter of the stream's filter chain ("" = none).
bool supported_filters(const Obj& s, std::string* bad) {
    Obj f = cos::get(s, "Filter");
    std::vector<Obj> names;
    if (f.isArray()) names = cos::array_items(f);
    else if (!f.isNull()) names.push_back(f);
    for (const Obj& n : names) {
        std::string v = cos::name(n).value_or("");
        if (((v == "CCITTFaxDecode" || v == "CCF") && names.size() > 1) || v == "JBIG2Decode" || v == "Crypt" || (v == "JPXDecode" && names.size() > 1)) {
            if (bad) *bad = v;
            return false;
        }
    }
    return true;
}

unsigned sample_at(const unsigned char* row, int index, int bpc) {
    if (bpc == 8) return row[index];
    if (bpc == 16) return row[index * 2];   // high byte
    int bit = index * bpc;
    return (row[bit >> 3] >> (8 - bpc - (bit & 7))) & ((1u << bpc) - 1);
}

bool name_is_ccitt(const Obj& image) {
    Obj f = cos::get(image, "Filter");
    if (f.isArray() && f.getArrayNItems() == 1) f = f.getArrayItem(0);
    const std::string v = f.isName() ? cos::name(f).value_or("") : std::string();
    return v == "CCITTFaxDecode" || v == "CCF";
}

// PAC decodes CCITT backgrounds (probes cb_ccitt_*, G4, G3, image mask). qpdf has no such filter: own bounded decoder, rows are piped one at a time, missing rows stay white.
bool pipe_ccitt(const Obj& image, Pipeline& pipe) {
    Obj parms = cos::get(image, "DecodeParms");
    if (parms.isArray() && parms.getArrayNItems() >= 1) parms = parms.getArrayItem(0);
    ccitt::Params p;
    p.k = int(cos::integer(parms, "K").value_or(0));
    p.columns = int(cos::integer(parms, "Columns").value_or(1728));
    p.black_is_1 = cos::boolean(parms, "BlackIs1").value_or(false);
    p.byte_align = cos::boolean(parms, "EncodedByteAlign").value_or(false);
    p.end_of_block = cos::boolean(parms, "EndOfBlock").value_or(true);
    p.rows = int(cos::integer(image, "Height").value_or(0));
    if (cos::integer(image, "Width").value_or(0) != p.columns) return false;
    std::shared_ptr<Buffer> raw = const_cast<Obj&>(image).getRawStreamData();
    const std::size_t row_bytes = (std::size_t(p.columns) + 7) / 8;
    const int got = ccitt::decode(raw->getBuffer(), raw->getSize(), p, [&](const unsigned char* row) { pipe.write(row, row_bytes); });
    if (got == 0) return false;
    if (got < p.rows) {
        std::vector<unsigned char> white(row_bytes, p.black_is_1 ? 0x00 : 0xFF);
        for (int y = got; y < p.rows; ++y) pipe.write(white.data(), row_bytes);
    }
    return true;
}

// Decoded samples into `pipe`; false when a filter cannot be decoded (then qpdf would pass the raw bytes on).
bool pipe_samples(const Obj& image, Pipeline& pipe) {
    try {
        if (name_is_ccitt(image)) return pipe_ccitt(image, pipe);
        bool attempted = false;
        bool ok = const_cast<Obj&>(image).pipeStreamData(&pipe, &attempted, 0, qpdf_dl_all, true, false);
        return ok && attempted;
    } catch (std::exception&) {
        return false;
    }
}

bool name_is_jpx(const Obj& image) {
    Obj f = cos::get(image, "Filter");
    if (f.isArray() && f.getArrayNItems() == 1) f = f.getArrayItem(0);
    return f.isName() && cos::name(f).value_or("") == "JPXDecode";
}

bool reduce_gray(const Obj& image, int max_side, int& w_out, int& h_out, std::vector<unsigned char>& out, bool mask1bit) {
    if (name_is_jpx(image)) {   // a soft mask stored as JPX: its first channel
        try {
            auto raw = const_cast<Obj&>(image).getRawStreamData();
            jpx::Image j;
            if (!jpx::decode_reduced(raw->getBuffer(), raw->getSize(), max_side, j)) return false;
            w_out = j.width, h_out = j.height;
            out.resize(std::size_t(j.width) * std::size_t(j.height));
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = j.samples[i * std::size_t(j.components)];
            return true;
        } catch (std::exception&) {
            return false;
        }
    }
    auto W = cos::integer(image, "Width"), H = cos::integer(image, "Height");
    if (!W || !H || *W < 1 || *H < 1 || *W > 1 << 20 || *H > 1 << 20) return false;
    int bpc = mask1bit || name_is_ccitt(image) ? 1 : int(cos::integer(image, "BitsPerComponent").value_or(8));
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) return false;
    if (!supported_filters(image, nullptr)) return false;
    int step = std::max(1, std::max(int(*W), int(*H)) / max_side + ((std::max(*W, *H) % max_side) ? 1 : 0));
    w_out = int((*W + step - 1) / step);
    h_out = int((*H + step - 1) / step);
    out.assign(std::size_t(w_out) * std::size_t(h_out), 255);
    double d0 = 0, d1 = 1;
    auto dec = cos::array_items(cos::get(image, "Decode"));
    if (dec.size() >= 2) d0 = cos::number(dec[0]).value_or(0), d1 = cos::number(dec[1]).value_or(1);
    const double maxv = bpc == 16 ? 255.0 : double((1u << bpc) - 1);
    const int sub = std::max(1, step / 4);
    // at most ceil(step / sub)^2 <= 49 samples per reduced pixel (sub = step / 4): a sum stays below 49 * 255 (16 bits)
    std::vector<std::uint16_t> sum(out.size(), 0);
    std::vector<std::uint8_t> count(out.size(), 0);
    // the 8-bit value of every possible sample, computed with the same expression as before (a lookup per sample)
    std::array<unsigned char, 256> lut{};
    for (unsigned raw = 0; raw < (bpc == 16 ? 256u : (1u << bpc)); ++raw) lut[raw] = to8(d0 + raw / maxv * (d1 - d0));
    SamplePipe pipe((std::size_t(*W) * std::size_t(bpc) + 7) / 8, sub, [&](const unsigned char* row, int y) {
        int yy = y / step;
        if (yy >= h_out) return;
        // `at` is the reduced pixel of sample x, base + x / step, counted up with x (sub <= step: at most one step on)
        std::size_t at = std::size_t(yy) * std::size_t(w_out);
        int into = 0;   // x % step
        const int width = int(*W);
        if (bpc == 8) {   // sample_at(row, x, 8) == row[x]
            for (int x = 0; x < width; x += sub) {
                sum[at] += lut[row[x]];
                ++count[at];
                if ((into += sub) >= step) into -= step, ++at;
            }
            return;
        }
        for (int x = 0; x < width; x += sub) {
            sum[at] += lut[sample_at(row, x, bpc)];
            ++count[at];
            if ((into += sub) >= step) into -= step, ++at;
        }
    });
    if (!pipe_samples(image, pipe)) return false;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (count[i]) out[i] = static_cast<unsigned char>(rounded_mean(sum[i], count[i]));
    return true;
}

bool reduce_jpx(const Obj& image, int max_side, ReducedImage& out, std::string* why) {
    auto fail = [&](const std::string& m) {
        if (why) *why = m;
        return false;
    };
    std::shared_ptr<Buffer> raw;
    try {
        raw = const_cast<Obj&>(image).getRawStreamData();
    } catch (std::exception&) {
        return fail("image data");
    }
    jpx::Image j;
    std::string jw;
    if (!jpx::decode_reduced(raw->getBuffer(), raw->getSize(), max_side, j, &jw)) return fail("JPXDecode: " + jw);
    // Colour channels: /ColorSpace of the dictionary, else the file's own colour space.
    std::shared_ptr<ColorSpace> cs;
    Obj dcs = cos::get(image, "ColorSpace");
    int colour = j.components;
    if (j.alpha_channel >= 0) --colour;
    if (!dcs.isNull()) {
        cs = ColorSpace::make(dcs, {});
        if (!cs) return fail("colour space");
    } else {
        cs = std::make_shared<ColorSpace>();
        if (colour >= 4 && j.enum_cs != 16 && j.enum_cs != 18) cs->family = ColorSpace::CMYK, cs->ncomp = 4;
        else if (colour >= 3) cs->family = ColorSpace::RGB, cs->ncomp = 3;
        else cs->family = ColorSpace::Gray, cs->ncomp = 1;
    }
    if (cs->ncomp > j.components) return fail("JPX channels");
    const bool sycc = j.enum_cs == 18 && cs->family == ColorSpace::RGB && dcs.isNull();
    const bool use_alpha = cos::integer(image, "SMaskInData").value_or(0) != 0;
    const int alpha = use_alpha ? (j.alpha_channel >= 0 ? j.alpha_channel : (j.components > cs->ncomp ? j.components - 1 : -1)) : -1;
    out.w = j.width;
    out.h = j.height;
    const std::size_t pixels = std::size_t(j.width) * std::size_t(j.height);
    out.rgb.resize(pixels * 3);
    if (alpha >= 0) out.alpha.resize(pixels);
    std::vector<double> scratch;
    double comps[64], rgb[3];
    // the channels in order, skipping the opacity channel
    std::vector<int> order;
    for (int c = 0; c < j.components && int(order.size()) < cs->ncomp; ++c)
        if (c != j.alpha_channel) order.push_back(c);
    if (int(order.size()) < cs->ncomp) return fail("JPX channels");
    for (std::size_t i = 0; i < pixels; ++i) {
        const unsigned char* px = &j.samples[i * std::size_t(j.components)];
        for (int k = 0; k < cs->ncomp && k < 64; ++k) {
            double v = px[order[std::size_t(k)]] / 255.0;
            if (cs->family == ColorSpace::Indexed) v = px[order[std::size_t(k)]];
            else if (cs->family == ColorSpace::Lab) v = k == 0 ? v * 100 : cs->lab_range[2 * (k - 1)] + v * (cs->lab_range[2 * (k - 1) + 1] - cs->lab_range[2 * (k - 1)]);
            comps[k] = v;
        }
        if (sycc) {
            const double y = comps[0], cb = comps[1] - 0.5, cr = comps[2] - 0.5;
            rgb[0] = clamp01(y + 1.402 * cr);
            rgb[1] = clamp01(y - 0.34413 * cb - 0.71414 * cr);
            rgb[2] = clamp01(y + 1.772 * cb);
        } else {
            cs->to_rgb(comps, rgb, scratch);
        }
        for (int k = 0; k < 3; ++k) out.rgb[i * 3 + std::size_t(k)] = to8(rgb[k]);
        if (alpha >= 0) out.alpha[i] = px[alpha];
    }
    return true;
}

bool reduce_samples(const Obj& image, int max_side, ReducedImage& out, std::int64_t W_, std::int64_t H_, std::string* why) {
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    std::optional<std::int64_t> W = W_, H = H_;
    auto cs = ColorSpace::make(cos::get(image, "ColorSpace"), {});
    if (!cs) return fail("colour space");
    int bpc = name_is_ccitt(image) ? 1 : int(cos::integer(image, "BitsPerComponent").value_or(8));
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) return fail("bits per component");
    const int n = cs->ncomp;
    int step = std::max(1, std::max(int(*W), int(*H)) / max_side + ((std::max(*W, *H) % max_side) ? 1 : 0));
    out.w = int((*W + step - 1) / step);
    out.h = int((*H + step - 1) / step);
    out.rgb.assign(std::size_t(out.w) * std::size_t(out.h) * 3, 255);
    // /Decode, defaults by family
    std::vector<double> dec;
    for (const Obj& v : cos::array_items(cos::get(image, "Decode"))) dec.push_back(cos::number(v).value_or(0));
    const bool indexed = cs->family == ColorSpace::Indexed;
    const double maxv = bpc == 16 ? 255.0 : double((1u << bpc) - 1);
    if (dec.size() < std::size_t(2 * n)) {
        dec.clear();
        for (int k = 0; k < n; ++k) {
            if (indexed) dec.push_back(0), dec.push_back(double((1u << std::min(bpc, 8)) - 1));
            else if (cs->family == ColorSpace::Lab) {
                dec.push_back(k == 0 ? 0 : cs->lab_range[2 * (k - 1)]);
                dec.push_back(k == 0 ? 100 : cs->lab_range[2 * (k - 1) + 1]);
            } else dec.push_back(0), dec.push_back(1);
        }
    }
    // Box average over a few samples per reduced pixel: a pattern finer than the reduced image stays its mean colour, as
    // in a rendering (m_fine_lines_gray).
    const int sub = std::max(1, step / 4);
    // at most ceil(step / sub)^2 <= 49 samples per reduced pixel: a channel's sum stays below 49 * 255 (16 bits), 7 instead of
    // 16 bytes per reduced pixel while the image is decoded
    std::vector<std::uint16_t> sum(std::size_t(out.w) * std::size_t(out.h) * 3, 0);
    std::vector<std::uint8_t> count(std::size_t(out.w) * std::size_t(out.h), 0);
    std::vector<double> scratch;
    double comps[32];
    // A per-sample colour conversion (tint transform, CMYK, palette) is what such an image costs (ph_333: 92 %), and images
    // repeat their samples. The conversion is a pure function of the sample values: a table or a memo of its results cannot
    // change a result. One component (Indexed, Separation, single-channel ICC): a table of every possible sample, made by the
    // general path below. Two to four components of at most 8 bits (CMYK, DeviceN, Lab): a direct-mapped memo of fixed size
    // (until 2026-11-10 an unordered map of up to 2^18 entries, Tint only).
    const unsigned levels = bpc == 16 ? 256u : (1u << bpc);   // 16 bit: sample_at takes the high byte
    const bool single = n == 1 && cs->family != ColorSpace::Gray && cs->family != ColorSpace::RGB;
    std::vector<std::array<int, 3>> single_lut;
    constexpr std::size_t kMemoSlots = 4096;
    const bool memo = !single && n >= 2 && n <= 4 && bpc <= 8 && cs->family != ColorSpace::RGB;
    struct MemoSlot {
        std::uint32_t key = 0;
        bool used = false;
        std::array<int, 3> px{};
    };
    std::vector<MemoSlot> memo_slots(memo ? kMemoSlots : 0);
    // Gray and RGB convert each component on its own (to_rgb only clamps it): the 8-bit value of every possible sample is a table
    // computed with the very same expressions, so a lookup per sample gives the same bytes as the general path (most images).
    const bool table = (cs->family == ColorSpace::Gray || cs->family == ColorSpace::RGB) && n <= 3;
    std::array<std::array<unsigned char, 256>, 3> lut{};
    if (table) {
        for (int k = 0; k < n; ++k)
            for (unsigned raw = 0; raw < levels; ++raw) lut[std::size_t(k)][raw] = to8(clamp01(dec[2 * k] + (raw / maxv) * (dec[2 * k + 1] - dec[2 * k])));
    }
    if (single) {
        single_lut.resize(levels);
        for (unsigned raw = 0; raw < levels; ++raw) {
            comps[0] = dec[0] + (raw / maxv) * (dec[1] - dec[0]);
            double rgb[3];
            cs->to_rgb(comps, rgb, scratch);
            single_lut[raw] = {int(to8(rgb[0])), int(to8(rgb[1])), int(to8(rgb[2]))};
        }
    }
    SamplePipe pipe((std::size_t(*W) * std::size_t(n) * std::size_t(bpc) + 7) / 8, sub, [&](const unsigned char* row, int y) {
        int yy = y / step;
        if (yy >= out.h) return;
        // `at` is the reduced pixel of sample x, row start + x / step, counted up with x (sub <= step: at most one step on)
        std::size_t at = std::size_t(yy) * std::size_t(out.w);
        int into = 0;   // x % step
        const int width = int(*W);
        if (single) {
            for (int x = 0; x < width; x += sub) {
                const std::array<int, 3>& px = single_lut[sample_at(row, x, bpc)];
                sum[at * 3] += std::uint32_t(px[0]), sum[at * 3 + 1] += std::uint32_t(px[1]), sum[at * 3 + 2] += std::uint32_t(px[2]);
                ++count[at];
                if ((into += sub) >= step) into -= step, ++at;
            }
            return;
        }
        if (table && bpc == 8) {   // the common case: one byte per sample (sample_at(row, i, 8) == row[i])
            if (n == 1) {
                for (int x = 0; x < width; x += sub) {
                    const unsigned v = lut[0][row[x]];
                    sum[at * 3] += v, sum[at * 3 + 1] += v, sum[at * 3 + 2] += v;
                    ++count[at];
                    if ((into += sub) >= step) into -= step, ++at;
                }
            } else {
                for (int x = 0; x < width; x += sub) {
                    const unsigned char* px = row + std::size_t(x) * 3;
                    sum[at * 3] += lut[0][px[0]], sum[at * 3 + 1] += lut[1][px[1]], sum[at * 3 + 2] += lut[2][px[2]];
                    ++count[at];
                    if ((into += sub) >= step) into -= step, ++at;
                }
            }
            return;
        }
        if (table) {
            for (int x = 0; x < width; x += sub) {
                if (n == 1) {
                    const unsigned v = lut[0][sample_at(row, x, bpc)];
                    sum[at * 3] += v, sum[at * 3 + 1] += v, sum[at * 3 + 2] += v;
                } else {
                    for (int k = 0; k < 3; ++k) sum[at * 3 + std::size_t(k)] += lut[std::size_t(k)][sample_at(row, x * n + k, bpc)];
                }
                ++count[at];
                if ((into += sub) >= step) into -= step, ++at;
            }
            return;
        }
        for (int x = 0; x < width; x += sub) {
            std::uint32_t key = 0;
            for (int k = 0; k < n && k < 32; ++k) {
                const auto raw = sample_at(row, x * n + k, bpc);
                if (memo) key = (key << 8) | std::uint32_t(raw);
                comps[k] = dec[2 * k] + (raw / maxv) * (dec[2 * k + 1] - dec[2 * k]);
            }
            std::array<int, 3> px;
            MemoSlot* slot = memo ? &memo_slots[((key * 2654435761u) >> 20) & (kMemoSlots - 1)] : nullptr;
            if (slot && slot->used && slot->key == key) {
                px = slot->px;
            } else {
                double rgb[3];
                cs->to_rgb(comps, rgb, scratch);
                px = {int(to8(rgb[0])), int(to8(rgb[1])), int(to8(rgb[2]))};
                if (slot) *slot = MemoSlot{key, true, px};
            }
            sum[at * 3] += std::uint32_t(px[0]);
            sum[at * 3 + 1] += std::uint32_t(px[1]);
            sum[at * 3 + 2] += std::uint32_t(px[2]);
            ++count[at];
            if ((into += sub) >= step) into -= step, ++at;
        }
    });
    if (!pipe_samples(image, pipe)) return fail("image data");
    for (std::size_t i = 0; i < count.size(); ++i)
        if (count[i])
            for (std::size_t k = 0; k < 3; ++k) out.rgb[i * 3 + k] = static_cast<unsigned char>(rounded_mean(sum[i * 3 + k], count[i]));
    return true;
}

}  // namespace

double wcag_contrast(const float a[3], const float b[3]) {
    // wcag::srgb_channel: channel() of the component, with the results of recent components kept
    auto lum = [](const float* c) {
        return 0.2126 * wcag::srgb_channel(c[0]) + 0.7152 * wcag::srgb_channel(c[1]) + 0.0722 * wcag::srgb_channel(c[2]);
    };
    // Text runs come in long rows of one colour on one background: the pair asked last is answered from memory (per
    // thread: files are validated in parallel). The function is pure, so the kept ratio is the ratio.
    struct Last {
        float a[3], b[3];
        double ratio = 0;
        bool set = false;
    };
    static thread_local Last last;
    if (last.set && std::memcmp(last.a, a, sizeof last.a) == 0 && std::memcmp(last.b, b, sizeof last.b) == 0) return last.ratio;
    double la = lum(a), lb = lum(b);
    if (la < lb) std::swap(la, lb);
    const double ratio = (la + 0.05) / (lb + 0.05);
    std::memcpy(last.a, a, sizeof last.a);
    std::memcpy(last.b, b, sizeof last.b);
    last.ratio = ratio;
    last.set = true;
    return ratio;
}

bool reduce_image(const Obj& image, int max_side, ReducedImage& out, std::string* why) {
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    if (!image.isStream()) return fail("not a stream");
    std::string bad;
    if (!supported_filters(image, &bad)) {
        if (why) *why = bad;
        return false;
    }
    auto W = cos::integer(image, "Width"), H = cos::integer(image, "Height");
    if (!W || !H || *W < 1 || *H < 1 || *W > 1 << 20 || *H > 1 << 20) return fail("image size");
    if (cos::boolean(image, "ImageMask").value_or(false)) {
        std::vector<unsigned char> g;
        int w = 0, h = 0;
        if (!reduce_gray(image, max_side, w, h, g, true)) return fail("stencil data");
        out.w = w, out.h = h, out.stencil = true;
        // sample 0 (after /Decode) marks the painted part (ISO 32000-1 8.9.6.2): alpha = 255 - decoded value
        out.alpha.resize(g.size());
        for (std::size_t i = 0; i < g.size(); ++i) out.alpha[i] = static_cast<unsigned char>(255 - g[i]);
        return true;
    }
    if (name_is_jpx(image)) {
        if (!reduce_jpx(image, max_side, out, why)) return false;
    } else if (!reduce_samples(image, max_side, out, *W, *H, why)) {
        return false;
    }
    int bpc = int(cos::integer(image, "BitsPerComponent").value_or(8));
    auto cs = ColorSpace::make(cos::get(image, "ColorSpace"), {});
    const int n = cs ? cs->ncomp : 1;
    int step = std::max(1, std::max(int(*W), int(*H)) / max_side + ((std::max(*W, *H) % max_side) ? 1 : 0));
    // Soft mask or stencil mask: alpha at the reduced size.
    Obj sm = cos::get(image, "SMask");
    if (!sm.isStream()) {
        Obj m = cos::get(image, "Mask");
        if (m.isStream()) sm = m;
    }
    if (sm.isStream()) {
        std::vector<unsigned char> g;
        int w = 0, h = 0;
        const bool stencil = cos::boolean(sm, "ImageMask").value_or(false);
        if (!reduce_gray(sm, max_side, w, h, g, stencil)) return fail("mask data");
        // A mask finer than its image carries the shape: Word draws text effects as a 2x2 image under a 217x78 glyph mask
        // (a real-world sample). The image goes to the mask's size.
        if (w > out.w || h > out.h) {
            const int nw = std::max(w, out.w), nh = std::max(h, out.h);
            std::vector<unsigned char> rgb(std::size_t(nw) * std::size_t(nh) * 3);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x) {
                    const std::size_t from = (std::size_t(y * out.h / nh) * std::size_t(out.w) + std::size_t(x * out.w / nw)) * 3;
                    const std::size_t to = (std::size_t(y) * std::size_t(nw) + std::size_t(x)) * 3;
                    rgb[to] = out.rgb[from], rgb[to + 1] = out.rgb[from + 1], rgb[to + 2] = out.rgb[from + 2];
                }
            out.rgb = std::move(rgb);
            out.w = nw, out.h = nh;
        }
        out.alpha.assign(std::size_t(out.w) * std::size_t(out.h), 255);
        std::vector<int> mask_x(std::size_t(out.w));   // the mask column of every image column: the same for each row
        for (int x = 0; x < out.w; ++x) mask_x[std::size_t(x)] = std::min(w - 1, int((x + 0.5) * w / out.w));
        for (int y = 0; y < out.h; ++y) {
            const int my = std::min(h - 1, int((y + 0.5) * h / out.h));
            const unsigned char* mask_row = &g[std::size_t(my) * std::size_t(w)];
            unsigned char* alpha_row = &out.alpha[std::size_t(y) * std::size_t(out.w)];
            for (int x = 0; x < out.w; ++x) {
                unsigned char v = mask_row[mask_x[std::size_t(x)]];
                // an explicit /Mask: sample 1 (after /Decode) is masked out; a /SMask holds the alpha itself
                alpha_row[x] = stencil ? static_cast<unsigned char>(255 - v) : v;
            }
        }
    } else if (auto key = cos::array_items(cos::get(image, "Mask")); key.size() >= std::size_t(2 * n) && !name_is_jpx(image)) {
        // Colour-key masking needs the original samples: re-read sparsely.
        std::vector<unsigned> lo, hi;
        for (int k = 0; k < n; ++k) {
            lo.push_back(unsigned(cos::integer(key[std::size_t(2 * k)]).value_or(0)));
            hi.push_back(unsigned(cos::integer(key[std::size_t(2 * k + 1)]).value_or(0)));
        }
        out.alpha.assign(std::size_t(out.w) * std::size_t(out.h), 255);
        SamplePipe kp((std::size_t(*W) * std::size_t(n) * std::size_t(bpc) + 7) / 8, step, [&](const unsigned char* row, int y) {
            int yy = y / step;
            if (yy >= out.h) return;
            for (int x = 0, xx = 0; x < int(*W); x += step, ++xx) {
                bool in = true;
                for (int k = 0; k < n; ++k) {
                    unsigned v = sample_at(row, x * n + k, bpc);
                    if (v < lo[std::size_t(k)] || v > hi[std::size_t(k)]) in = false;
                }
                if (in) out.alpha[std::size_t(yy) * std::size_t(out.w) + std::size_t(xx)] = 0;
            }
        });
        if (!pipe_samples(image, kp)) out.alpha.clear();
    }
    return true;
}

namespace {

// --- geometry ---------------------------------------------------------------------------------------------

struct Pt {
    float x, y;
};

struct PathRef {
    std::uint32_t first = 0, count = 0;    // subpath records in the page's pool
};

struct Sub {
    std::uint32_t first = 0, count = 0;    // points in the page's pool
    bool closed = false;
};

// Clip chain node: a rectangle in device space, optionally the polygon that made it.
struct ClipNode {
    int parent = -1;
    Rect box;                 // own box intersected with the parents' boxes
    PathRef poly;
    bool has_poly = false;
    bool chain_poly = false;  // this node or a parent has a polygon
    bool evenodd = false;
};

Rect intersect_rects(const Rect& a, const Rect& b) {
    return {std::max(a.llx, b.llx), std::max(a.lly, b.lly), std::min(a.urx, b.urx), std::min(a.ury, b.ury)};
}

struct Paint {
    enum Kind : std::uint8_t { Path, Image, Shading } kind = Path;
    bool fill = false, stroke = false, evenodd = false;
    bool blend_multiply = false;
    bool unknown = false;              // painted, but its colours cannot be determined (tiling pattern, soft mask ...)
    int clip = -1;
    PathRef path;
    Rect box;                          // device box of the paint (clip not applied)
    unsigned char fill_rgb[3] = {0, 0, 0};
    unsigned char stroke_rgb[3] = {0, 0, 0};
    float fill_alpha = 1, stroke_alpha = 1;
    float line_width = 0;              // device units
    Matrix ctm;                        // image or shading matrix
    Obj object;                        // image XObject or shading dictionary
    bool object_pattern = false;       // `object` is painted through the path (shading pattern)
    bool tiling = false;               // `object` is a tiling pattern painted through the path, `ctm` its pattern matrix
};

// PAC's text unit: one string, or the strings of a TJ array between two numbers.
struct TextPiece {
    int stream = -1;
    std::uint32_t op = 0;
    Rect box;
    std::uint32_t first_glyph = 0, glyphs = 0;   // non-blank glyph boxes in the page's pool
    // the state the text was shown in (EqualDoX WCAG checks only)
    int clip = -1;
    float alpha = 1;             // constant group alpha (soft mask of known value)
    bool blend = false;          // soft mask of unknown value or a blend mode
};

struct SavedState {
    int clip;
    bool multiply, soft_mask, blend_other;
    float soft_alpha;
    std::vector<double> fill_comps, stroke_comps;
};

// Per stream instance (depth-first, so a stack).
struct StreamState {
    int si = -1;
    std::vector<Obj> res;
    std::vector<SavedState> saved;
    int clip = -1;
    bool multiply = false, soft_mask = false, blend_other = false;
    float soft_alpha = 1;   // a soft mask of known, constant value (soft_mask = not known)
    std::vector<char> oc_stack;   // per open marked-content sequence: it switches off an optional content group
    int oc_hidden = 0;            // open sequences that do; forms inherit it
    bool do_hidden = false;       // the XObject being invoked has a hidden /OC
    int pending_clip = 0;   // 1 = nonzero, 2 = even-odd
    // current path
    std::vector<Pt> pts;
    std::vector<Sub> subs;
    bool open = false;
    double ux = 0, uy = 0, sx = 0, sy = 0;
    int rects = 0;
    bool other = false;
    Rect box;
    bool box_valid = false;
    // the current fill pattern and the operands of the last sc/scn and SC/SCN
    Obj fill_pattern;
    std::vector<double> fill_comps, stroke_comps;
};

void flatten_curve(std::vector<Pt>& out, const Matrix& m, double x0, double y0, double x1, double y1, double x2, double y2,
                   double x3, double y3) {
    double dx0, dy0, dx3, dy3;
    m.apply(x0, y0, dx0, dy0);
    m.apply(x3, y3, dx3, dy3);
    double dx1, dy1, dx2, dy2;
    m.apply(x1, y1, dx1, dy1);
    m.apply(x2, y2, dx2, dy2);
    double len = std::hypot(dx1 - dx0, dy1 - dy0) + std::hypot(dx2 - dx1, dy2 - dy1) + std::hypot(dx3 - dx2, dy3 - dy2);
    int n = !(len < 128) ? 32 : std::clamp(int(len / 4), 2, 32);
    for (int i = 1; i <= n; ++i) {
        double t = double(i) / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        out.push_back({float(a * dx0 + b * dx1 + c * dx2 + d * dx3), float(a * dy0 + b * dy1 + c * dy2 + d * dy3)});
    }
}

// --- canvas -------------------------------------------------------------------------------------------------

class Canvas {
public:
    Canvas(const Rect& page) : page_(page) {
        double w = std::max(1.0, page.width()), h = std::max(1.0, page.height());
        scale_ = std::min(1.0, double(kMaxCanvasSide) / std::max(w, h));
        w_ = std::max(1, int(std::ceil(w * scale_)));
        h_ = std::max(1, int(std::ceil(h * scale_)));
    }
    int width() const { return w_; }
    int height() const { return h_; }
    double scale() const { return scale_; }
    double px(double x) const { return (x - page_.llx) * scale_; }
    double py(double y) const { return (y - page_.lly) * scale_; }
    double ux(double px) const { return px / scale_ + page_.llx; }
    double uy(double py) const { return py / scale_ + page_.lly; }
    // Only the wanted pixels (the extent [lo, hi) of each row, see want) have storage: all painting is clamped to them
    // by span(), so every other pixel keeps the initial state for good: white, known. Reading one gives exactly that.
    // The storage is laid out by allocate(), after the last want() and before the first pixel is touched.
    void allocate() {
        base_.assign(std::size_t(h_), 0);
        std::size_t n = 0;
        if (!lo_.empty())
            for (int y = std::max(0, want_y0_); y < std::min(h_, want_y1_); ++y) {
                if (lo_[std::size_t(y)] >= hi_[std::size_t(y)]) continue;
                base_[std::size_t(y)] = std::ptrdiff_t(n) - lo_[std::size_t(y)];
                n += std::size_t(hi_[std::size_t(y)] - lo_[std::size_t(y)]);
            }
        rgb_.assign(n * 3, 255);
        unknown_.assign(n, 0);
    }
    // Writable pixel: painting reaches wanted pixels only (anything else would go to a scratch pixel nobody reads).
    unsigned char* at(int x, int y) { return stored(x, y) ? &rgb_[index(x, y) * 3] : scratch_; }
    const unsigned char* at(int x, int y) const { return stored(x, y) ? &rgb_[index(x, y) * 3] : kWhite; }
    unsigned char& unknown(int x, int y) { return stored(x, y) ? unknown_[index(x, y)] : scratch_[3]; }
    unsigned char unknown(int x, int y) const { return stored(x, y) ? unknown_[index(x, y)] : 0; }
    // Pixels that matter: the horizontal extent of the judged text boxes per row. Painting elsewhere is skipped.
    void want(int x0, int y0, int x1, int y1) {
        if (lo_.empty()) {
            lo_.assign(std::size_t(h_), w_);
            hi_.assign(std::size_t(h_), 0);
        }
        for (int y = std::max(0, y0); y < std::min(h_, y1); ++y) {
            lo_[std::size_t(y)] = std::min(lo_[std::size_t(y)], std::max(0, x0));
            hi_[std::size_t(y)] = std::max(hi_[std::size_t(y)], std::min(w_, x1));
            if (lo_[std::size_t(y)] < hi_[std::size_t(y)]) {
                want_y0_ = std::min(want_y0_, y), want_y1_ = std::max(want_y1_, y + 1);
                want_x0_ = std::min(want_x0_, lo_[std::size_t(y)]), want_x1_ = std::max(want_x1_, hi_[std::size_t(y)]);
            }
        }
    }
    // The bounding box of the wanted pixels; empty when none. Nothing outside it is read, so no mask needs to reach further.
    void wanted_box(int& x0, int& y0, int& x1, int& y1) const {
        x0 = want_x0_, y0 = want_y0_, x1 = want_x1_, y1 = want_y1_;
    }
    // Clamps [xa, xb) to the wanted pixels of the row; false when none.
    bool span(int y, int& xa, int& xb) const {
        xa = std::max(xa, lo_[std::size_t(y)]);
        xb = std::min(xb, hi_[std::size_t(y)]);
        return xa < xb;
    }

private:
    bool stored(int x, int y) const {
        return !unknown_.empty() && x >= lo_[std::size_t(y)] && x < hi_[std::size_t(y)];   // not empty: allocate() ran, lo_ is set
    }
    std::size_t index(int x, int y) const { return std::size_t(base_[std::size_t(y)] + x); }
    static constexpr unsigned char kWhite[3] = {255, 255, 255};
    Rect page_;
    double scale_ = 1;
    int w_ = 1, h_ = 1;
    std::vector<unsigned char> rgb_;        // 3 bytes per wanted pixel, row after row
    std::vector<unsigned char> unknown_;    // 1 byte per wanted pixel
    std::vector<std::ptrdiff_t> base_;      // per row: index of pixel x is base_[y] + x (rows with wanted pixels)
    unsigned char scratch_[4] = {255, 255, 255, 0};
    std::vector<int> lo_, hi_;
    int want_x0_ = 1 << 30, want_y0_ = 1 << 30, want_x1_ = 0, want_y1_ = 0;
};

// Pixel window [x0, x1) x [y0, y1).
struct Window {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

// Damaged files can hold huge or undefined coordinates: never cast those to int.
int to_int(double v) {
    if (!(v == v)) return 0;
    return int(std::min(1e8, std::max(-1e8, v)));
}

Window window_of(const Canvas& c, const Rect& r) {
    Window w;
    w.x0 = std::max(0, to_int(std::floor(c.px(r.llx))));
    w.y0 = std::max(0, to_int(std::floor(c.py(r.lly))));
    w.x1 = std::min(c.width(), to_int(std::ceil(c.px(r.urx))));
    w.y1 = std::min(c.height(), to_int(std::ceil(c.py(r.ury))));
    return w;
}

// window_of limited to the pixels that are read: masks and paint stay inside the wanted box.
Window paint_window(const Canvas& c, const Rect& r) {
    Window w = window_of(c, r), want;
    c.wanted_box(want.x0, want.y0, want.x1, want.y1);
    w.x0 = std::max(w.x0, want.x0), w.y0 = std::max(w.y0, want.y0);
    w.x1 = std::min(w.x1, want.x1), w.y1 = std::min(w.y1, want.y1);
    return w;
}

// Scan-line coverage of a polygon set (pixel centres) inside a window.
// One bit per pixel (masks of clips are cached per page: 1 byte per pixel held up to 32 MB on pages full of clipping paths).
class Mask {
public:
    explicit Mask(const Window& w)
        : win_(w), stride_((std::size_t(std::max(0, w.w())) + 7) / 8), bits_(stride_ * std::size_t(std::max(0, w.h())), std::uint8_t(0xFF)) {}
    const Window& window() const { return win_; }
    bool on(int x, int y) const {
        const std::size_t dx = std::size_t(x - win_.x0);
        return (bits_[std::size_t(y - win_.y0) * stride_ + (dx >> 3)] >> (dx & 7)) & 1u;
    }

    // Intersects the mask with the polygon set (each sub-path closed implicitly).
    void intersect(const Canvas& c, const std::vector<Pt>& pool, const std::vector<Sub>& subs, const PathRef& ref, bool evenodd) {
        if (win_.empty()) return;
        std::vector<std::vector<std::pair<float, int>>> rows(static_cast<std::size_t>(win_.h()));
        for (std::uint32_t s = ref.first; s < ref.first + ref.count; ++s) {
            const Sub& sub = subs[s];
            if (sub.count < 2) continue;
            for (std::uint32_t i = 0; i < sub.count; ++i) {
                const Pt& a = pool[sub.first + i];
                const Pt& b = pool[sub.first + (i + 1) % sub.count];
                add_edge(c, a, b, rows);
            }
        }
        apply(rows, evenodd);
    }
    // Intersects with a set of quads (stroke outline), united with the nonzero rule.
    void intersect_quads(const Canvas& c, const std::vector<std::array<Pt, 4>>& quads) {
        if (win_.empty()) return;
        std::vector<std::vector<std::pair<float, int>>> rows(static_cast<std::size_t>(win_.h()));
        for (const auto& q : quads) {
            // consistent winding: a stroke's quads must not cancel each other
            double area = 0;
            for (int i = 0; i < 4; ++i) {
                const Pt& a = q[std::size_t(i)];
                const Pt& b = q[std::size_t((i + 1) % 4)];
                area += double(a.x) * b.y - double(b.x) * a.y;
            }
            for (int i = 0; i < 4; ++i) {
                const Pt& a = q[std::size_t(area >= 0 ? i : 3 - i)];
                const Pt& b = q[std::size_t(area >= 0 ? (i + 1) % 4 : (6 - i) % 4)];
                add_edge(c, a, b, rows);
            }
        }
        apply(rows, false);
    }
    // Intersects with another mask; pixels outside its window are off.
    void intersect_mask(const Mask& o) {
        if (win_.empty()) return;
        const int xa = std::max(win_.x0, o.win_.x0), xb = std::min(win_.x1, o.win_.x1);
        for (int y = win_.y0; y < win_.y1; ++y) {
            std::uint8_t* out = &bits_[std::size_t(y - win_.y0) * stride_];
            if (y < o.win_.y0 || y >= o.win_.y1 || xa >= xb) {
                std::fill_n(out, stride_, 0);
                continue;
            }
            for (int x = win_.x0; x < win_.x1; ++x)
                if (x < xa || x >= xb || !o.on(x, y)) clear(out, std::size_t(x - win_.x0));
        }
    }

private:
    static void clear(std::uint8_t* row, std::size_t dx) { row[dx >> 3] &= std::uint8_t(~(1u << (dx & 7))); }
    // Sets the bits [a, b) of a row: partial bytes bit by bit, whole bytes at once.
    static void set_span(std::uint8_t* row, std::size_t a, std::size_t b) {
        while (a < b && (a & 7)) row[a >> 3] |= std::uint8_t(1u << (a & 7)), ++a;
        if (b - a >= 8) {
            std::fill(row + (a >> 3), row + (b >> 3), std::uint8_t(0xFF));
            a = b & ~std::size_t(7);
        }
        while (a < b) row[a >> 3] |= std::uint8_t(1u << (a & 7)), ++a;
    }
    void add_edge(const Canvas& c, const Pt& a, const Pt& b, std::vector<std::vector<std::pair<float, int>>>& rows) {
        double ax = c.px(a.x), ay = c.py(a.y), bx = c.px(b.x), by = c.py(b.y);
        if (ay == by || !(ay == ay) || !(by == by)) return;
        int dir = ay < by ? 1 : -1;
        if (dir < 0) std::swap(ax, bx), std::swap(ay, by);
        int y0 = std::max(win_.y0, to_int(std::ceil(ay - 0.5))), y1 = std::min(win_.y1, to_int(std::ceil(by - 0.5)));
        for (int y = y0; y < y1; ++y) {
            double t = (y + 0.5 - ay) / (by - ay);
            rows[std::size_t(y - win_.y0)].push_back({float(ax + t * (bx - ax)), dir});
        }
    }
    void apply(std::vector<std::vector<std::pair<float, int>>>& rows, bool evenodd) {
        std::vector<std::uint8_t> row(stride_);
        for (int y = win_.y0; y < win_.y1; ++y) {
            auto& v = rows[std::size_t(y - win_.y0)];
            std::fill(row.begin(), row.end(), 0);
            std::sort(v.begin(), v.end());
            int wind = 0;
            for (std::size_t i = 0; i + 1 < v.size(); ++i) {
                wind += v[i].second;
                bool in = evenodd ? ((i + 1) % 2 == 1) : wind != 0;
                if (!in) continue;
                int xa = std::max(win_.x0, to_int(std::ceil(double(v[i].first) - 0.5)));
                int xb = std::min(win_.x1, to_int(std::ceil(double(v[i + 1].first) - 0.5)));
                if (xa < xb) set_span(row.data(), std::size_t(xa - win_.x0), std::size_t(xb - win_.x0));
            }
            std::uint8_t* out = &bits_[std::size_t(y - win_.y0) * stride_];
            for (std::size_t k = 0; k < stride_; ++k) out[k] &= row[k];
        }
    }
    Window win_;
    std::size_t stride_;                 // bytes per row (8 pixels each)
    std::vector<std::uint8_t> bits_;    // bit x of row y: pixel (win_.x0 + x, win_.y0 + y)
};

// The colour PAC judges: the stroke colour whenever the mode strokes (r_tr2_*, r_tr5_grey, r_tr6_gf, tr2_greyf), else the fill.
bool uses_stroke(int tr) { return tr == 1 || tr == 2 || tr == 5 || tr == 6; }

bool bold_font(const Font* f) {
    if (!f) return false;
    std::string n = f->base_font;
    for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (n.find("bold") != std::string::npos || n.find("black") != std::string::npos || n.find("heavy") != std::string::npos)
        return true;
    Obj fd = cos::get(f->dict, "FontDescriptor");
    if (!fd.isDictionary()) {
        Obj desc = cos::get(f->dict, "DescendantFonts");
        if (desc.isArray() && desc.getArrayNItems() > 0) fd = cos::get(desc.getArrayItem(0), "FontDescriptor");
    }
    if (auto w = cos::number(fd, "FontWeight"); w && *w >= 700) return true;
    if (auto fl = cos::integer(fd, "Flags"); fl && (*fl & (1 << 18))) return true;
    return false;
}

std::string hex8(const unsigned char* c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02x%02x%02x", c[0], c[1], c[2]);
    return b;
}

}  // namespace

// --- the observer -------------------------------------------------------------------------------------------

// One cell of a tiling pattern as a small raster: enough for the colour under a glyph box, bounded memory. Fills of rectangles and polygons, images and colours; anything else leaves the
// cell undecided (never a guess).
struct TileRaster {
    int w = 0, h = 0;
    double x0 = 0, y0 = 0, xstep = 1, ystep = 1;
    std::vector<unsigned char> rgb, alpha;
};

bool build_tile(const Obj& pat, const unsigned char* uncoloured, TileRaster& t) {
    auto steps = std::make_pair(cos::number(cos::get(pat, "XStep")).value_or(0), cos::number(cos::get(pat, "YStep")).value_or(0));
    auto bbox = cos::rect(cos::get(pat, "BBox"));
    if (!bbox || steps.first == 0 || steps.second == 0) return false;
    t.xstep = std::abs(steps.first), t.ystep = std::abs(steps.second);
    t.x0 = bbox->llx, t.y0 = bbox->lly;
    t.w = std::clamp(int(std::ceil(t.xstep * 2)), 4, 256);
    t.h = std::clamp(int(std::ceil(t.ystep * 2)), 4, 256);
    t.rgb.assign(std::size_t(t.w) * std::size_t(t.h) * 3, 255);
    t.alpha.assign(std::size_t(t.w) * std::size_t(t.h), 0);
    std::string data;
    try {
        auto buf = const_cast<Obj&>(pat).getStreamData(qpdf_dl_generalized);
        if (buf->getSize() > 4 << 20) return false;
        data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (std::exception&) {
        return false;
    }
    Obj res = cos::get(pat, "Resources");
    struct G {
        Matrix m;
        unsigned char c[3];
    };
    G g{};
    if (uncoloured) std::copy(uncoloured, uncoloured + 3, g.c);
    std::vector<G> stack;
    struct P { double x, y; };
    std::vector<std::vector<P>> subs;
    auto fill = [&](bool evenodd) {
        double lx = 1e30, ly = 1e30, ux = -1e30, uy = -1e30;
        for (auto& sp : subs)
            for (const P& q : sp) lx = std::min(lx, q.x), ly = std::min(ly, q.y), ux = std::max(ux, q.x), uy = std::max(uy, q.y);
        if (lx > ux) return;
        const double sx = t.w / t.xstep, sy = t.h / t.ystep;
        for (int y = std::max(0, int((ly - t.y0) * sy)); y < t.h && t.y0 + y / sy <= uy; ++y)
            for (int x = std::max(0, int((lx - t.x0) * sx)); x < t.w && t.x0 + x / sx <= ux; ++x) {
                const double px = t.x0 + (x + 0.5) / sx, py = t.y0 + (y + 0.5) / sy;
                int wind = 0;
                for (auto& sp : subs)
                    for (std::size_t i = 0; i < sp.size(); ++i) {
                        const P& a = sp[i];
                        const P& b = sp[(i + 1) % sp.size()];
                        if ((a.y <= py) != (b.y <= py)) {
                            const double cross = a.x + (py - a.y) / (b.y - a.y) * (b.x - a.x);
                            if (cross > px) wind += b.y > a.y ? 1 : -1;
                        }
                    }
                if (evenodd ? (wind & 1) == 0 : wind == 0) continue;
                const std::size_t at = std::size_t(y) * std::size_t(t.w) + std::size_t(x);
                std::copy(g.c, g.c + 3, &t.rgb[at * 3]);
                t.alpha[at] = 255;
            }
    };
    ContentParser parser(data);
    RawOp op;
    int guard = 0;
    while (parser.next(op)) {
        if (++guard > 20000) return false;
        const std::vector<Operand>& a = *op.operands;
        const auto num = [&](std::size_t i) { return i < a.size() && a[i].is_number() ? a[i].num() : 0.0; };
        const auto to = [&](double x, double y) {
            double ox, oy;
            g.m.apply(x, y, ox, oy);
            return P{ox, oy};
        };
        const auto set = [&](double r, double gr, double b) {
            if (!uncoloured) g.c[0] = to8(r), g.c[1] = to8(gr), g.c[2] = to8(b);
        };
        switch (op.code) {
            case Op::q: stack.push_back(g); break;
            case Op::Q:
                if (!stack.empty()) g = stack.back(), stack.pop_back();
                break;
            case Op::cm: g.m = Matrix{num(0), num(1), num(2), num(3), num(4), num(5)} * g.m; break;
            case Op::g: set(num(0), num(0), num(0)); break;
            case Op::rg: set(num(0), num(1), num(2)); break;
            case Op::k: {
                double in[4] = {num(0), num(1), num(2), num(3)}, rgb[3];
                pac_cmyk_paint(in, rgb);
                set(rgb[0], rgb[1], rgb[2]);
                break;
            }
            case Op::sc:
            case Op::scn:
                if (a.size() == 1) set(num(0), num(0), num(0));
                else if (a.size() == 3) set(num(0), num(1), num(2));
                else if (a.size() == 4) {
                    double in[4] = {num(0), num(1), num(2), num(3)}, rgb[3];
                    pac_cmyk_paint(in, rgb);
                    set(rgb[0], rgb[1], rgb[2]);
                } else if (!uncoloured) return false;
                break;
            case Op::re:
                subs.push_back({to(num(0), num(1)), to(num(0) + num(2), num(1)), to(num(0) + num(2), num(1) + num(3)), to(num(0), num(1) + num(3))});
                break;
            case Op::m: subs.push_back({to(num(0), num(1))}); break;
            case Op::l:
                if (!subs.empty()) subs.back().push_back(to(num(0), num(1)));
                break;
            case Op::c:
                if (!subs.empty()) subs.back().push_back(to(num(4), num(5)));
                break;
            case Op::v:
            case Op::y:
                if (!subs.empty()) subs.back().push_back(to(num(2), num(3)));
                break;
            case Op::h:
            case Op::cs:
            case Op::CS:
            case Op::G:
            case Op::RG:
            case Op::K:
            case Op::SC:
            case Op::SCN:
            case Op::w:
            case Op::J:
            case Op::j:
            case Op::M:
            case Op::d:
            case Op::i:
            case Op::ri:
            case Op::gs:
            case Op::BMC:
            case Op::BDC:
            case Op::EMC:
            case Op::d0:
            case Op::d1:
                break;
            case Op::n: subs.clear(); break;
            case Op::f:
            case Op::F:
                fill(false);
                subs.clear();
                break;
            case Op::f_star:
                fill(true);
                subs.clear();
                break;
            case Op::Do: {
                if (a.empty() || a.back().type != Operand::Name) return false;
                Obj xo = cos::get(cos::get(res, "XObject"), a.back().s);
                if (!xo.isStream() || cos::name(xo, "Subtype").value_or("") != "Image") return false;
                ReducedImage img;
                if (!reduce_image(xo, 128, img) || img.w < 1 || img.h < 1) return false;
                const double det = g.m.a * g.m.d - g.m.b * g.m.c;
                if (det == 0) break;
                const double sx = t.w / t.xstep, sy = t.h / t.ystep;
                for (int y = 0; y < t.h; ++y)
                    for (int x = 0; x < t.w; ++x) {
                        const double ux = t.x0 + (x + 0.5) / sx - g.m.e, uy = t.y0 + (y + 0.5) / sy - g.m.f;
                        const double u = (g.m.d * ux - g.m.c * uy) / det, v = (-g.m.b * ux + g.m.a * uy) / det;
                        if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
                        const std::size_t si = std::size_t(int((1 - v) * img.h)) * std::size_t(img.w) + std::size_t(int(u * img.w));
                        const std::size_t at = std::size_t(y) * std::size_t(t.w) + std::size_t(x);
                        if (!img.alpha.empty() && si < img.alpha.size() && img.alpha[si] < 128) continue;
                        if (si * 3 + 2 < img.rgb.size()) std::copy(&img.rgb[si * 3], &img.rgb[si * 3] + 3, &t.rgb[at * 3]);
                        else if (img.stencil) std::copy(g.c, g.c + 3, &t.rgb[at * 3]);
                        t.alpha[at] = 255;
                    }
                break;
            }
            default:
                return false;   // strokes, text, shadings in a cell: undecided
        }
    }
    return true;
}

// --- inline images ------------------------------------------------------------------------------------------

// An inline image's dictionary, abbreviations expanded (ISO 32000-1 Tables 93, 94). Hosted in a private QPDF, so the document's objects stay untouched.
Obj operand_to_obj(const Operand& o, bool colour_value, bool filter_value, const std::vector<Obj>& res) {
    switch (o.type) {
        case Operand::Bool: return Obj::newBool(o.b);
        case Operand::Int: return Obj::newInteger(o.i);
        case Operand::Real: return Obj::newReal(o.r, 6);
        case Operand::String: return Obj::newString(o.s);
        case Operand::Name: {
            std::string v = o.s;
            if (colour_value) {
                if (v == "G") v = "DeviceGray";
                else if (v == "RGB") v = "DeviceRGB";
                else if (v == "CMYK") v = "DeviceCMYK";
                else if (v == "I") v = "Indexed";
                else if (v != "DeviceGray" && v != "DeviceRGB" && v != "DeviceCMYK" && v != "Indexed" && v != "Pattern") {
                    // a name from the ColorSpace resources
                    for (const Obj& r : res) {
                        Obj cs = cos::get(cos::get(r, "ColorSpace"), v);
                        if (!cs.isNull()) return cs.shallowCopy();
                    }
                }
            }
            if (filter_value) {
                if (v == "AHx") v = "ASCIIHexDecode";
                else if (v == "A85") v = "ASCII85Decode";
                else if (v == "LZW") v = "LZWDecode";
                else if (v == "Fl") v = "FlateDecode";
                else if (v == "RL") v = "RunLengthDecode";
                else if (v == "CCF") v = "CCITTFaxDecode";
                else if (v == "DCT") v = "DCTDecode";
            }
            return Obj::newName("/" + v);
        }
        case Operand::Array: {
            Obj a = Obj::newArray();
            for (const Operand& x : o.items) a.appendItem(operand_to_obj(x, colour_value, filter_value, res));
            return a;
        }
        case Operand::Dict: {
            Obj d = Obj::newDictionary();
            for (std::size_t k = 0; k + 1 < o.items.size(); k += 2) d.replaceKey("/" + o.items[k].s, operand_to_obj(o.items[k + 1], false, false, res));
            return d;
        }
        default: return Obj::newNull();
    }
}

// The value of a soft mask that is one constant over its group (a rectangle in a grey level, an image or shading taken at its mean); -1 when it cannot be told. PAC renders the
// mask (probes cb_gs_smask_*: full, half, zero); a per-pixel mask stays open, its area is unknown.
double soft_mask_value(const Obj& sm) {
    if (!sm.isDictionary() || !cos::get(sm, "TR").isNull()) {
        Obj tr = cos::get(sm, "TR");
        if (!(tr.isName() && cos::name(tr).value_or("") == "Identity") && !tr.isNull()) return -1;
    }
    const bool luminosity = cos::name_is(sm, "S", "Luminosity");
    Obj g = cos::get(sm, "G");
    if (!g.isStream()) return -1;
    auto bbox = cos::rect(cos::get(g, "BBox"));
    if (!bbox) return -1;
    std::string data;
    try {
        auto buf = const_cast<Obj&>(g).getStreamData(qpdf_dl_generalized);
        if (buf->getSize() > 1 << 20) return -1;
        data.assign(reinterpret_cast<const char*>(buf->getBuffer()), buf->getSize());
    } catch (std::exception&) {
        return -1;
    }
    double level = 0;        // current fill as luminosity
    double out = -1;         // value of the group so far: none painted yet
    double path_area = 0;
    int painted = 0;
    ContentParser parser(data);
    RawOp op;
    std::vector<Operand> stack;
    int guard = 0;
    Obj res = cos::get(g, "Resources");
    while (parser.next(op)) {
        if (++guard > 10000) return -1;
        const std::vector<Operand>& a = *op.operands;
        const auto num = [&](std::size_t i) { return i < a.size() && a[i].is_number() ? a[i].num() : 0.0; };
        switch (op.code) {
            case Op::g: level = num(0); break;
            case Op::rg: level = 0.3 * num(0) + 0.59 * num(1) + 0.11 * num(2); break;
            case Op::k: level = (1 - num(3)) * (1 - (0.3 * num(0) + 0.59 * num(1) + 0.11 * num(2))); break;
            case Op::sc:
            case Op::scn:
                if (a.size() == 1) level = num(0);
                else if (a.size() == 3) level = 0.3 * num(0) + 0.59 * num(1) + 0.11 * num(2);
                else return -1;
                break;
            case Op::re: path_area += std::abs(num(2) * num(3)); break;
            case Op::f:
            case Op::F:
            case Op::f_star:
                if (path_area < 0.9 * bbox->width() * bbox->height()) return -1;
                out = luminosity ? level : 1.0;
                path_area = 0;
                ++painted;
                break;
            case Op::n: path_area = 0; break;
            case Op::Do: {
                if (a.empty() || a.back().type != Operand::Name) return -1;
                Obj xo = cos::get(cos::get(res, "XObject"), a.back().s);
                if (!xo.isStream() || cos::name(xo, "Subtype").value_or("") != "Image") return -1;
                ReducedImage img;
                if (!reduce_image(xo, 32, img) || img.rgb.empty()) return -1;
                double sum = 0;
                const std::size_t n = img.rgb.size() / 3;
                for (std::size_t i = 0; i < n; ++i) sum += (0.3 * img.rgb[i * 3] + 0.59 * img.rgb[i * 3 + 1] + 0.11 * img.rgb[i * 3 + 2]) / 255.0;
                out = luminosity ? sum / double(std::max<std::size_t>(1, n)) : 1.0;
                ++painted;
                break;
            }
            case Op::q:
            case Op::Q:
            case Op::cm:
            case Op::gs:
            case Op::W:
            case Op::W_star:
            case Op::cs:
            case Op::CS:
            case Op::BMC:
            case Op::BDC:
            case Op::EMC:
                break;
            default:
                return -1;   // paths, shadings, text: not one constant value here
        }
        if (painted > 1) return -1;
    }
    if (out < 0) {
        // nothing painted: the backdrop colour of the group (black by default)
        auto bc = cos::array_items(cos::get(sm, "BC"));
        out = 0;
        if (luminosity && bc.size() == 1) out = cos::number(bc[0]).value_or(0);
    }
    return clamp01(out);
}

// Bytes a cached reduced image holds (0 for an image that could not be decoded).
static std::size_t image_size(const ReducedImage* img) { return img ? img->rgb.size() + img->alpha.size() : 0; }

class ContrastEngine final : public ContentObserver {
public:
    explicit ContrastEngine(ValidationContext& ctx) : ctx_(ctx) {}

    void on_operator(const ContentModel& cm, int si, std::uint32_t op_index, const RawOp& op, const GState& gs) override;
    void on_stream_end(const ContentModel& cm, int si) override;
    void on_glyph(const ContentModel& cm, const GlyphEvent& ev) override;

    std::vector<ContrastMeasure> findings;
    std::vector<ArtifactTextPiece> artifact_pieces;
    std::vector<ContrastFinding> missing_res;   // PAC closes such a document
    bool gave_up = false;   // a page exceeded the geometry or pixel budget and was not judged

private:
    struct CodeText {
        static constexpr std::uint8_t kKnown = 1;       // the entry was read
        static constexpr std::uint8_t kNoUnicode = 2;   // the code has no Unicode value
        static constexpr std::uint8_t kInk = 4;         // a character that is neither white space nor U+0000
        static constexpr std::uint8_t kLetter = 8;      // such a character that is a letter
        static constexpr std::uint8_t kOneChar = 16;    // exactly one such character: `utf8` holds it
        std::uint8_t flags = 0;
        std::uint8_t len = 0;
        char utf8[4] = {0, 0, 0, 0};
    };
    static CodeText read_code_text(const Font* font, std::uint32_t code);
    CodeText code_text(const Font* font, std::uint32_t code);
    CodeText code_text_slow(const Font* font, std::uint32_t code);
    std::unordered_map<const Font*, std::vector<std::unique_ptr<CodeText[]>>> code_texts_;
    const Font* code_text_font_ = nullptr;                          // the font used last and its pages
    const std::unique_ptr<CodeText[]>* code_text_pages_ = nullptr;
    std::size_t code_text_npages_ = 0;

    StreamState* state_for(const ContentModel& cm, int si);
    void begin_text_op(int si, std::uint32_t op_index, const RawOp& op, const GState& gs);
    void close_piece(const ContentModel& cm);
    bool artifact_text_frame(const ContentModel& cm, int frame) const;
    int text_frame_ = -2;                 // the frame text_frame_artifact_ was computed for (-2: none yet)
    bool text_frame_artifact_ = false;
    void end_text_op(const ContentModel& cm);
    void begin_page(const ContentModel& cm, int si);
    void end_path(StreamState& st);
    void add_clip(StreamState& st);
    void record_paint(const ContentModel& cm, StreamState& st, int si, std::uint32_t op_index, const GState& gs, bool fill, bool stroke,
                      bool evenodd);
    void flush_page(const ContentModel& cm);
    void paint_item(const Paint& p, Canvas& canvas);
    void paint_path(const Paint& p, Canvas& canvas);
    void paint_image(const Paint& p, Canvas& canvas);
    void paint_shading(const Paint& p, Canvas& canvas);
    void paint_mesh(const Paint& p, Canvas& canvas, const Window& w, const Mask& mask, const ColorSpace& cs, std::int64_t type);
    void paint_function_shading(const Paint& p, Canvas& canvas, const Window& w, const Mask& mask, const ColorSpace& cs);
    void paint_tile(const Paint& p, Canvas& canvas, const Window& w, const Mask& m);
    std::shared_ptr<TileRaster> tile_of(const Paint& p);
    std::unordered_map<std::string, std::shared_ptr<TileRaster>> tiles_;
    const Mask* clip_mask(const Canvas& canvas, int clip);
    // Paint in a colour the interpreter does not know (Separation, DeviceN, Lab: t4_bg_black).
    bool tint_paint(const Obj& cso, const std::vector<double>& comps, const StreamState& st, double* rgb) const;
    Rect clipped(const Rect& box, int clip) const {
        return clip >= 0 ? intersect_rects(box, clips_[std::size_t(clip)].box) : box;
    }
    bool spend(const Window& w) {
        const std::int64_t area = std::int64_t(std::max(0, w.w())) * std::max(0, w.h());
        budget_ -= area;
        return budget_ >= 0;
    }
    void check_text(const ContentModel& cm, const ContentItem& it, const Canvas& canvas);
    bool background(const Canvas& canvas, const Window& w, unsigned char* out) const;
    std::pair<std::size_t, std::size_t> piece_range(int stream, std::uint32_t op) const;
    mutable std::size_t piece_hint_ = 0;   // where the last piece_range ended (a guess for the next, checked there)
    const ReducedImage* image_of(const Obj& xo);
    Obj inline_image_stream(const RawOp& op, const StreamState& st);
    bool oc_is_off(const Obj& oc);
    std::unordered_set<std::uint64_t> hidden_ops_;      // operators inside switched-off optional content, this page
    // Whether the item's operator is one of them (mostly there are none on the page: asked before the set is searched).
    bool hidden_op(const ContentItem& it) const {
        return !hidden_ops_.empty() && hidden_ops_.count((std::uint64_t(std::uint32_t(it.stream)) << 32) | it.op_index) != 0;
    }
    bool oc_read_ = false;
    bool oc_base_off_ = false;
    std::unordered_set<std::uint64_t> oc_off_, oc_on_;
    std::unique_ptr<QPDF> inline_pdf_;     // hosts decoded inline images
    std::size_t inline_bytes_ = 0;

    ValidationContext& ctx_;
    std::vector<StreamState> stack_;
    // page state
    int page_ = -1;
    bool overflow_ = false;
    std::size_t first_item_ = 0;
    std::vector<Pt> pool_;
    std::vector<Sub> subs_;
    std::vector<ClipNode> clips_;
    std::vector<Paint> paints_;
    std::unordered_map<std::uint64_t, int> paint_of_;
    std::unordered_map<std::uint64_t, std::array<unsigned char, 3>> text_rgb_;   // runs in Separation, DeviceN, Lab
    std::unordered_map<std::uint64_t, std::shared_ptr<ReducedImage>> images_;
    std::vector<std::uint64_t> image_order_;   // least recently used first
    std::size_t image_bytes_ = 0;               // pixel bytes held in images_
    std::size_t page_mask_pixels_ = 0;          // pixels of the clip masks this page made (freed with the page)
    std::shared_ptr<Mask> shading_mask_;   // set while a shading pattern paints through a path
    // the text operator being shown, cut into PAC's text pieces
    int text_stream_ = -1;
    std::uint32_t text_op_ = 0;
    std::vector<std::uint32_t> piece_ends_;
    std::size_t piece_at_ = 0;
    int glyph_frame_ = -1;
    Rect piece_box_;
    bool piece_valid_ = false, piece_letter_ = false, piece_ink_ = false;
    std::string piece_text_;
    std::vector<CharCode> code_scratch_;
    std::vector<TextPiece> pieces_;        // of the current page, for the contrast judgement
    std::vector<float> glyph_boxes_;       // x0, y0, x1, y1 per non-blank glyph of the page
    static constexpr std::size_t kMaxSizeHint = std::size_t(1) << 20;   // 4 MB of boxes; a sixteenth as many pieces
    std::size_t boxes_hint_ = 0, pieces_hint_ = 0;   // sizes of the two on the page before (capped), reserved for the next
    double tm_a_ = 1, tm_b_ = 0;           // first row of the text matrix
    double text_dir_x_ = 1, text_dir_y_ = 0;   // unit vector of the writing direction in user space
    std::uint32_t piece_first_glyph_ = 0;
    std::unordered_map<int, std::shared_ptr<Mask>> clip_masks_;   // polygon clips rendered for this page
    std::size_t clip_mask_bytes_ = 0;
    std::int64_t budget_ = kPixelBudget;   // pixels touched while rendering, for the document

    // EqualDoX's WCAG colour checks (wcag_color.h) on the same measurements; they never change PAC's judgement.
    void wcag_text(const ContentModel& cm, const ContentItem& it, const TextPiece& piece, const Rect& box, const float* fg, const float* bg,
                   bool rendered, bool exact_bg, bool artifact);
    std::optional<ObjRef> figure_of(const ContentModel& cm, const ContentItem& it);
    void wcag_incomplete(bool page_skipped);
    template <class CellRange, class PaintedBox>
    void wcag_figures(const ContentModel& cm, const std::vector<int>& parts, const std::vector<std::vector<int>>& cells, int n,
                      const CellRange& cell_range, const PaintedBox& painted_box);
    std::unordered_map<const Font*, int> bold_states_;   // wcag::bold_state of a font shown in a mode other than 2 and 6
    std::vector<int> frame_fig_;           // per frame: -2 not looked up, -1 not inside a Figure, else its index in frame_fig_ref_
    std::vector<ObjRef> frame_fig_ref_;    // one entry per frame inside a Figure
    bool wcag_inc_all_ = false, wcag_inc_review_ = false;
    int text_clip_ = -1;
    float text_alpha_ = 1;
    bool text_blend_ = false;
};

StreamState* ContrastEngine::state_for(const ContentModel& cm, int si) {
    if (!stack_.empty() && stack_.back().si == si) return &stack_.back();
    const StreamInstance& s = cm.streams[si];
    if (!s.semantic || s.page < 0) return nullptr;
    StreamState st;
    st.si = si;
    if (s.kind == StreamInstance::Kind::Page) {
        begin_page(cm, si);
        const auto& pages = ctx_.doc.pages();
        if (s.page < int(pages.size()) && pages[s.page].resources.isDictionary()) st.res.push_back(pages[s.page].resources);
    } else {
        Obj fr = cos::get(s.stream, "Resources");
        if (fr.isDictionary()) st.res.push_back(fr);
        if (!stack_.empty()) {
            const StreamState& up = stack_.back();
            st.res.insert(st.res.end(), up.res.begin(), up.res.end());
            st.clip = up.clip;
            st.multiply = up.multiply;
            st.soft_mask = up.soft_mask;
            st.soft_alpha = up.soft_alpha;
            st.blend_other = up.blend_other;
            st.oc_hidden = up.oc_hidden + (up.do_hidden ? 1 : 0);
        }
        // Form /BBox clips the form (in the form's coordinate system, base_ctm).
        if (auto bb = cos::rect(cos::get(s.stream, "BBox"))) {
            double xs[4], ys[4];
            s.base_ctm.apply(bb->llx, bb->lly, xs[0], ys[0]);
            s.base_ctm.apply(bb->urx, bb->lly, xs[1], ys[1]);
            s.base_ctm.apply(bb->urx, bb->ury, xs[2], ys[2]);
            s.base_ctm.apply(bb->llx, bb->ury, xs[3], ys[3]);
            ClipNode n;
            n.parent = st.clip;
            n.box = {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                     *std::max_element(ys, ys + 4)};
            if (st.clip >= 0) {
                n.box = intersect_rects(n.box, clips_[std::size_t(st.clip)].box);
                n.chain_poly = clips_[std::size_t(st.clip)].chain_poly;
            }
            clips_.push_back(n);
            st.clip = int(clips_.size()) - 1;
        }
    }
    stack_.push_back(std::move(st));
    return &stack_.back();
}

void ContrastEngine::begin_page(const ContentModel& cm, int si) {
    page_ = cm.streams[si].page;
    overflow_ = false;
    first_item_ = cm.items.size();
    pool_.clear();
    subs_.clear();
    clips_.clear();
    paints_.clear();
    paint_of_.clear();
    hidden_ops_.clear();
    if (inline_pdf_) {   // inline images live for their page only
        for (auto it = images_.begin(); it != images_.end();) {
            if (!(it->first >> 63)) {
                ++it;
                continue;
            }
            image_bytes_ -= image_size(it->second.get());
            it = images_.erase(it);
        }
        image_order_.erase(std::remove_if(image_order_.begin(), image_order_.end(), [](std::uint64_t k) { return (k >> 63) != 0; }), image_order_.end());
        inline_pdf_.reset();
        inline_bytes_ = 0;
    }
    text_rgb_.clear();
    clip_masks_.clear();
    clip_mask_bytes_ = 0;
    pieces_.clear();
    glyph_boxes_.clear();
}

void ContrastEngine::end_path(StreamState& st) {
    st.pts.clear();
    st.subs.clear();
    st.open = false;
    st.rects = 0;
    st.other = false;
    st.box_valid = false;
    st.pending_clip = 0;
}

void ContrastEngine::add_clip(StreamState& st) {
    if (!st.pending_clip) return;
    ClipNode n;
    n.parent = st.clip;
    if (st.box_valid) n.box = st.box;   // an empty path clips everything (an empty box)
    const bool rect = st.rects == 1 && !st.other && st.subs.size() == 1 && st.pts.size() >= 4 &&
                      (st.pts[0].x == st.pts[1].x || st.pts[0].y == st.pts[1].y) &&
                      (st.pts[1].x == st.pts[2].x || st.pts[1].y == st.pts[2].y);
    if (!rect && st.box_valid && pool_.size() + st.pts.size() <= kMaxPathPoints) {
        n.has_poly = true;
        n.evenodd = st.pending_clip == 2;
        n.poly.first = std::uint32_t(subs_.size());
        n.poly.count = std::uint32_t(st.subs.size());
        std::uint32_t base = std::uint32_t(pool_.size());
        pool_.insert(pool_.end(), st.pts.begin(), st.pts.end());
        for (Sub s : st.subs) {
            s.first += base;
            subs_.push_back(s);
        }
    } else if (!rect && st.box_valid) {
        overflow_ = true;
    }
    if (st.clip >= 0) {
        n.box = intersect_rects(n.box, clips_[std::size_t(st.clip)].box);
        if (clips_[std::size_t(st.clip)].chain_poly) n.chain_poly = true;
    }
    if (n.has_poly) n.chain_poly = true;
    clips_.push_back(std::move(n));
    st.clip = int(clips_.size()) - 1;
}

void ContrastEngine::record_paint(const ContentModel&, StreamState& st, int si, std::uint32_t op_index, const GState& gs, bool fill,
                                  bool stroke, bool evenodd) {
    Paint p;
    p.kind = Paint::Path;
    p.fill = fill;
    p.stroke = stroke;
    p.evenodd = evenodd;
    p.clip = st.clip;
    p.box = st.box;
    p.blend_multiply = st.multiply;
    p.unknown = st.soft_mask || st.blend_other;
    p.fill_alpha = float(gs.fill_alpha) * st.soft_alpha;
    p.stroke_alpha = float(gs.stroke_alpha) * st.soft_alpha;
    const double scale = std::sqrt(std::abs(gs.ctm.a * gs.ctm.d - gs.ctm.b * gs.ctm.c));
    p.line_width = float(gs.line_width * scale);
    p.ctm = gs.ctm;
    if (fill) {
        if (gs.fill.family == ColorFamily::Pattern) {
            Obj pat = st.fill_pattern;
            if (pat.isDictionary() && cos::integer(pat, "PatternType").value_or(0) == 2) {
                p.object = cos::get(pat, "Shading");
                p.object_pattern = true;
                Matrix pm;
                auto mtx = cos::array_items(cos::get(pat, "Matrix"));
                if (mtx.size() == 6)
                    pm = Matrix{cos::number(mtx[0]).value_or(1), cos::number(mtx[1]).value_or(0), cos::number(mtx[2]).value_or(0),
                                cos::number(mtx[3]).value_or(1), cos::number(mtx[4]).value_or(0), cos::number(mtx[5]).value_or(0)};
                // A pattern matrix maps to the default coordinates of the page (or of the enclosing form).
                const StreamInstance& base = ctx_.content.streams[si];
                p.ctm = pm * base.base_ctm;
            } else if (pat.isStream() && cos::integer(pat, "PatternType").value_or(0) == 1) {
                // PAC renders tiling patterns (probes cb_tiling_*): one cell as a small raster, see tile_of
                p.object = pat;
                p.tiling = true;
                Matrix pm;
                auto mtx = cos::array_items(cos::get(pat, "Matrix"));
                if (mtx.size() == 6)
                    pm = Matrix{cos::number(mtx[0]).value_or(1), cos::number(mtx[1]).value_or(0), cos::number(mtx[2]).value_or(0),
                                cos::number(mtx[3]).value_or(1), cos::number(mtx[4]).value_or(0), cos::number(mtx[5]).value_or(0)};
                p.ctm = pm * ctx_.content.streams[si].base_ctm;
                if (cos::integer(pat, "PaintType").value_or(1) == 2) {
                    // uncoloured: the colour comes with the scn operator, in the underlying colour space of the /Pattern space
                    double rgb[3];
                    if (st.fill_comps.size() == 1) rgb[0] = rgb[1] = rgb[2] = st.fill_comps[0];
                    else if (st.fill_comps.size() == 3) rgb[0] = st.fill_comps[0], rgb[1] = st.fill_comps[1], rgb[2] = st.fill_comps[2];
                    else if (st.fill_comps.size() == 4) pac_cmyk_paint(st.fill_comps.data(), rgb);
                    else p.unknown = true;
                    if (!p.unknown) for (int k = 0; k < 3; ++k) p.fill_rgb[k] = to8(rgb[k]);
                }
            } else {
                p.unknown = true;
            }
        } else if (!gs.fill.known()) {
            double rgb[3];
            if (tint_paint(gs.fill_cs, st.fill_comps, st, rgb)) for (int k = 0; k < 3; ++k) p.fill_rgb[k] = to8(rgb[k]);
            else p.unknown = true;
        } else if (gs.fill.family == ColorFamily::CMYK && st.fill_comps.size() >= 4) {
            double rgb[3];
            pac_cmyk_paint(st.fill_comps.data(), rgb);
            for (int k = 0; k < 3; ++k) p.fill_rgb[k] = to8(rgb[k]);
        } else {
            for (int k = 0; k < 3; ++k) p.fill_rgb[k] = to8(gs.fill.rgb[k]);
        }
    }
    if (stroke) {
        double rgb[3];
        if (!gs.stroke.known()) {
            if (tint_paint(gs.stroke_cs, st.stroke_comps, st, rgb)) for (int k = 0; k < 3; ++k) p.stroke_rgb[k] = to8(rgb[k]);
            else p.unknown = true;
        } else if (gs.stroke.family == ColorFamily::CMYK && st.stroke_comps.size() >= 4) {
            pac_cmyk_paint(st.stroke_comps.data(), rgb);
            for (int k = 0; k < 3; ++k) p.stroke_rgb[k] = to8(rgb[k]);
        } else {
            for (int k = 0; k < 3; ++k) p.stroke_rgb[k] = to8(gs.stroke.rgb[k]);
        }
    }
    if (pool_.size() + st.pts.size() > kMaxPathPoints) {
        overflow_ = true;
        return;
    }
    p.path.first = std::uint32_t(subs_.size());
    p.path.count = std::uint32_t(st.subs.size());
    std::uint32_t base = std::uint32_t(pool_.size());
    pool_.insert(pool_.end(), st.pts.begin(), st.pts.end());
    for (Sub s : st.subs) {
        s.first += base;
        subs_.push_back(s);
    }
    paint_of_[(std::uint64_t(std::uint32_t(si)) << 32) | op_index] = int(paints_.size());
    paints_.push_back(std::move(p));
}

bool ContrastEngine::tint_paint(const Obj& cso, const std::vector<double>& comps, const StreamState& st, double* rgb) const {
    auto cs = ColorSpace::make(cso, st.res);
    if (!cs || cs->family == ColorSpace::Bad || int(comps.size()) < cs->ncomp) return false;
    std::vector<double> base;
    if (cs->family == ColorSpace::Tint && cs->base->family == ColorSpace::CMYK && cs->tint && cs->tint->eval(comps.data(), std::size_t(cs->ncomp), base) && base.size() >= 4) {
        pac_cmyk_paint(base.data(), rgb);
        return true;
    }
    std::vector<double> scratch;
    cs->to_rgb(comps.data(), rgb, scratch);
    return true;
}

void ContrastEngine::begin_text_op(int si, std::uint32_t op_index, const RawOp& op, const GState& gs) {
    text_stream_ = si;
    text_op_ = op_index;
    text_dir_x_ = tm_a_ * gs.ctm.a + tm_b_ * gs.ctm.c;
    text_dir_y_ = tm_a_ * gs.ctm.b + tm_b_ * gs.ctm.d;
    if (const double len = std::hypot(text_dir_x_, text_dir_y_); len > 1e-9) text_dir_x_ /= len, text_dir_y_ /= len;
    else text_dir_x_ = 1, text_dir_y_ = 0;
    piece_ends_.clear();
    piece_at_ = 0;
    piece_valid_ = piece_letter_ = piece_ink_ = false;
    piece_text_.clear();
    // the first text of a page: room for as many glyph boxes and pieces as the page before had
    if (glyph_boxes_.capacity() == 0 && boxes_hint_ > 0) glyph_boxes_.reserve(boxes_hint_);
    if (pieces_.capacity() == 0 && pieces_hint_ > 0) pieces_.reserve(pieces_hint_);
    // TJ: a number ends a piece (n_tj_kern_small, n_tj_kern_zero: strings in a row stay one piece; a real-world sample)
    if (op.code == Op::TJ && gs.font && !op.operands->empty() && op.operands->back().type == Operand::Array) {
        std::uint32_t glyphs = 0;
        bool open = false;
        for (const Operand& e : op.operands->back().items) {
            if (e.type == Operand::String) {
                code_scratch_.clear();
                gs.font->codes(e.s, code_scratch_);
                glyphs += std::uint32_t(code_scratch_.size());
                open = open || !code_scratch_.empty();
            } else if (e.is_number() && open) {
                piece_ends_.push_back(glyphs);
                open = false;
            }
        }
    }
}

// The facts on_glyph takes from the Unicode value of a character code. They depend on the font and the code only (a font
// maps a code to the same value for its lifetime), so they are read once and kept: per font, pages of 64 codes.
ContrastEngine::CodeText ContrastEngine::read_code_text(const Font* font, std::uint32_t code) {
    CodeText t;
    t.flags = CodeText::kKnown;
    const std::u32string* u = font ? font->to_unicode(code) : nullptr;
    if (!u) {
        t.flags |= CodeText::kNoUnicode;
        return t;
    }
    int inking = 0;
    char32_t one = 0;
    for (char32_t c : *u) {
        if (is_unicode_space(c) || c == 0) continue;   // U+0000 draws nothing (a real-world sample: not judged)
        ++inking;
        one = c;
        t.flags |= CodeText::kInk;
        // a symbol is no letter: arrow, bullet, box stay unflagged (oa_art_bullet; two real-world samples, 2 each)
        const bool symbol = c == 0xD7 || c == 0xF7 || (c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xE000 && c <= 0xF8FF) || c >= 0xFFF0;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && !symbol)) t.flags |= CodeText::kLetter;
    }
    if (inking == 1) {
        std::string s;
        append_utf8(s, one);   // at most 4 bytes
        t.flags |= CodeText::kOneChar;
        t.len = std::uint8_t(s.size());
        std::copy(s.begin(), s.end(), t.utf8);
    }
    return t;
}

// The entry of (font, code) when it is not the read entry of the font used last: reads it, and makes `font` that font.
ContrastEngine::CodeText ContrastEngine::code_text_slow(const Font* font, std::uint32_t code) {
    if (!font || code >= 0x10000) return read_code_text(font, code);   // not kept: no font, or a code beyond the tables
    std::vector<std::unique_ptr<CodeText[]>>& pages = code_texts_[font];
    const std::size_t p = code >> 6;
    if (p >= pages.size()) pages.resize(p + 1);
    if (!pages[p]) pages[p] = std::make_unique<CodeText[]>(64);
    code_text_font_ = font;
    code_text_pages_ = pages.data();   // after the resize: the array may have moved
    code_text_npages_ = pages.size();
    CodeText& t = pages[p][code & 63];
    if (!(t.flags & CodeText::kKnown)) t = read_code_text(font, code);
    return t;
}

inline ContrastEngine::CodeText ContrastEngine::code_text(const Font* font, std::uint32_t code) {
    const std::size_t p = code >> 6;
    if (font == code_text_font_ && p < code_text_npages_) {
        if (const CodeText* page = code_text_pages_[p].get()) {
            const CodeText t = page[code & 63];
            if (t.flags & CodeText::kKnown) return t;
        }
    }
    return code_text_slow(font, code);
}

void ContrastEngine::on_glyph(const ContentModel& cm, const GlyphEvent& ev) {
    if (ev.stream != text_stream_ || ev.op_index != text_op_ || !ev.real_content) return;
    while (piece_at_ < piece_ends_.size() && ev.glyph_index >= piece_ends_[piece_at_]) {
        close_piece(cm);
        ++piece_at_;
    }
    glyph_frame_ = ev.frame;
    // The glyph box, upright around the turned glyph (rotated text: rt_* probes: 6.5 pt letters along a band of 9 pt and more pass).
    const double along = std::max(ev.advance_user, 0.01), dx = text_dir_x_, dy = text_dir_y_;
    double x0 = ev.x, x1 = ev.x + along, y0 = ev.y - 0.2 * ev.size_user, y1 = ev.y + 0.8 * ev.size_user;
    if (std::abs(dy) > 0.02) {
        const double xs[4] = {-0.2 * ev.size_user * -dy, along * dx - 0.2 * ev.size_user * -dy, 0.8 * ev.size_user * -dy, along * dx + 0.8 * ev.size_user * -dy};
        const double ys[4] = {-0.2 * ev.size_user * dx, along * dy - 0.2 * ev.size_user * dx, 0.8 * ev.size_user * dx, along * dy + 0.8 * ev.size_user * dx};
        x0 = ev.x + *std::min_element(xs, xs + 4), x1 = ev.x + *std::max_element(xs, xs + 4);
        y0 = ev.y + *std::min_element(ys, ys + 4), y1 = ev.y + *std::max_element(ys, ys + 4);
    }
    if (!piece_valid_) {
        piece_box_ = {x0, y0, x1, y1};
        piece_valid_ = true;
        piece_first_glyph_ = std::uint32_t(glyph_boxes_.size() / 4);
    } else {
        piece_box_.llx = std::min(piece_box_.llx, x0);
        piece_box_.lly = std::min(piece_box_.lly, y0);
        piece_box_.urx = std::max(piece_box_.urx, x1);
        piece_box_.ury = std::max(piece_box_.ury, y1);
    }
    // What the code's Unicode value says (code_text: read once per font and code).
    const CodeText ct = code_text(ev.font, ev.code.code);
    bool blank = true;
    if (ct.flags & CodeText::kNoUnicode) {
        piece_letter_ = piece_ink_ = true;     // a glyph without Unicode counts as text
        blank = false;
    } else if (ct.flags & CodeText::kInk) {
        piece_ink_ = true;
        blank = false;
        if (ct.flags & CodeText::kLetter) piece_letter_ = true;
        // The text of the piece is read by close_piece for artifact text only: collected in such a frame, else left empty.
        if (ev.frame != text_frame_) {
            text_frame_ = ev.frame;
            text_frame_artifact_ = artifact_text_frame(cm, ev.frame);
        }
        if (text_frame_artifact_) {
            if (ct.flags & CodeText::kOneChar) {
                if (piece_text_.size() < 60) piece_text_.append(ct.utf8, ct.len);
            } else {   // several characters: one by one, each only while the text is short
                for (char32_t c : *ev.font->to_unicode(ev.code.code)) {
                    if (is_unicode_space(c) || c == 0) continue;
                    if (piece_text_.size() < 60) append_utf8(piece_text_, c);
                }
            }
        }
    }
    if (!blank) {
        glyph_boxes_.push_back(float(x0));
        glyph_boxes_.push_back(float(y0));
        glyph_boxes_.push_back(float(x1));
        glyph_boxes_.push_back(float(y1));
    }
}

// Whether close_piece takes a piece whose glyphs are in `frame` (of the text operator's stream) for artifact text: an
// /Artifact sequence around it, none of them with an MCID an element owns, and the stream not inside an artifact
// already. The frames around a text operator do not change while it is shown.
bool ContrastEngine::artifact_text_frame(const ContentModel& cm, int frame) const {
    if (frame < 0) return false;
    bool artifact = false, mcid = false;
    for (int f = frame; f >= 0; f = cm.frames[std::size_t(f)].parent) {
        if (cm.frames[std::size_t(f)].tag != "Artifact") continue;
        artifact = true;
        if (cm.frames[std::size_t(f)].mcid && cm.frames[std::size_t(f)].own_struct_elem.isDictionary()) mcid = true;
    }
    if (cm.streams[std::size_t(text_stream_)].inherited_artifact) artifact = false;
    return artifact && !mcid;
}

void ContrastEngine::close_piece(const ContentModel& cm) {
    if (!piece_valid_) return;
    const StreamInstance& s = cm.streams[text_stream_];
    // Artifact text (PAC ArtifactedBodyParagraphs): the /Artifact sequence must hold the text operator in the
    // same stream.
    if (piece_letter_ && glyph_frame_ >= 0) {
        // An MCID on the /Artifact sequence itself makes the text tagged; one inside it or around it does not (ai_inner_mcid,
        // ai_outer_mcid, ag_*; a real-world sample: /Artifact BDC /P <</MCID n>> BDC text EMC EMC, 127 pieces).
        // A Form XObject painted inside an /Artifact is not looked into at all, its own /Artifact sequences neither
        // (art_form_inside, a real-world Canva export).
        // ... an MCID counts when an element owns it: an MCID nobody claims leaves it an artifact (ct_artifact_with_mcid).
        // (artifact_text_frame; on_glyph collected the piece's text for such a frame.)
        if (artifact_text_frame(cm, glyph_frame_)) artifact_pieces.push_back({s.page, s.ref, piece_box_, piece_text_});
    }
    if (piece_ink_) {
        const std::uint32_t n = std::uint32_t(glyph_boxes_.size() / 4) - piece_first_glyph_;
        pieces_.push_back({text_stream_, text_op_, piece_box_, piece_first_glyph_, n, text_clip_, text_alpha_, text_blend_});
    }
    piece_valid_ = piece_letter_ = piece_ink_ = false;
    piece_text_.clear();
}

void ContrastEngine::end_text_op(const ContentModel& cm) {
    if (text_stream_ < 0) return;
    close_piece(cm);
    text_stream_ = -1;
}

void ContrastEngine::on_operator(const ContentModel& cm, int si, std::uint32_t op_index, const RawOp& op, const GState& gs) {
    end_text_op(cm);
    if (op.code == Op::BT) tm_a_ = 1, tm_b_ = 0;
    if (op.code == Op::Tm && op.operands->size() >= 6 && (*op.operands)[op.operands->size() - 6].is_number() && (*op.operands)[op.operands->size() - 5].is_number())
        tm_a_ = (*op.operands)[op.operands->size() - 6].num(), tm_b_ = (*op.operands)[op.operands->size() - 5].num();
    if (!cm.streams[si].semantic || cm.streams[si].page < 0) return;
    if (op.code == Op::Tj || op.code == Op::TJ || op.code == Op::quote || op.code == Op::dquote) begin_text_op(si, op_index, op, gs);
    StreamState* stp = state_for(cm, si);
    if (!stp) return;
    StreamState& st = *stp;
    if (text_stream_ == si && text_op_ == op_index) {   // the text operator begin_text_op took up
        text_clip_ = st.clip;
        text_alpha_ = st.soft_alpha;
        text_blend_ = st.soft_mask || st.multiply || st.blend_other;
    }
    const std::vector<Operand>& a = *op.operands;
    // A named resource that does not exist: PAC 26.1 closes the document (reporting the missing font or shading by name; probes sx_tf_unknown_font, sx_gs_unknown, sx_do_unknown, sx_sh_unknown,
    // sx_cs_unknown, sx_bdc_unknown_prop). Reported, so that such a file never passes silently.
    {
        const char* cat = nullptr;
        const Operand* nm = nullptr;
        switch (op.code) {
            case Op::gs: cat = "ExtGState"; nm = a.empty() ? nullptr : &a.back(); break;
            case Op::Do: cat = "XObject"; nm = a.empty() ? nullptr : &a.back(); break;
            case Op::sh: cat = "Shading"; nm = a.empty() ? nullptr : &a.back(); break;
            case Op::Tf: cat = "Font"; nm = a.size() < 2 ? nullptr : &a[a.size() - 2]; break;
            case Op::cs:
            case Op::CS:
                if (!a.empty() && a.back().type == Operand::Name && a.back().s != "DeviceGray" && a.back().s != "DeviceRGB" && a.back().s != "DeviceCMYK" && a.back().s != "Pattern")
                    cat = "ColorSpace", nm = &a.back();
                break;
            case Op::BDC: cat = "Properties"; nm = a.empty() ? nullptr : &a.back(); break;
            case Op::EI:
                if (!a.empty() && a.back().type == Operand::Dict) {
                    const Operand* c = a.back().get("CS");
                    if (!c) c = a.back().get("ColorSpace");
                    if (c && c->type == Operand::Name && c->s != "G" && c->s != "RGB" && c->s != "CMYK" && c->s != "I" && c->s != "DeviceGray" && c->s != "DeviceRGB" && c->s != "DeviceCMYK" && c->s != "Indexed")
                        cat = "ColorSpace", nm = c;
                }
                break;
            default: break;
        }
        if (cat && nm && nm->type == Operand::Name && res_lookup(st.res, cat, nm->s).isNull() && missing_res.size() < 1000)
            missing_res.push_back({cm.streams[si].page, cm.streams[si].ref, "PAC: " + std::string(op.op) + " names the resource /" + nm->s + ", no such " + cat + " (PAC closes the document)"});
    }
    // Content of an optional content group that is off is not on the page for PAC (probes cb_oc_off_bg_*).
    switch (op.code) {
        case Op::BMC: st.oc_stack.push_back(0); break;
        case Op::BDC: {
            bool off = false;
            if (a.size() >= 2 && a[a.size() - 2].type == Operand::Name && a[a.size() - 2].s == "OC" && a.back().type == Operand::Name)
                off = oc_is_off(res_lookup(st.res, "Properties", a.back().s));
            st.oc_stack.push_back(off ? 1 : 0);
            if (off) ++st.oc_hidden;
            break;
        }
        case Op::EMC:
            if (!st.oc_stack.empty()) {
                if (st.oc_stack.back()) --st.oc_hidden;
                st.oc_stack.pop_back();
            }
            break;
        case Op::Do:
            st.do_hidden = !a.empty() && a.back().type == Operand::Name && oc_is_off(cos::get(res_lookup(st.res, "XObject", a.back().s), "OC"));
            if (st.do_hidden) hidden_ops_.insert((std::uint64_t(std::uint32_t(si)) << 32) | op_index);
            break;
        default: break;
    }
    if (st.oc_hidden > 0) hidden_ops_.insert((std::uint64_t(std::uint32_t(si)) << 32) | op_index);
    auto nums = [&](std::size_t n) {
        if (a.size() < n) return false;
        for (std::size_t i = a.size() - n; i < a.size(); ++i)
            if (!a[i].is_number()) return false;
        return true;
    };
    auto num = [&](std::size_t back) { return a[a.size() - back].num(); };
    auto device = [&](double x, double y) {
        double dx, dy;
        gs.ctm.apply(x, y, dx, dy);
        Pt p{float(dx), float(dy)};
        if (!st.box_valid) {
            st.box = {dx, dy, dx, dy};
            st.box_valid = true;
        } else {
            st.box.llx = std::min(st.box.llx, dx);
            st.box.lly = std::min(st.box.lly, dy);
            st.box.urx = std::max(st.box.urx, dx);
            st.box.ury = std::max(st.box.ury, dy);
        }
        return p;
    };
    auto begin_sub = [&]() {
        Sub s;
        s.first = std::uint32_t(st.pts.size());
        st.subs.push_back(s);
    };
    auto sub_end = [&]() {
        if (!st.subs.empty()) st.subs.back().count = std::uint32_t(st.pts.size()) - st.subs.back().first;
    };
    auto extend_box = [&](std::size_t from) {
        for (std::size_t i = from; i < st.pts.size(); ++i) {
            st.box.llx = std::min(st.box.llx, double(st.pts[i].x));
            st.box.lly = std::min(st.box.lly, double(st.pts[i].y));
            st.box.urx = std::max(st.box.urx, double(st.pts[i].x));
            st.box.ury = std::max(st.box.ury, double(st.pts[i].y));
        }
    };
    switch (op.code) {
        case Op::q:
            st.saved.push_back({st.clip, st.multiply, st.soft_mask, st.blend_other, st.soft_alpha, st.fill_comps, st.stroke_comps});
            return;
        case Op::Q:
            if (!st.saved.empty()) {
                const SavedState& s = st.saved.back();
                st.clip = s.clip;
                st.multiply = s.multiply;
                st.soft_mask = s.soft_mask;
                st.soft_alpha = s.soft_alpha;
                st.blend_other = s.blend_other;
                st.fill_comps = s.fill_comps;
                st.stroke_comps = s.stroke_comps;
                st.saved.pop_back();
            }
            return;
        case Op::m:
            if (nums(2)) {
                sub_end();
                begin_sub();
                st.ux = st.sx = num(2);
                st.uy = st.sy = num(1);
                st.pts.push_back(device(st.ux, st.uy));
                st.other = true;
                st.open = true;
            }
            return;
        case Op::l:
            if (nums(2)) {
                if (st.subs.empty()) begin_sub();
                st.ux = num(2);
                st.uy = num(1);
                st.pts.push_back(device(st.ux, st.uy));
                st.other = true;
            }
            return;
        case Op::c:
        case Op::v:
        case Op::y:
            if (nums(op.code == Op::c ? 6 : 4)) {
                double x1, y1, x2, y2, x3, y3;
                if (op.code == Op::c) x1 = num(6), y1 = num(5), x2 = num(4), y2 = num(3), x3 = num(2), y3 = num(1);
                else if (op.code == Op::v) x1 = st.ux, y1 = st.uy, x2 = num(4), y2 = num(3), x3 = num(2), y3 = num(1);
                else x1 = num(4), y1 = num(3), x2 = num(2), y2 = num(1), x3 = x2, y3 = y2;
                if (st.subs.empty()) begin_sub();
                device(st.ux, st.uy);
                std::size_t from = st.pts.size();
                flatten_curve(st.pts, gs.ctm, st.ux, st.uy, x1, y1, x2, y2, x3, y3);
                extend_box(from);
                st.ux = x3;
                st.uy = y3;
                st.other = true;
            }
            return;
        case Op::h:
            if (!st.subs.empty()) {
                sub_end();
                st.subs.back().closed = true;
                st.ux = st.sx;
                st.uy = st.sy;
            }
            return;
        case Op::re:
            if (nums(4)) {
                double x = num(4), y = num(3), w = num(2), h = num(1);
                sub_end();
                begin_sub();
                st.pts.push_back(device(x, y));
                st.pts.push_back(device(x + w, y));
                st.pts.push_back(device(x + w, y + h));
                st.pts.push_back(device(x, y + h));
                st.subs.back().closed = true;
                sub_end();
                st.ux = st.sx = x;
                st.uy = st.sy = y;
                ++st.rects;
            }
            return;
        case Op::W:
            st.pending_clip = 1;
            return;
        case Op::W_star:
            st.pending_clip = 2;
            return;
        case Op::n:
            sub_end();
            add_clip(st);
            end_path(st);
            return;
        case Op::f:
        case Op::F:
        case Op::f_star:
        case Op::S:
        case Op::s:
        case Op::B:
        case Op::B_star:
        case Op::b:
        case Op::b_star: {
            sub_end();
            const bool fill = op.code != Op::S && op.code != Op::s;
            const bool stroke = op.code == Op::S || op.code == Op::s || op.code == Op::B || op.code == Op::B_star ||
                                op.code == Op::b || op.code == Op::b_star;
            if (op.code == Op::s || op.code == Op::b || op.code == Op::b_star)
                if (!st.subs.empty()) st.subs.back().closed = true;
            if (st.box_valid)
                record_paint(cm, st, si, op_index, gs, fill, stroke,
                             op.code == Op::f_star || op.code == Op::B_star || op.code == Op::b_star);
            add_clip(st);
            end_path(st);
            return;
        }
        case Op::gs: {
            if (a.empty() || a.back().type != Operand::Name) return;
            Obj eg = res_lookup(st.res, "ExtGState", a.back().s);
            if (!eg.isDictionary()) return;
            Obj bm = cos::get(eg, "BM");
            if (bm.isArray() && bm.getArrayNItems() > 0) bm = bm.getArrayItem(0);
            if (auto n = cos::name(bm)) {
                st.multiply = *n == "Multiply";
                st.blend_other = !(*n == "Normal" || *n == "Compatible" || *n == "Multiply");
            }
            Obj sm = cos::get(eg, "SMask");
            if (!sm.isNull()) {
                const bool none = sm.isName() && cos::name(sm).value_or("") == "None";
                const double alpha = none ? 1.0 : soft_mask_value(sm);
                st.soft_mask = alpha < 0;
                st.soft_alpha = alpha < 0 ? 1.f : float(alpha);
            }
            return;
        }
        case Op::k:
        case Op::K: {
            std::vector<double>& out = op.code == Op::k ? st.fill_comps : st.stroke_comps;
            out.clear();
            for (const Operand& x : a)
                if (x.is_number()) out.push_back(x.num());
            return;
        }
        case Op::sc:
        case Op::scn: {
            if (!a.empty() && a.back().type == Operand::Name) st.fill_pattern = res_lookup(st.res, "Pattern", a.back().s);
            st.fill_comps.clear();
            for (const Operand& x : a)
                if (x.is_number()) st.fill_comps.push_back(x.num());
            return;
        }
        case Op::SC:
        case Op::SCN: {
            st.stroke_comps.clear();
            for (const Operand& x : a)
                if (x.is_number()) st.stroke_comps.push_back(x.num());
            return;
        }
        case Op::Tj:
        case Op::TJ:
        case Op::quote:
        case Op::dquote: {
            // The interpreter knows no colour for Separation and DeviceN: compute it here for the text run. PAC does not judge
            // text in a Lab colour (h_lab_100: white on white passes). CalGray: PAC takes A^Gamma as the grey value
            // (g_cal22_60, s_calgray_50 pass, g_cal1_50, g_cal1_70 fail).
            const bool stroke_only = uses_stroke(gs.tr);
            const Color& c = stroke_only ? gs.stroke : gs.fill;
            const Obj& cso = stroke_only ? gs.stroke_cs : gs.fill_cs;
            const std::vector<double>& comps = stroke_only ? st.stroke_comps : st.fill_comps;
            double rgb[3];
            if (c.family == ColorFamily::Gray && cso.isArray() && cso.getArrayNItems() > 1 && cos::name(cso.getArrayItem(0)).value_or("") == "CalGray") {
                if (comps.empty()) return;
                const double g = std::pow(clamp01(comps[0]), cos::number(cso.getArrayItem(1), "Gamma").value_or(1));
                rgb[0] = rgb[1] = rgb[2] = g;
            } else if (c.family == ColorFamily::CMYK) {
                if (comps.size() < 4) return;
                pac_cmyk(comps.data(), rgb);
            } else {
                if (c.family != ColorFamily::Unknown) return;
                auto cs = ColorSpace::make(cso, st.res);
                if (!cs || cs->family == ColorSpace::Lab || int(comps.size()) < cs->ncomp) return;
                // PAC does not judge text whose tint transform is a PostScript calculator (t4_*, nc_*_f4_*: not even counted;
                // a real-world sample: 538 pieces), though it paints such colours (t4_bg_black).
                if (cs->family == ColorSpace::Tint && cs->tint && cs->tint->is_calculator()) return;
                std::vector<double> scratch;
                std::vector<double> base;
                if (cs->family == ColorSpace::Tint && cs->base->family == ColorSpace::CMYK && cs->tint && cs->tint->eval(comps.data(), std::size_t(cs->ncomp), base) && base.size() >= 4)
                    pac_cmyk(base.data(), rgb);   // the same inks as CMYK text
                else cs->to_rgb(comps.data(), rgb, scratch);
            }
            text_rgb_[(std::uint64_t(std::uint32_t(si)) << 32) | op_index] = {to8(rgb[0]), to8(rgb[1]), to8(rgb[2])};
            return;
        }
        case Op::sh: {
            if (a.empty() || a.back().type != Operand::Name) return;
            Paint p;
            p.kind = Paint::Shading;
            p.clip = st.clip;
            p.object = res_lookup(st.res, "Shading", a.back().s);
            p.ctm = gs.ctm;
            p.fill_alpha = float(gs.fill_alpha) * st.soft_alpha;
            p.blend_multiply = st.multiply;
            p.unknown = st.soft_mask || st.blend_other;
            const auto& pages = ctx_.doc.pages();
            p.box = page_ >= 0 && page_ < int(pages.size()) ? pages[page_].crop_box : Rect{};
            paint_of_[(std::uint64_t(std::uint32_t(si)) << 32) | op_index] = int(paints_.size());
            paints_.push_back(std::move(p));
            return;
        }
        case Op::Do: {
            if (a.empty() || a.back().type != Operand::Name) return;
            Obj xo = res_lookup(st.res, "XObject", a.back().s);
            if (!xo.isStream() || cos::name(xo, "Subtype").value_or("") != "Image") return;
            Paint p;
            p.kind = Paint::Image;
            p.clip = st.clip;
            p.object = xo;
            p.ctm = gs.ctm;
            p.fill_alpha = float(gs.fill_alpha) * st.soft_alpha;
            p.blend_multiply = st.multiply;
            p.unknown = st.soft_mask || st.blend_other;
            for (int k = 0; k < 3; ++k) p.fill_rgb[k] = gs.fill.known() ? to8(gs.fill.rgb[k]) : 0;
            if (cos::boolean(xo, "ImageMask").value_or(false) && !gs.fill.known()) p.unknown = true;
            double xs[4], ys[4];
            gs.ctm.apply(0, 0, xs[0], ys[0]);
            gs.ctm.apply(1, 0, xs[1], ys[1]);
            gs.ctm.apply(0, 1, xs[2], ys[2]);
            gs.ctm.apply(1, 1, xs[3], ys[3]);
            p.box = {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                     *std::max_element(ys, ys + 4)};
            paint_of_[(std::uint64_t(std::uint32_t(si)) << 32) | op_index] = int(paints_.size());
            paints_.push_back(std::move(p));
            return;
        }
        case Op::EI: {
            // PAC decodes inline images like image XObjects (probes cb_inline_*).
            Paint p;
            p.kind = Paint::Image;
            p.clip = st.clip;
            p.object = inline_image_stream(op, st);
            p.unknown = !p.object.isStream() || st.soft_mask || st.blend_other;
            p.ctm = gs.ctm;
            p.fill_alpha = float(gs.fill_alpha) * st.soft_alpha;
            p.blend_multiply = st.multiply;
            for (int k = 0; k < 3; ++k) p.fill_rgb[k] = gs.fill.known() ? to8(gs.fill.rgb[k]) : 0;
            if (p.object.isStream() && cos::boolean(p.object, "ImageMask").value_or(false) && !gs.fill.known()) p.unknown = true;
            double xs[4], ys[4];
            gs.ctm.apply(0, 0, xs[0], ys[0]);
            gs.ctm.apply(1, 0, xs[1], ys[1]);
            gs.ctm.apply(0, 1, xs[2], ys[2]);
            gs.ctm.apply(1, 1, xs[3], ys[3]);
            p.box = {*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
                     *std::max_element(ys, ys + 4)};
            paint_of_[(std::uint64_t(std::uint32_t(si)) << 32) | op_index] = int(paints_.size());
            paints_.push_back(std::move(p));
            return;
        }
        default:
            return;
    }
}

// The default configuration: /BaseState, /ON, /OFF; a membership dictionary with its policy over /OCGs. A /VE is not evaluated (off is not claimed).
bool ContrastEngine::oc_is_off(const Obj& oc) {
    if (!oc.isDictionary()) return false;
    if (!oc_read_) {
        oc_read_ = true;
        Obj d = cos::get(cos::get(ctx_.doc.catalog(), "OCProperties"), "D");
        oc_base_off_ = cos::name_is(d, "BaseState", "OFF");
        for (const Obj& o : cos::array_items(cos::get(d, "OFF"))) oc_off_.insert(ref_of(o).key());
        for (const Obj& o : cos::array_items(cos::get(d, "ON"))) oc_on_.insert(ref_of(o).key());
    }
    const auto group_off = [&](const Obj& g) {
        const std::uint64_t k = ref_of(g).key();
        return oc_base_off_ ? oc_on_.count(k) == 0 : oc_off_.count(k) != 0;
    };
    if (!cos::name_is(oc, "Type", "OCMD")) return group_off(oc);
    if (cos::has(oc, "VE")) return false;
    Obj gs = cos::get(oc, "OCGs");
    std::vector<Obj> list = gs.isArray() ? cos::array_items(gs) : std::vector<Obj>{};
    if (gs.isDictionary()) list.push_back(gs);
    if (list.empty()) return false;
    int off = 0;
    for (const Obj& g : list) off += group_off(g) ? 1 : 0;
    const std::string p = cos::name(oc, "P").value_or("AnyOn");
    const int n = int(list.size());
    if (p == "AllOn") return off > 0;
    if (p == "AnyOff") return off == 0;
    if (p == "AllOff") return off < n;
    return off == n;
}

Obj ContrastEngine::inline_image_stream(const RawOp& op, const StreamState& st) {
    constexpr std::size_t kMaxInlineTotal = 16u << 20;   // per page; beyond it the area is unknown
    if (op.operands->empty() || op.operands->back().type != Operand::Dict || inline_bytes_ + op.inline_data.size() > kMaxInlineTotal) return Obj::newNull();
    try {
        if (!inline_pdf_) {
            inline_pdf_ = std::make_unique<QPDF>();
            inline_pdf_->emptyPDF();
        }
        Obj dict = Obj::newDictionary();
        dict.replaceKey("/Type", Obj::newName("/XObject"));
        dict.replaceKey("/Subtype", Obj::newName("/Image"));
        const Operand& params = op.operands->back();
        for (std::size_t k = 0; k + 1 < params.items.size(); k += 2) {
            std::string key = params.items[k].s;
            if (key == "BPC") key = "BitsPerComponent";
            else if (key == "CS") key = "ColorSpace";
            else if (key == "D") key = "Decode";
            else if (key == "DP") key = "DecodeParms";
            else if (key == "F") key = "Filter";
            else if (key == "H") key = "Height";
            else if (key == "IM") key = "ImageMask";
            else if (key == "I") key = "Interpolate";
            else if (key == "W") key = "Width";
            dict.replaceKey("/" + key, operand_to_obj(params.items[k + 1], key == "ColorSpace", key == "Filter", st.res));
        }
        Obj stream = inline_pdf_->newStream(std::string(op.inline_data));
        stream.replaceDict(dict);
        inline_bytes_ += op.inline_data.size();
        return stream;
    } catch (std::exception&) {
        return Obj::newNull();
    }
}

void ContrastEngine::on_stream_end(const ContentModel& cm, int si) {
    end_text_op(cm);
    if (stack_.empty() || stack_.back().si != si) return;
    stack_.pop_back();
    if (cm.streams[si].kind == StreamInstance::Kind::Page) {
        try {
            flush_page(cm);
        } catch (std::exception&) {
            // a damaged image or function must not lose the other findings of the document
        }
        pool_.clear();
        pool_.shrink_to_fit();
        subs_.clear();
        subs_.shrink_to_fit();
        clips_.clear();
        clips_.shrink_to_fit();
        paints_.clear();
        paints_.shrink_to_fit();
        paint_of_.clear();
        text_rgb_.clear();
        clip_masks_.clear();
        clip_mask_bytes_ = 0;
        // Freed with the page as before; the next page with text asks for as much at once (begin_text_op) instead of
        // growing to it by doubling, which copied every box about twice.
        pieces_hint_ = std::min<std::size_t>(pieces_.size(), kMaxSizeHint / 16);
        boxes_hint_ = std::min<std::size_t>(glyph_boxes_.size(), kMaxSizeHint);
        pieces_.clear();
        pieces_.shrink_to_fit();
        glyph_boxes_.clear();
        glyph_boxes_.shrink_to_fit();
        // after the document's last page no later page can reuse a reduced image: they are not kept until the end of the run
        // (where the content model of the whole document is largest)
        if (cm.streams[si].page + 1 >= int(ctx_.doc.pages().size())) {
            images_.clear();
            image_order_.clear();
            image_order_.shrink_to_fit();
            image_bytes_ = 0;
        }
#if defined(__GLIBC__)
        // A page full of clipping paths frees many masks (one bit per pixel since 2026-11-10, below glibc's mmap threshold): give
        // the freed heap back, else it stays in the process's resident size up to the end of the run. Only then: a trim walks the
        // whole heap (after every rendered page it cost a real-world sample 18 % CPU).
        if (page_mask_pixels_ >= kTrimMaskPixels) malloc_trim(0);
#endif
        page_mask_pixels_ = 0;
        page_ = -1;
    }
}

// --- EqualDoX WCAG colour checks (wcag_color.h) --------------------------------------------------------------

// The Figure element tagged content belongs to (Figure itself or an ancestor), cached per marked-content frame.
std::optional<ObjRef> ContrastEngine::figure_of(const ContentModel& cm, const ContentItem& it) {
    const Flavor fl = ctx_.pac_applicable ? Flavor::Pdf17 : Flavor::Pdf20;
    auto lookup = [&]() -> std::optional<ObjRef> {
        Obj e = cm.item_struct_parent(it);
        for (int guard = 0; e.isDictionary() && guard < 256; ++guard) {
            if (ctx_.st.standard_of(e, fl).value_or("") == "Figure") return ref_of(e);
            e = cos::get(e, "P");
        }
        return std::nullopt;
    };
    if (it.frame < 0) return lookup();
    const std::size_t f = std::size_t(it.frame);
    if (frame_fig_.size() <= f) frame_fig_.resize(std::max(cm.frames.size(), f + 1), -2);
    if (frame_fig_[f] == -2) {
        const std::optional<ObjRef> r = lookup();
        frame_fig_[f] = -1;
        if (r) {
            frame_fig_ref_.push_back(*r);
            frame_fig_[f] = int(frame_fig_ref_.size()) - 1;
        }
    }
    if (frame_fig_[f] < 0) return std::nullopt;
    return frame_fig_ref_[std::size_t(frame_fig_[f])];
}

// WCAG 1.4.3 on one measurement. `fg`, `bg`: the colours PAC's judgement used (text without its own transparency).
void ContrastEngine::wcag_text(const ContentModel& cm, const ContentItem& it, const TextPiece& piece, const Rect& box, const float* fg,
                               const float* bg, bool rendered, bool exact_bg, bool artifact) {
    static const RuleIndex rc = R("EQX-WCAG-1.4.3-TextContrast"), rr = R("EQX-WCAG-1.4.3-TextContrastReview");
    if (hidden_op(it)) return;   // optional content that is off
    const TextRun& tr = cm.texts[std::size_t(it.text)];
    bool clipped = false;
    if (piece.clip >= 0) {
        const ClipNode& c = clips_[std::size_t(piece.clip)];
        const Rect in = intersect_rects(box, c.box);
        if (in.empty()) return;   // clipped away: not visible
        clipped = c.chain_poly || in.llx > box.llx + 0.5 || in.lly > box.lly + 0.5 || in.urx < box.urx - 0.5 || in.ury < box.ury - 0.5;
    }
    // the text's own transparency (PAC ignores it) composed over the background
    const double a = std::clamp(double(tr.fill_alpha) * piece.alpha, 0.0, 1.0);
    wcag::TextFacts f;
    float fgc[3];
    for (int k = 0; k < 3; ++k) fgc[k] = float(a * fg[k] + (1 - a) * bg[k]);
    f.ratio = wcag_contrast(fgc, bg);
    if (f.ratio >= 4.5) return;
    for (int k = 0; k < 3; ++k) {   // the colours of the detail: only a text below 4.5:1 gets that far
        f.fg[k] = to8(fgc[k]);
        f.bg[k] = to8(bg[k]);
    }
    f.size = tr.size_user;
    // wcag::bold_state(tr.font, tr.tr): -1 without a font and for fill-and-stroke modes, else a fact of the font
    // dictionary, read once per font
    if (!tr.font || tr.tr == 2 || tr.tr == 6) {
        f.bold = -1;
    } else {
        auto known = bold_states_.find(tr.font);
        if (known == bold_states_.end()) known = bold_states_.emplace(tr.font, wcag::bold_state(tr.font, tr.tr)).first;
        f.bold = known->second;
    }
    f.rendered = rendered;
    const Color& tc = uses_stroke(tr.tr) ? tr.stroke : tr.fill;
    f.device_colour = !exact_bg || !(tc.family == ColorFamily::Gray || tc.family == ColorFamily::RGB);
    f.transparency = piece.blend;
    f.clipped = clipped;
    f.stroked = tr.tr != 0 && tr.tr != 4;
    f.artifact = artifact;
    f.in_figure = figure_of(cm, it).has_value();
    const wcag::TextJudgement j = wcag::judge_text(f);
    if (j.verdict == wcag::Verdict::Pass) return;
    const StreamInstance& s = cm.streams[std::size_t(it.stream)];
    const RuleIndex rule = j.verdict == wcag::Verdict::Violation ? rc : rr;
    if (ctx_.findings.saturated(rule)) ctx_.report(rule, s.page, s.ref);   // only counted: the detail would be dropped
    else ctx_.report(rule, s.page, s.ref, wcag::text_detail(f, j, box, to_utf8(tr.text)));
}

// What the engine could not measure leaves the WCAG rules open (never a silent pass).
void ContrastEngine::wcag_incomplete(bool page_skipped) {
    static const RuleIndex rc = R("EQX-WCAG-1.4.3-TextContrast"), rr = R("EQX-WCAG-1.4.3-TextContrastReview"),
                           rn = R("EQX-WCAG-1.4.11-NonTextContrastReview");
    const std::string where = "page " + std::to_string(page_ + 1);
    const std::string skipped = where + " holds more geometry than the contrast engine keeps: its colours were not measured";
    if (page_skipped && !wcag_inc_all_) {
        wcag_inc_all_ = true;
        ctx_.mark_incomplete(rc, skipped);
        ctx_.mark_incomplete(rn, skipped);
    }
    if (!wcag_inc_review_) {
        wcag_inc_review_ = true;
        ctx_.mark_incomplete(rr, page_skipped ? skipped
                                              : "the contrast engine's rendering budget ran out on " + where +
                                                    ": text on rendered backgrounds there and on later pages was not measured");
    }
}

// WCAG 1.4.11 for the painted parts of figures: fill or stroke against the background around the part, which is the last earlier fill
// under at least half of it (white when there is none). Only a flat background is measured (an opaque rectangle covering the part);
// whether a part is needed to understand the figure takes a reviewer, so one finding per Figure and page is for review.
template <class CellRange, class PaintedBox>
void ContrastEngine::wcag_figures(const ContentModel& cm, const std::vector<int>& parts, const std::vector<std::vector<int>>& cells, int n,
                                  const CellRange& cell_range, const PaintedBox& painted_box) {
    static const RuleIndex rn = R("EQX-WCAG-1.4.11-NonTextContrastReview");
    struct Fig {
        ObjRef ref;
        int parts = 0, low = 0;
        double worst = 99;
        unsigned char fg[3] = {0, 0, 0}, bg[3] = {255, 255, 255};
        Rect box;
    };
    std::vector<Fig> figs;
    auto key = [](const ContentItem& i) { return (std::uint64_t(std::uint32_t(i.stream)) << 32) | i.op_index; };
    const Rect& mb = ctx_.doc.pages()[std::size_t(page_)].media_box;
    const double page_area = std::max(1.0, mb.width() * mb.height());
    for (const int idx : parts) {
        const ContentItem& it = cm.items[std::size_t(idx)];
        auto rec = paint_of_.find(key(it));
        if (rec == paint_of_.end()) continue;
        const Paint& p = paints_[std::size_t(rec->second)];
        if (p.unknown || p.blend_multiply) continue;
        Rect pb = painted_box(it);
        if (std::max(pb.width(), pb.height()) < 3) continue;   // a dot, a tick: too small to judge
        if (pb.width() < 1) pb.llx = (pb.llx + pb.urx) / 2 - 0.5, pb.urx = pb.llx + 1;
        if (pb.height() < 1) pb.lly = (pb.lly + pb.ury) / 2 - 0.5, pb.ury = pb.lly + 1;
        const double area = pb.width() * pb.height();
        int x0, y0, x1, y1;
        cell_range(pb, x0, y0, x1, y1);
        int under = -1;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const auto& v = cells[std::size_t(y * n + x)];
                for (auto pos = v.rbegin(); pos != v.rend(); ++pos) {
                    const int j = *pos;
                    if (j >= idx) continue;
                    if (j <= under) break;
                    const ContentItem& q = cm.items[std::size_t(j)];
                    if (q.kind == ContentItem::Kind::LineArt && !(q.paint & 1)) continue;   // a line is no background
                    const Rect in = intersect_rects(painted_box(q), pb);
                    if (in.empty() || in.width() * in.height() < 0.5 * area) continue;
                    under = j;
                    break;
                }
            }
        if (area > 0.5 * page_area) continue;   // a page-sized panel is a background (its text is judged by 1.4.3), no part
        unsigned char bg[3] = {255, 255, 255};
        if (under >= 0) {
            const ContentItem& q = cm.items[std::size_t(under)];
            // the outline of the fill under it: one shape, whose fill is judged as a part of its own
            const double tol = std::max(1.0, double(p.line_width));
            if (q.bbox_valid && std::abs(q.bbox.llx - it.bbox.llx) <= tol && std::abs(q.bbox.lly - it.bbox.lly) <= tol &&
                std::abs(q.bbox.urx - it.bbox.urx) <= tol && std::abs(q.bbox.ury - it.bbox.ury) <= tol)
                continue;
            const bool covers = q.bbox_valid && q.bbox.llx <= pb.llx + 0.5 && q.bbox.lly <= pb.lly + 0.5 && q.bbox.urx >= pb.urx - 0.5 &&
                                q.bbox.ury >= pb.ury - 0.5;
            auto qr = paint_of_.find(key(q));
            if (q.kind != ContentItem::Kind::LineArt || q.paint != 1 || !q.exact_rect || !covers || qr == paint_of_.end()) continue;
            const Paint& qp = paints_[std::size_t(qr->second)];
            if (qp.unknown || qp.blend_multiply || qp.fill_alpha < 1.0f) continue;
            std::copy(qp.fill_rgb, qp.fill_rgb + 3, bg);
        }
        double best = 0;
        unsigned char col[3] = {0, 0, 0};
        auto part = [&](const unsigned char* c, float alpha) {
            const double a = std::clamp(double(alpha), 0.0, 1.0);
            unsigned char m[3];
            for (int k = 0; k < 3; ++k) m[k] = static_cast<unsigned char>(std::lround(a * c[k] + (1 - a) * bg[k]));
            const double r = wcag::contrast8(m, bg);
            if (r > best) best = r, std::copy(m, m + 3, col);
        };
        if (p.fill) part(p.fill_rgb, p.fill_alpha);
        if (p.stroke && p.line_width > 0) part(p.stroke_rgb, p.stroke_alpha);
        if (best < 1.1) continue;   // about the background's colour: nothing visible (a plot area, a mask)
        const ObjRef fr = figure_of(cm, it).value_or(ObjRef{});
        auto g = std::find_if(figs.begin(), figs.end(), [&](const Fig& x) { return x.ref.key() == fr.key(); });
        if (g == figs.end()) {
            g = figs.insert(figs.end(), Fig{});
            g->ref = fr;
        }
        ++g->parts;
        if (best >= 3.0) continue;
        g->box = g->low++ ? Rect{std::min(g->box.llx, pb.llx), std::min(g->box.lly, pb.lly), std::max(g->box.urx, pb.urx), std::max(g->box.ury, pb.ury)}
                          : pb;
        if (best < g->worst) {
            g->worst = best;
            std::copy(col, col + 3, g->fg);
            std::copy(bg, bg + 3, g->bg);
        }
    }
    for (const Fig& g : figs) {
        if (!g.low) continue;
        ctx_.report(rn, page_, g.ref,
                    "WCAG 1.4.11 manual review: " + std::to_string(g.low) + " of " + std::to_string(g.parts) +
                        " graphical parts of this Figure have less than 3:1 against their background (lowest " + wcag::ratio_str(g.worst) +
                        ", " + wcag::hex(g.fg) + " on " + wcag::hex(g.bg) + "), required 3:1 where a part is needed to understand the content; " +
                        wcag::box_str(g.box));
    }
}

// PAC 26.1 text contrast check (NotEnoughContrastForText), all of it: every text piece against the finished page, later paint included (li_*: black text under a
// later black rectangle or image fails, grey text under a later black one passes); artifacts and invisible text are not judged
// (art_lowfg, tr3_lowfg); clipping of the text is ignored (clip_out). A run nothing touches is on white, a run whose last painter is one exact opaque rectangle on that colour; only the
// others need rendering.
void ContrastEngine::flush_page(const ContentModel& cm) {
    if (overflow_) {
        gave_up = true;
        if (page_ >= 0) wcag_incomplete(true);
    }
    if (overflow_ || page_ < 0) return;
    const auto& pages = ctx_.doc.pages();
    if (page_ >= int(pages.size())) return;
    const Page& pg = pages[page_];

    std::vector<int> items;   // this page's semantic items in paint order
    for (std::size_t i = first_item_; i < cm.items.size(); ++i)
        if (cm.streams[cm.items[i].stream].page == page_) items.push_back(int(i));
    // WCAG 1.4.11 (EqualDoX): the painted parts of figures, tagged content inside a Figure element
    std::vector<int> fig_items;
    for (const int idx : items) {
        const ContentItem& it = cm.items[idx];
        if (it.kind == ContentItem::Kind::LineArt && it.paint && it.bbox_valid &&
            !hidden_op(it) && figure_of(cm, it) &&
            !cm.parents_tags_contain(it.stream, it.frame, "Artifact"))
            fig_items.push_back(idx);
    }
    if (pieces_.empty() && fig_items.empty()) return;

    // Same grid search as the content module, over the same painters.
    constexpr int N = 24;
    std::vector<std::vector<int>> cells(N * N);
    const Rect grid = pg.media_box;
    auto cell_range = [&](const Rect& r, int& x0, int& y0, int& x1, int& y1) {
        double w = std::max(1.0, grid.width()), h = std::max(1.0, grid.height());
        auto cx = [&](double x) { return std::clamp(int((x - grid.llx) / w * N), 0, N - 1); };
        auto cy = [&](double y) { return std::clamp(int((y - grid.lly) / h * N), 0, N - 1); };
        x0 = cx(r.llx), x1 = cx(r.urx), y0 = cy(r.lly), y1 = cy(r.ury);
    };
    std::vector<char> needed(cm.items.size() - first_item_, 0);
    int last_needed = -1;
    struct FlatRun {
        int item;
        float bg[3];
        bool artifact = false;   // measured for the WCAG checks only
        bool exact = true;       // the background colour converts without a device profile
        bool later = false;      // the background is painted after the text (which it then hides)
    };
    std::vector<FlatRun> flat_runs;
    // The painted box of an item: a stroke reaches half its width beyond the path (a real-world sample: white
    // headings on 42 pt wide lines).
    auto painted_box = [&](const ContentItem& it) {
        Rect r = it.bbox_valid ? it.bbox : grid;
        if (it.kind == ContentItem::Kind::LineArt && (it.paint & 2)) {
            auto rec = paint_of_.find((std::uint64_t(std::uint32_t(it.stream)) << 32) | it.op_index);
            if (rec != paint_of_.end()) {
                const double hw = paints_[std::size_t(rec->second)].line_width / 2.0;
                r.llx -= hw, r.lly -= hw, r.urx += hw, r.ury += hw;
            }
        }
        return r;
    };
    for (const int idx : items) {
        const ContentItem& it = cm.items[idx];
        const bool painter = (it.kind == ContentItem::Kind::LineArt && it.paint) || it.kind == ContentItem::Kind::InlineImage ||
                             it.kind == ContentItem::Kind::ImageXObject || it.kind == ContentItem::Kind::Shading;
        if (!painter || hidden_op(it)) continue;
        Rect r = painted_box(it);
        int x0, y0, x1, y1;
        cell_range(r, x0, y0, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) cells[std::size_t(y * N + x)].push_back(idx);
    }
    if (!fig_items.empty()) wcag_figures(cm, fig_items, cells, N, cell_range, painted_box);
    if (pieces_.empty()) return;
    for (std::size_t n = 0; n < items.size(); ++n) {
        const int idx = items[n];
        const ContentItem& it = cm.items[idx];
        if (it.kind != ContentItem::Kind::Text || it.text < 0 || !it.bbox_valid) continue;
        const TextRun& tr = cm.texts[it.text];
        if (tr.glyphs == 0 || tr.tr == 3 || tr.tr == 7) continue;
        // PAC does not judge artifacts; the WCAG checks measure them where that needs no rendering
        const bool artifact = cm.parents_tags_contain(it.stream, it.frame, "Artifact");
        if (tr.unmapped == 0 && is_whitespace_only(tr.text)) continue;
        const Color& tc = uses_stroke(tr.tr) ? tr.stroke : tr.fill;
        const bool own_colour = text_rgb_.count((std::uint64_t(std::uint32_t(it.stream)) << 32) | it.op_index) != 0;
        if (!tc.known() && !own_colour) continue;
        int x0, y0, x1, y1;
        cell_range(it.bbox, x0, y0, x1, y1);
        int best = -1;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const auto& v = cells[std::size_t(y * N + x)];
                auto pos = v.end();
                while (pos != v.begin()) {
                    --pos;
                    if (*pos <= best) break;
                    const ContentItem& p = cm.items[*pos];
                    if (painted_box(p).intersects(it.bbox)) {
                        best = *pos;
                        break;
                    }
                }
            }
        const float* bg = nullptr;
        float paint_bg[3];
        static const float white[3] = {1, 1, 1};
        bool exact = true;
        if (best < 0) {
            bg = white;
        } else {
            const ContentItem& p = cm.items[best];
            exact = p.fill.family == ColorFamily::Gray || p.fill.family == ColorFamily::RGB;
            const bool covers = p.bbox_valid && p.bbox.llx <= it.bbox.llx + 0.5 && p.bbox.lly <= it.bbox.lly + 0.5 &&
                                p.bbox.urx >= it.bbox.urx - 0.5 && p.bbox.ury >= it.bbox.ury - 0.5;
            if (p.kind == ContentItem::Kind::LineArt && p.paint == 1 && p.exact_rect && p.fill.known() && p.fill_alpha >= 1.0f && covers) {
                bg = p.fill.rgb;
                // the colour as recorded for the paint (CMYK as PAC renders it)
                auto rec = paint_of_.find((std::uint64_t(std::uint32_t(p.stream)) << 32) | p.op_index);
                if (rec != paint_of_.end()) {
                    const Paint& pp = paints_[std::size_t(rec->second)];
                    for (int k = 0; k < 3; ++k) paint_bg[k] = pp.fill_rgb[k] / 255.0f;
                    bg = paint_bg;
                    // soft mask or blend mode: not a flat colour (probes cb_gs_smask_*)
                    if (pp.unknown || pp.blend_multiply || pp.fill_alpha < 1.0f) bg = nullptr;
                }
            }
        }
        if (artifact) {
            if (bg && best < idx) flat_runs.push_back({idx, {bg[0], bg[1], bg[2]}, true, exact, false});
            continue;
        }
        if (!bg) {
            needed[std::size_t(idx) - first_item_] = 1;
            last_needed = int(n);
        } else {
            flat_runs.push_back({idx, {bg[0], bg[1], bg[2]}, false, exact, best > idx});
        }
    }
    // pieces_ in (stream, operator) order for the item lookup (nested forms interleave with their page)
    auto piece_order = [](const TextPiece& a, const TextPiece& b) { return a.stream != b.stream ? a.stream < b.stream : a.op < b.op; };
    // (nearly always already in order: a stable sort of a sorted range changes nothing, and it allocates a merge buffer)
    if (!std::is_sorted(pieces_.begin(), pieces_.end(), piece_order)) std::stable_sort(pieces_.begin(), pieces_.end(), piece_order);
    // Text on nothing or on one opaque rectangle needs no rendering. Text outside the crop box is not on the page.
    const Rect page_box = pg.crop_box.empty() ? pg.media_box : pg.crop_box;
    for (const FlatRun& f : flat_runs) {
        const ContentItem& it = cm.items[f.item];
        auto range = piece_range(it.stream, it.op_index);
        if (range.second == range.first) continue;
        const TextRun& tr = cm.texts[it.text];
        const Color& tc = uses_stroke(tr.tr) ? tr.stroke : tr.fill;
        float fg[3] = {tc.rgb[0], tc.rgb[1], tc.rgb[2]};
        auto own = text_rgb_.find((std::uint64_t(std::uint32_t(it.stream)) << 32) | it.op_index);
        if (own != text_rgb_.end())
            for (int k = 0; k < 3; ++k) fg[k] = own->second[std::size_t(k)] / 255.0f;
        if (!f.later && it.bbox.intersects(page_box)) wcag_text(cm, it, pieces_[range.first], it.bbox, fg, f.bg, false, f.exact, f.artifact);
        if (f.artifact) continue;
        const bool large = tr.size_user >= 18.0 || (tr.size_user >= 14.0 && bold_font(tr.font));
        const double need = large ? 3.0 : 4.5;
        const double ratio = wcag_contrast(fg, f.bg);
        if (ratio + 1e-9 >= need) continue;
        const StreamInstance& s = cm.streams[it.stream];
        ContrastMeasure m;   // its detail: ContrastMeasure::detail
        m.page = s.page;
        m.obj = s.ref;
        m.ratio = ratio;
        m.size_user = tr.size_user;
        m.need = need;
        m.text = it.text;
        m.large = large;
        for (int k = 0; k < 3; ++k) m.fg[k] = to8(fg[k]), m.bg[k] = to8(f.bg[k]);
        for (std::size_t k = range.first; k < range.second; ++k)
            if (pieces_[k].box.intersects(page_box)) findings.push_back(m);
    }
    if (last_needed < 0) return;

    // Coarse map of the needed text boxes: a paint that touches none is skipped.
    Canvas canvas(page_box);
    constexpr int M = 48;
    std::vector<char> touched(M * M, 0);
    auto touch_range = [&](const Rect& r, int& x0, int& y0, int& x1, int& y1) {
        double w = std::max(1.0, page_box.width()), h = std::max(1.0, page_box.height());
        auto cx = [&](double x) { return std::clamp(int((x - page_box.llx) / w * M), 0, M - 1); };
        auto cy = [&](double y) { return std::clamp(int((y - page_box.lly) / h * M), 0, M - 1); };
        x0 = cx(r.llx), x1 = cx(r.urx), y0 = cy(r.lly), y1 = cy(r.ury);
    };
    for (std::size_t n = 0; n <= std::size_t(last_needed); ++n) {
        if (!needed[std::size_t(items[n]) - first_item_]) continue;
        const Window tw = window_of(canvas, cm.items[items[n]].bbox);
        canvas.want(tw.x0, tw.y0, tw.x1, tw.y1);
        int x0, y0, x1, y1;
        touch_range(cm.items[items[n]].bbox, x0, y0, x1, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) touched[std::size_t(y * M + x)] = 1;
    }
    canvas.allocate();   // the wanted pixels are complete
    for (std::size_t n = 0; n < items.size(); ++n) {
        const ContentItem& it = cm.items[items[n]];
        if (it.kind == ContentItem::Kind::Text) continue;
        auto found = paint_of_.find((std::uint64_t(std::uint32_t(it.stream)) << 32) | it.op_index);
        if (found == paint_of_.end() || hidden_op(it)) continue;
        const Paint& p = paints_[std::size_t(found->second)];
        Rect outer = p.box;
        if (p.stroke) {   // a straight line has an empty path box
            const double pad = std::max(1.0, double(p.line_width));
            outer.llx -= pad, outer.lly -= pad, outer.urx += pad, outer.ury += pad;
        }
        const Rect box = clipped(outer, p.clip);
        if (box.empty()) continue;
        int x0, y0, x1, y1;
        touch_range(box, x0, y0, x1, y1);
        bool hit = false;
        for (int y = y0; y <= y1 && !hit; ++y)
            for (int x = x0; x <= x1 && !hit; ++x) hit = touched[std::size_t(y * M + x)] != 0;
        if (hit) {
            if (!spend(window_of(canvas, box))) {
                gave_up = true;
                wcag_incomplete(false);
                return;
            }
            paint_item(p, canvas);
        }
    }
    for (std::size_t n = 0; n <= std::size_t(last_needed); ++n)
        if (needed[std::size_t(items[n]) - first_item_]) check_text(cm, cm.items[items[n]], canvas);
}

// Mask of a polygon clip chain over the chain's box; nullptr when the chain is rectangles only (the paint window is
// already inside them). Cached: many paints share a clip.
const Mask* ContrastEngine::clip_mask(const Canvas& canvas, int clip) {
    if (clip < 0 || !clips_[std::size_t(clip)].chain_poly) return nullptr;
    auto found = clip_masks_.find(clip);
    if (found != clip_masks_.end()) return found->second.get();
    const ClipNode& n = clips_[std::size_t(clip)];
    const Mask* parent = clip_mask(canvas, n.parent);
    auto m = std::make_shared<Mask>(paint_window(canvas, n.box));
    const Window& w = m->window();
    if (n.has_poly) m->intersect(canvas, pool_, subs_, n.poly, n.evenodd);
    if (parent) m->intersect_mask(*parent);
    const std::size_t bytes = std::size_t(std::max(0, w.w())) * std::size_t(std::max(0, w.h()));
    if (clip_mask_bytes_ + bytes > kMaxClipMaskBytes) {
        clip_masks_.clear();
        clip_mask_bytes_ = 0;
        // `parent` may have been dropped: it is not used below
    }
    clip_mask_bytes_ += bytes;
    page_mask_pixels_ += bytes;
    const Mask* out = m.get();
    clip_masks_[clip] = std::move(m);
    return out;
}

void ContrastEngine::paint_item(const Paint& p, Canvas& canvas) {
    if (p.kind == Paint::Path) paint_path(p, canvas);
    else if (p.kind == Paint::Image) paint_image(p, canvas);
    else paint_shading(p, canvas);
}

namespace {

void blend(Canvas& canvas, int x, int y, const unsigned char* src, double alpha, bool multiply, bool unknown) {
    if (unknown) {
        canvas.unknown(x, y) = 1;
        return;
    }
    unsigned char* dst = canvas.at(x, y);
    if (alpha >= 0.999 && !multiply) {
        dst[0] = src[0], dst[1] = src[1], dst[2] = src[2];
        canvas.unknown(x, y) = 0;
        return;
    }
    const int a = int(alpha * 256 + 0.5);
    for (int k = 0; k < 3; ++k) {
        const int s = multiply ? (src[k] * dst[k] + 127) / 255 : src[k];
        dst[k] = static_cast<unsigned char>((a * s + (256 - a) * dst[k] + 128) >> 8);
    }
    if (alpha >= 0.999) canvas.unknown(x, y) = 0;
}

}  // namespace

void ContrastEngine::paint_path(const Paint& p, Canvas& canvas) {
    Rect box = p.box;
    const double pad = p.stroke ? std::max(1.0, double(p.line_width)) : 0.0;
    box.llx -= pad, box.lly -= pad, box.urx += pad, box.ury += pad;
    box = clipped(box, p.clip);
    if (box.empty()) return;
    Window w = paint_window(canvas, box);
    if (w.empty()) return;
    const Mask* clip = clip_mask(canvas, p.clip);

    if (p.fill) {
        Mask m(w);
        m.intersect(canvas, pool_, subs_, p.path, p.evenodd);
        if (clip) m.intersect_mask(*clip);
        if (p.object_pattern && !p.unknown) {
            Paint s = p;
            s.kind = Paint::Shading;
            s.box = box;
            // through `m`: the shading paints only inside the path
            shading_mask_ = std::make_shared<Mask>(m);
            paint_shading(s, canvas);
            shading_mask_.reset();
        } else if (p.tiling && !p.unknown) {
            paint_tile(p, canvas, w, m);
        } else {
            for (int y = w.y0; y < w.y1; ++y) {
                int xa = w.x0, xb = w.x1;
                if (!canvas.span(y, xa, xb)) continue;
                for (int x = xa; x < xb; ++x)
                    if (m.on(x, y)) blend(canvas, x, y, p.fill_rgb, p.fill_alpha, p.blend_multiply, p.unknown);
            }
        }
    }
    if (p.stroke) {
        std::vector<std::array<Pt, 4>> quads;
        const float hw = std::max(p.line_width / 2, float(0.5 / canvas.scale()) * 0.5f);
        for (std::uint32_t s = p.path.first; s < p.path.first + p.path.count; ++s) {
            const Sub& sub = subs_[s];
            std::uint32_t segs = sub.closed ? sub.count : (sub.count ? sub.count - 1 : 0);
            for (std::uint32_t i = 0; i < segs; ++i) {
                const Pt& a = pool_[sub.first + i];
                const Pt& b = pool_[sub.first + (i + 1) % sub.count];
                float dx = b.x - a.x, dy = b.y - a.y;
                float len = std::hypot(dx, dy);
                if (len <= 0) continue;
                float nx = -dy / len * hw, ny = dx / len * hw;
                quads.push_back({Pt{a.x + nx, a.y + ny}, Pt{b.x + nx, b.y + ny}, Pt{b.x - nx, b.y - ny}, Pt{a.x - nx, a.y - ny}});
            }
        }
        if (!quads.empty()) {
            Mask m(w);
            m.intersect_quads(canvas, quads);
            if (clip) m.intersect_mask(*clip);
            for (int y = w.y0; y < w.y1; ++y) {
                int xa = w.x0, xb = w.x1;
                if (!canvas.span(y, xa, xb)) continue;
                for (int x = xa; x < xb; ++x)
                    if (m.on(x, y)) blend(canvas, x, y, p.stroke_rgb, p.stroke_alpha, p.blend_multiply, p.unknown);
            }
        }
    }
}

const ReducedImage* ContrastEngine::image_of(const Obj& xo) {
    std::uint64_t key = ref_of(xo).key();
    if (inline_pdf_ && xo.getOwningQPDF() == inline_pdf_.get()) key |= std::uint64_t(1) << 63;   // the private inline-image file has its own numbers
    auto it = images_.find(key);
    if (it != images_.end()) {
        auto pos = std::find(image_order_.begin(), image_order_.end(), key);   // used again: last out
        if (pos != image_order_.end()) std::rotate(pos, pos + 1, image_order_.end());
        return it->second.get();
    }
    auto img = std::make_shared<ReducedImage>();
    if (!reduce_image(xo, kMaxImageSide, *img)) img.reset();
    const std::size_t size = image_size(img.get());
    while (!image_order_.empty() && (image_order_.size() >= kMaxImageCache || image_bytes_ + size > kMaxImageCacheBytes)) {
        auto old = images_.find(image_order_.front());
        if (old != images_.end()) {
            image_bytes_ -= image_size(old->second.get());
            images_.erase(old);
        }
        image_order_.erase(image_order_.begin());
    }
    images_[key] = img;
    image_order_.push_back(key);
    image_bytes_ += size;
    return img.get();
}

void ContrastEngine::paint_image(const Paint& p, Canvas& canvas) {
    const Rect box = clipped(p.box, p.clip);
    if (box.empty()) return;
    Window w = paint_window(canvas, box);
    if (w.empty()) return;
    Mask clip(w);
    if (const Mask* cm = clip_mask(canvas, p.clip)) clip.intersect_mask(*cm);
    const ReducedImage* img = (!p.unknown && p.object.isStream()) ? image_of(p.object) : nullptr;
    const Matrix& m = p.ctm;
    const double det = m.a * m.d - m.b * m.c;
    if (!img || det == 0) {
        // Not decodable: what lies under the placed unit square is unknown.
        for (int y = w.y0; y < w.y1; ++y) {
            int xa = w.x0, xb = w.x1;
            if (!canvas.span(y, xa, xb)) continue;
            for (int x = xa; x < xb; ++x) {
                if (!clip.on(x, y)) continue;
                if (det != 0) {
                    double ux = canvas.ux(x + 0.5) - m.e, uy = canvas.uy(y + 0.5) - m.f;
                    double u = (m.d * ux - m.c * uy) / det, v = (-m.b * ux + m.a * uy) / det;
                    if (u < 0 || u > 1 || v < 0 || v > 1) continue;
                }
                canvas.unknown(x, y) = 1;
            }
        }
        return;
    }
    for (int y = w.y0; y < w.y1; ++y) {
        int xa = w.x0, xb = w.x1;
        if (!canvas.span(y, xa, xb)) continue;
        for (int x = xa; x < xb; ++x) {
            if (!clip.on(x, y)) continue;
            double ux = canvas.ux(x + 0.5) - m.e, uy = canvas.uy(y + 0.5) - m.f;
            double u = (m.d * ux - m.c * uy) / det, v = (-m.b * ux + m.a * uy) / det;
            if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
            int ix = std::min(img->w - 1, int(u * img->w)), iy = std::min(img->h - 1, int((1 - v) * img->h));   // u, v in [0, 1)
            std::size_t at = std::size_t(iy) * std::size_t(img->w) + std::size_t(ix);
            double alpha = p.fill_alpha;
            if (!img->alpha.empty()) alpha *= img->alpha[at] / 255.0;
            if (alpha <= 0) continue;
            const unsigned char* src = img->stencil ? p.fill_rgb : &img->rgb[at * 3];
            blend(canvas, x, y, src, alpha, p.blend_multiply, false);
        }
    }
}

std::shared_ptr<TileRaster> ContrastEngine::tile_of(const Paint& p) {
    char key[64];
    std::snprintf(key, sizeof key, "%llu:%d:%d:%d", (unsigned long long)ref_of(p.object).key(), p.fill_rgb[0], p.fill_rgb[1], p.fill_rgb[2]);
    auto it = tiles_.find(key);
    if (it != tiles_.end()) return it->second;
    auto t = std::make_shared<TileRaster>();
    const bool uncoloured = cos::integer(p.object, "PaintType").value_or(1) == 2;
    if (!build_tile(p.object, uncoloured ? p.fill_rgb : nullptr, *t)) t.reset();
    if (tiles_.size() >= 32) tiles_.clear();
    tiles_[key] = t;
    return t;
}

void ContrastEngine::paint_tile(const Paint& p, Canvas& canvas, const Window& w, const Mask& m) {
    auto t = tile_of(p);
    const Matrix& pm = p.ctm;
    const double det = pm.a * pm.d - pm.b * pm.c;
    for (int y = w.y0; y < w.y1; ++y) {
        int xa = w.x0, xb = w.x1;
        if (!canvas.span(y, xa, xb)) continue;
        for (int x = xa; x < xb; ++x) {
            if (!m.on(x, y)) continue;
            if (!t || det == 0) {
                canvas.unknown(x, y) = 1;
                continue;
            }
            const double ux = canvas.ux(x + 0.5) - pm.e, uy = canvas.uy(y + 0.5) - pm.f;
            double u = (pm.d * ux - pm.c * uy) / det - t->x0, v = (-pm.b * ux + pm.a * uy) / det - t->y0;
            u -= std::floor(u / t->xstep) * t->xstep;
            v -= std::floor(v / t->ystep) * t->ystep;
            const int tx = std::min(t->w - 1, int(u / t->xstep * t->w)), ty = std::min(t->h - 1, int(v / t->ystep * t->h));
            const std::size_t at = std::size_t(ty) * std::size_t(t->w) + std::size_t(tx);
            if (t->alpha[at]) blend(canvas, x, y, &t->rgb[at * 3], p.fill_alpha, p.blend_multiply, false);
        }
    }
}

// Type 1: the function over its domain, per canvas pixel.
void ContrastEngine::paint_function_shading(const Paint& p, Canvas& canvas, const Window& w, const Mask& mask, const ColorSpace& cs) {
    auto unknown = [&]() {
        for (int y = w.y0; y < w.y1; ++y) {
            int xa = w.x0, xb = w.x1;
            if (!canvas.span(y, xa, xb)) continue;
            for (int x = xa; x < xb; ++x)
                if (mask.on(x, y)) canvas.unknown(x, y) = 1;
        }
    };
    double dom[4] = {0, 1, 0, 1};
    auto d = cos::array_items(cos::get(p.object, "Domain"));
    if (d.size() >= 4)
        for (int k = 0; k < 4; ++k) dom[k] = cos::number(d[std::size_t(k)]).value_or(dom[k]);
    Matrix sm;
    auto mx = cos::array_items(cos::get(p.object, "Matrix"));
    if (mx.size() == 6)
        sm = Matrix{cos::number(mx[0]).value_or(1), cos::number(mx[1]).value_or(0), cos::number(mx[2]).value_or(0),
                    cos::number(mx[3]).value_or(1), cos::number(mx[4]).value_or(0), cos::number(mx[5]).value_or(0)};
    const Matrix all = sm * p.ctm;
    const double det = all.a * all.d - all.b * all.c;
    Obj fo = cos::get(p.object, "Function");
    std::vector<std::shared_ptr<Function>> fn;
    if (fo.isArray()) {
        for (const Obj& f : cos::array_items(fo)) fn.push_back(Function::make(f));
    } else {
        fn.push_back(Function::make(fo));
    }
    for (auto& f : fn)
        if (!f) return unknown();
    if (det == 0 || fn.empty()) return unknown();
    std::vector<double> out, comps, scratch;
    for (int y = w.y0; y < w.y1; ++y) {
        int xa = w.x0, xb = w.x1;
        if (!canvas.span(y, xa, xb)) continue;
        for (int x = xa; x < xb; ++x) {
            if (!mask.on(x, y)) continue;
            const double ux = canvas.ux(x + 0.5) - all.e, uy = canvas.uy(y + 0.5) - all.f;
            const double in[2] = {(all.d * ux - all.c * uy) / det, (-all.b * ux + all.a * uy) / det};
            if (in[0] < dom[0] || in[0] > dom[1] || in[1] < dom[2] || in[1] > dom[3]) continue;
            comps.clear();
            bool ok = true;
            if (fn.size() == 1) ok = fn[0]->eval(in, 2, comps);
            else
                for (auto& f : fn) {
                    ok = ok && f->eval(in, 2, out) && !out.empty();
                    if (ok) comps.push_back(out[0]);
                }
            if (!ok || int(comps.size()) < cs.ncomp) {
                canvas.unknown(x, y) = 1;
                continue;
            }
            double rgb[3];
            cs.to_rgb(comps.data(), rgb, scratch);
            const unsigned char px[3] = {to8(rgb[0]), to8(rgb[1]), to8(rgb[2])};
            blend(canvas, x, y, px, p.fill_alpha, p.blend_multiply, false);
        }
    }
}

// Types 4-7: triangles with colours interpolated per vertex; a patch becomes a grid of triangles over its Coons boundary (the inner points of type 7 are ignored).
void ContrastEngine::paint_mesh(const Paint& p, Canvas& canvas, const Window& w, const Mask& mask, const ColorSpace& cs, std::int64_t type) {
    auto unknown = [&]() {
        for (int y = w.y0; y < w.y1; ++y) {
            int xa = w.x0, xb = w.x1;
            if (!canvas.span(y, xa, xb)) continue;
            for (int x = xa; x < xb; ++x)
                if (mask.on(x, y)) canvas.unknown(x, y) = 1;
        }
    };
    Obj sh = p.object;
    if (!sh.isStream()) return unknown();
    const int bco = int(cos::integer(sh, "BitsPerCoordinate").value_or(0)), bcm = int(cos::integer(sh, "BitsPerComponent").value_or(0));
    const int bfl = type == 5 ? 0 : int(cos::integer(sh, "BitsPerFlag").value_or(0));
    std::vector<double> dec;
    for (const Obj& v : cos::array_items(cos::get(sh, "Decode"))) dec.push_back(cos::number(v).value_or(0));
    Obj fo = cos::get(sh, "Function");
    std::vector<std::shared_ptr<Function>> fn;
    if (fo.isArray())
        for (const Obj& f : cos::array_items(fo)) fn.push_back(Function::make(f));
    else if (!fo.isNull()) fn.push_back(Function::make(fo));
    for (auto& f : fn)
        if (!f) return unknown();
    const int ncomp = fn.empty() ? cs.ncomp : 1;
    if (bco < 1 || bco > 32 || bcm < 1 || bcm > 16 || dec.size() < std::size_t(4 + 2 * ncomp)) return unknown();
    std::shared_ptr<Buffer> buf;
    try {
        buf = const_cast<Obj&>(sh).getStreamData(qpdf_dl_generalized);
    } catch (std::exception&) {
        return unknown();
    }
    if (buf->getSize() > 32u << 20) return unknown();
    const unsigned char* d = buf->getBuffer();
    const std::size_t total = buf->getSize() * 8;
    std::size_t bit = 0;
    auto read = [&](int n) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i, ++bit) {
            v <<= 1;
            if (bit < total) v |= (d[bit >> 3] >> (7 - (bit & 7))) & 1;
        }
        return v;
    };
    struct V { double x, y; float c[3]; };
    std::vector<double> scratch, out, comps(std::size_t(std::max(1, ncomp)));
    auto read_pt = [&](double& x, double& y) {
        const double mx = bco == 32 ? 4294967295.0 : double((1ull << bco) - 1);
        const double a = read(bco) / mx, b = read(bco) / mx;
        double ux = dec[0] + a * (dec[1] - dec[0]), uy = dec[2] + b * (dec[3] - dec[2]);
        p.ctm.apply(ux, uy, x, y);
    };
    bool colour_ok = true;
    auto read_col = [&](float* c) {
        const double mx = double((1u << bcm) - 1);
        for (int k = 0; k < ncomp; ++k) comps[std::size_t(k)] = dec[std::size_t(4 + 2 * k)] + read(bcm) / mx * (dec[std::size_t(5 + 2 * k)] - dec[std::size_t(4 + 2 * k)]);
        std::vector<double> in = comps;
        if (!fn.empty()) {
            in.clear();
            if (fn.size() == 1) colour_ok = fn[0]->eval(comps.data(), 1, in) && colour_ok;
            else
                for (auto& f : fn) {
                    colour_ok = f->eval(comps.data(), 1, out) && !out.empty() && colour_ok;
                    in.push_back(out.empty() ? 0 : out[0]);
                }
            if (int(in.size()) < cs.ncomp) {
                colour_ok = false;
                in.resize(std::size_t(cs.ncomp), 0);
            }
        }
        double rgb[3];
        cs.to_rgb(in.data(), rgb, scratch);
        for (int k = 0; k < 3; ++k) c[k] = float(clamp01(rgb[k]) * 255);
    };
    std::int64_t tris = 0;
    bool stop = false;
    auto tri = [&](const V& a, const V& b, const V& c) {
        if (++tris > 400000 || stop) {
            stop = true;
            return;
        }
        const Rect r{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}), std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y})};
        Window tw = window_of(canvas, r);
        if (tw.empty()) return;
        const double den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        if (den == 0) return;
        for (int y = std::max(tw.y0, w.y0); y < std::min(tw.y1, w.y1); ++y) {
            int xa = std::max(tw.x0, w.x0), xb = std::min(tw.x1, w.x1);
            if (!canvas.span(y, xa, xb)) continue;
            for (int x = xa; x < xb; ++x) {
                if (!mask.on(x, y)) continue;
                const double ux = canvas.ux(x + 0.5), uy = canvas.uy(y + 0.5);
                const double l1 = ((b.y - c.y) * (ux - c.x) + (c.x - b.x) * (uy - c.y)) / den;
                const double l2 = ((c.y - a.y) * (ux - c.x) + (a.x - c.x) * (uy - c.y)) / den;
                const double l3 = 1 - l1 - l2;
                if (l1 < -1e-6 || l2 < -1e-6 || l3 < -1e-6) continue;
                unsigned char px[3];
                for (int k = 0; k < 3; ++k) px[k] = static_cast<unsigned char>(std::clamp(l1 * a.c[k] + l2 * b.c[k] + l3 * c.c[k], 0.0, 255.0));
                blend(canvas, x, y, px, p.fill_alpha, p.blend_multiply, false);
            }
        }
    };
    const auto align = [&]() { bit = (bit + 7) & ~std::size_t(7); };
    if (type == 4) {
        V v[3] = {};
        int have = 0;
        while (bit + std::size_t(bfl + 2 * bco + ncomp * bcm) <= total && !stop) {
            const std::uint32_t f = read(bfl);
            V n{};
            read_pt(n.x, n.y);
            read_col(n.c);
            align();
            if (f == 0 || have < 3) {
                if (f == 0 && have >= 3) have = 0;
                v[have++] = n;
                if (have == 3) tri(v[0], v[1], v[2]);
            } else {
                if (f == 1) v[0] = v[1];
                v[1] = v[2];
                v[2] = n;
                tri(v[0], v[1], v[2]);
            }
        }
    } else if (type == 5) {
        const int per = int(cos::integer(sh, "VerticesPerRow").value_or(0));
        if (per < 2 || per > 1 << 16) return unknown();
        std::vector<V> prev, cur;
        while (bit + std::size_t(2 * bco + ncomp * bcm) <= total && !stop) {
            cur.clear();
            for (int i = 0; i < per && bit + std::size_t(2 * bco + ncomp * bcm) <= total; ++i) {
                V n{};
                read_pt(n.x, n.y);
                read_col(n.c);
                align();
                cur.push_back(n);
            }
            if (int(cur.size()) < per) break;
            if (!prev.empty())
                for (int i = 0; i + 1 < per; ++i) {
                    tri(prev[std::size_t(i)], prev[std::size_t(i + 1)], cur[std::size_t(i)]);
                    tri(prev[std::size_t(i + 1)], cur[std::size_t(i + 1)], cur[std::size_t(i)]);
                }
            prev = cur;
        }
    } else {
        const int total_pts = type == 6 ? 12 : 16;
        double px[16] = {}, py[16] = {};
        float pc[4][3] = {};
        constexpr int G = 8;
        while (bit + std::size_t(bfl) < total && !stop) {
            const std::uint32_t f = read(bfl) & 3;
            int first_pt = 0, first_col = 0;
            if (f != 0) {
                double nx[4], ny[4];
                float c0[3], c1[3];
                static const int edge[4][4] = {{0, 0, 0, 0}, {3, 4, 5, 6}, {6, 7, 8, 9}, {9, 10, 11, 0}};
                for (int k = 0; k < 4; ++k) nx[k] = px[edge[f][k]], ny[k] = py[edge[f][k]];
                std::copy(pc[f], pc[f] + 3, c0);
                std::copy(pc[(f + 1) & 3], pc[(f + 1) & 3] + 3, c1);
                for (int k = 0; k < 4; ++k) px[k] = nx[k], py[k] = ny[k];
                std::copy(c0, c0 + 3, pc[0]);
                std::copy(c1, c1 + 3, pc[1]);
                first_pt = 4;
                first_col = 2;
            }
            if (bit + std::size_t((total_pts - first_pt) * 2 * bco) > total) break;
            for (int k = first_pt; k < total_pts; ++k) read_pt(px[k], py[k]);
            for (int k = first_col; k < 4; ++k) read_col(pc[k]);
            align();
            // Boundaries: p0..p3 (u = 0), p3..p6 (v = 1), p6..p9 reversed (u = 1), p9..p0 reversed (v = 0)
            auto bez = [&](const double* a, int i0, int i1, int i2, int i3, double s) {
                const double t = 1 - s;
                return t * t * t * a[i0] + 3 * s * t * t * a[i1] + 3 * s * s * t * a[i2] + s * s * s * a[i3];
            };
            auto surf = [&](const double* a, double u, double v) {
                const double d1 = bez(a, 0, 11, 10, 9, u), d2 = bez(a, 3, 4, 5, 6, u);
                const double c1 = bez(a, 0, 1, 2, 3, v), c2 = bez(a, 9, 8, 7, 6, v);
                const double sc = (1 - v) * d1 + v * d2, sd = (1 - u) * c1 + u * c2;
                const double sb = (1 - v) * ((1 - u) * a[0] + u * a[9]) + v * ((1 - u) * a[3] + u * a[6]);
                return sc + sd - sb;
            };
            V grid[G + 1][G + 1];
            for (int j = 0; j <= G; ++j)
                for (int i = 0; i <= G; ++i) {
                    const double u = double(i) / G, v = double(j) / G;
                    V& n = grid[j][i];
                    n.x = surf(px, u, v);
                    n.y = surf(py, u, v);
                    for (int k = 0; k < 3; ++k)
                        n.c[k] = float((1 - u) * (1 - v) * pc[0][k] + (1 - u) * v * pc[1][k] + u * v * pc[2][k] + u * (1 - v) * pc[3][k]);
                }
            for (int j = 0; j < G; ++j)
                for (int i = 0; i < G; ++i) {
                    tri(grid[j][i], grid[j][i + 1], grid[j + 1][i]);
                    tri(grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]);
                }
        }
    }
    if (stop || !colour_ok) unknown();
}

void ContrastEngine::paint_shading(const Paint& p, Canvas& canvas) {
    const Rect box = clipped(p.box, p.clip);
    if (box.empty()) return;
    Window w = paint_window(canvas, box);
    if (w.empty()) return;
    const Mask* through = shading_mask_.get();
    Mask clip(w);
    if (!through)
        if (const Mask* cm = clip_mask(canvas, p.clip)) clip.intersect_mask(*cm);
    const Mask& mask = through ? *through : clip;

    auto paint_unknown = [&]() {
        for (int y = w.y0; y < w.y1; ++y) {
            int xa = w.x0, xb = w.x1;
            if (!canvas.span(y, xa, xb)) continue;
            for (int x = xa; x < xb; ++x)
                if (mask.on(x, y)) canvas.unknown(x, y) = 1;
        }
    };
    Obj sh = p.object;
    if (p.unknown || !(sh.isDictionary() || sh.isStream())) return paint_unknown();
    const auto type = cos::integer(sh, "ShadingType").value_or(0);
    auto cs = ColorSpace::make(cos::get(sh, "ColorSpace"), {});
    if (!cs) return paint_unknown();
    if (type >= 4 && type <= 7) return paint_mesh(p, canvas, w, mask, *cs, type);
    if (type == 1) return paint_function_shading(p, canvas, w, mask, *cs);
    if (type != 2 && type != 3) return paint_unknown();
    auto coords = cos::array_items(cos::get(sh, "Coords"));
    if (coords.size() != (type == 2 ? 4u : 6u)) return paint_unknown();
    double co[6] = {0, 0, 0, 0, 0, 0};
    for (std::size_t k = 0; k < coords.size(); ++k) co[k] = cos::number(coords[k]).value_or(0);
    double t0 = 0, t1 = 1;
    if (auto dom = cos::array_items(cos::get(sh, "Domain")); dom.size() >= 2)
        t0 = cos::number(dom[0]).value_or(0), t1 = cos::number(dom[1]).value_or(1);
    bool ext0 = false, ext1 = false;
    if (auto ex = cos::array_items(cos::get(sh, "Extend")); ex.size() >= 2) {
        ext0 = ex[0].isBool() && ex[0].getBoolValue();
        ext1 = ex[1].isBool() && ex[1].getBoolValue();
    }
    // /Function: one function, or one per component
    std::vector<std::shared_ptr<Function>> fn;
    Obj fo = cos::get(sh, "Function");
    if (fo.isArray()) {
        for (const Obj& f : cos::array_items(fo)) {
            auto x = Function::make(f);
            if (!x) return paint_unknown();
            fn.push_back(std::move(x));
        }
    } else {
        auto x = Function::make(fo);
        if (!x) return paint_unknown();
        fn.push_back(std::move(x));
    }
    // A lookup table over the parameter makes per-pixel evaluation cheap.
    constexpr int LUT = 256;
    std::vector<unsigned char> lut(3 * (LUT + 1));
    std::vector<double> out, scratch, comps;
    for (int i = 0; i <= LUT; ++i) {
        double t = t0 + (t1 - t0) * i / LUT;
        comps.clear();
        if (fn.size() == 1) {
            if (!fn[0]->eval(&t, 1, comps)) return paint_unknown();
        } else {
            for (auto& f : fn) {
                if (!f->eval(&t, 1, out) || out.empty()) return paint_unknown();
                comps.push_back(out[0]);
            }
        }
        if (int(comps.size()) < cs->ncomp) return paint_unknown();
        double rgb[3];
        cs->to_rgb(comps.data(), rgb, scratch);
        lut[std::size_t(i) * 3] = to8(rgb[0]);
        lut[std::size_t(i) * 3 + 1] = to8(rgb[1]);
        lut[std::size_t(i) * 3 + 2] = to8(rgb[2]);
    }
    const Matrix& m = p.ctm;
    const double det = m.a * m.d - m.b * m.c;
    if (det == 0) return paint_unknown();
    for (int y = w.y0; y < w.y1; ++y) {
        int xa = w.x0, xb = w.x1;
        if (!canvas.span(y, xa, xb)) continue;
        for (int x = xa; x < xb; ++x) {
            if (!mask.on(x, y)) continue;
            double ux = canvas.ux(x + 0.5) - m.e, uy = canvas.uy(y + 0.5) - m.f;
            double sx = (m.d * ux - m.c * uy) / det, sy = (-m.b * ux + m.a * uy) / det;
            double s;
            bool paint = true;
            if (type == 2) {
                double dx = co[2] - co[0], dy = co[3] - co[1], den = dx * dx + dy * dy;
                s = den > 0 ? ((sx - co[0]) * dx + (sy - co[1]) * dy) / den : 0;
                if (s < 0) paint = ext0, s = 0;
                else if (s > 1) paint = ext1, s = 1;
            } else {
                // Largest parameter with a non-negative radius on the circle family through the point.
                double cdx = co[3] - co[0], cdy = co[4] - co[1], dr = co[5] - co[2];
                double px = sx - co[0], py = sy - co[1];
                double a = cdx * cdx + cdy * cdy - dr * dr;
                double b = px * cdx + py * cdy + co[2] * dr;
                double c = px * px + py * py - co[2] * co[2];
                paint = false;
                s = 0;
                double cand[2];
                int nc = 0;
                if (std::abs(a) < 1e-9) {
                    if (b != 0) cand[nc++] = c / (2 * b);
                } else {
                    double disc = b * b - a * c;
                    if (disc >= 0) {
                        double q = std::sqrt(disc);
                        cand[nc++] = (b + q) / a;
                        cand[nc++] = (b - q) / a;
                        if (cand[0] < cand[1]) std::swap(cand[0], cand[1]);
                    }
                }
                for (int k = 0; k < nc && !paint; ++k) {
                    double v = cand[k];
                    if (co[2] + v * dr < 0) continue;
                    if (v >= 0 && v <= 1) s = v, paint = true;
                    else if (v < 0 && ext0) s = 0, paint = true;
                    else if (v > 1 && ext1) s = 1, paint = true;
                }
            }
            if (!paint) continue;
            int li = std::clamp(to_int(s * LUT + 0.5), 0, LUT);
            blend(canvas, x, y, &lut[std::size_t(li) * 3], p.fill_alpha, p.blend_multiply, false);
        }
    }
}

// Each piece of the run, each non-blank glyph box: its background is the box's prevailing colour; the worst glyph decides.
void ContrastEngine::check_text(const ContentModel& cm, const ContentItem& it, const Canvas& canvas) {
    const TextRun& tr = cm.texts[it.text];
    const Color& tc = uses_stroke(tr.tr) ? tr.stroke : tr.fill;
    auto own = text_rgb_.find((std::uint64_t(std::uint32_t(it.stream)) << 32) | it.op_index);
    if (!tc.known() && own == text_rgb_.end()) return;
    const bool use_own = own != text_rgb_.end();
    const bool large = tr.size_user >= 18.0 || (tr.size_user >= 14.0 && bold_font(tr.font));
    const double need = large ? 3.0 : 4.5;
    // PAC ignores the transparency of the text itself (probe alpha_text: black at ca 0.35 passes).
    const double a = 1.0;
    unsigned char fg[3];
    if (use_own) std::copy(own->second.begin(), own->second.end(), fg);
    else fg[0] = to8(tc.rgb[0]), fg[1] = to8(tc.rgb[1]), fg[2] = to8(tc.rgb[2]);

    // Worst glyph of a box list.
    auto judge = [&](const Rect& box, double& worst, unsigned char* worst_bg, unsigned char* worst_fg) {
        Window w = window_of(canvas, box);
        if (w.empty()) return;
        unsigned char bg[3];
        if (!background(canvas, w, bg)) return;
        unsigned char f[3];
        for (int k = 0; k < 3; ++k) f[k] = static_cast<unsigned char>(std::lround(a * fg[k] + (1 - a) * bg[k]));
        double la = luminance8(f), lb = luminance8(bg);
        if (la < lb) std::swap(la, lb);
        const double ratio = (la + 0.05) / (lb + 0.05);
        if (ratio < worst) {
            worst = ratio;
            std::copy(bg, bg + 3, worst_bg);
            std::copy(f, f + 3, worst_fg);
        }
    };
    auto range = piece_range(it.stream, it.op_index);
    for (std::size_t k = range.first; k < range.second; ++k) {
        const TextPiece& piece = pieces_[k];
        if (window_of(canvas, piece.box).empty()) continue;   // outside the crop box
        double worst = 1e9;
        unsigned char worst_bg[3] = {255, 255, 255}, worst_fg[3] = {fg[0], fg[1], fg[2]};
        for (std::uint32_t g = 0; g < piece.glyphs; ++g) {
            const float* b = &glyph_boxes_[std::size_t(piece.first_glyph + g) * 4];
            judge(Rect{b[0], b[1], b[2], b[3]}, worst, worst_bg, worst_fg);
        }
        if (worst < 1e8) {   // the worst glyph's background, for the WCAG checks (always for review: the background was rendered)
            const float wf[3] = {fg[0] / 255.0f, fg[1] / 255.0f, fg[2] / 255.0f};
            const float wb[3] = {worst_bg[0] / 255.0f, worst_bg[1] / 255.0f, worst_bg[2] / 255.0f};
            wcag_text(cm, it, piece, piece.box, wf, wb, true, true, false);
        }
        if (worst + 1e-9 >= need) continue;
        const StreamInstance& s = cm.streams[it.stream];
        ContrastMeasure m;   // its detail: ContrastMeasure::detail
        m.page = s.page;
        m.obj = s.ref;
        m.ratio = worst;
        m.size_user = tr.size_user;
        m.need = need;
        m.text = it.text;
        m.rendered = true;
        m.large = large;
        std::copy(worst_fg, worst_fg + 3, m.fg);
        std::copy(worst_bg, worst_bg + 3, m.bg);
        findings.push_back(m);
    }
}

// The prevailing colour of a pixel window; false when at least half of it is unknown. A glyph box takes the colour most of
// it shows: PAC fails black text only when a dark area covers most of a glyph box (sh_8, sw_4, m_black_60), not for thin rules
// or small spots under it (sh_1..sh_4, sw_1..sw_3, sb_below*, m_black_50), and the same strips with grey text agree
// (m_gray_*). Means do not fit (sh_8).
bool ContrastEngine::background(const Canvas& canvas, const Window& w, unsigned char* out) const {
    int total = 0, unknown = 0;
    int bins[64] = {0};
    long bin_sum[64][3] = {};
    for (int y = w.y0; y < w.y1; ++y)
        for (int x = w.x0; x < w.x1; ++x) {
            ++total;
            if (canvas.unknown(x, y)) {
                ++unknown;
                continue;
            }
            const unsigned char* p = canvas.at(x, y);
            const int b = (p[0] >> 6) << 4 | (p[1] >> 6) << 2 | (p[2] >> 6);
            ++bins[b];
            for (int k = 0; k < 3; ++k) bin_sum[b][k] += p[k];
        }
    if (total == 0 || unknown * 2 >= total) return false;
    int best = 0;
    for (int b = 1; b < 64; ++b)
        if (bins[b] > bins[best]) best = b;
    for (int k = 0; k < 3; ++k) out[k] = static_cast<unsigned char>((bin_sum[best][k] + bins[best] / 2) / bins[best]);
    return true;
}

std::pair<std::size_t, std::size_t> ContrastEngine::piece_range(int stream, std::uint32_t op) const {
    const std::pair<int, std::uint32_t> key(stream, op);
    const auto below = [](const TextPiece& p, const std::pair<int, std::uint32_t>& k) { return std::make_pair(p.stream, p.op) < k; };
    // The lower bound of the key in the sorted pieces: the position with every piece before it below the key and the
    // piece at it not below. The runs ask in paint order, so that is mostly where the range before ended: tried first.
    std::size_t lo = piece_hint_;
    if (lo > pieces_.size() || (lo > 0 && !below(pieces_[lo - 1], key)) || (lo < pieces_.size() && below(pieces_[lo], key)))
        lo = std::size_t(std::lower_bound(pieces_.begin(), pieces_.end(), key, below) - pieces_.begin());
    std::size_t hi = lo;
    while (hi < pieces_.size() && pieces_[hi].stream == stream && pieces_[hi].op == op) ++hi;
    piece_hint_ = hi;
    return {lo, hi};
}

std::unique_ptr<ContentObserver> make_contrast_observer(ValidationContext& ctx) { return std::make_unique<ContrastEngine>(ctx); }

const std::vector<ContrastMeasure>& contrast_findings(const ContentObserver& observer) {
    return static_cast<const ContrastEngine&>(observer).findings;
}

std::string ContrastMeasure::detail(const ContentModel& cm) const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "contrast %.2f:1 (text %s on %s%s, %.1f pt%s), needs %.1f:1", ratio, hex8(fg).c_str(), rendered ? "rendered " : "",
                  hex8(bg).c_str(), size_user, large ? ", large" : "", need);
    std::string run = to_utf8(cm.texts[std::size_t(text)].text);
    if (run.size() > 40) run = run.substr(0, 40) + "...";
    return std::string(buf) + (run.empty() ? "" : ": \"" + run + "\"");
}

bool contrast_gave_up(const ContentObserver& observer) { return static_cast<const ContrastEngine&>(observer).gave_up; }
const std::vector<ContrastFinding>& missing_resources(const ContentObserver& observer) { return static_cast<const ContrastEngine&>(observer).missing_res; }

const std::vector<ArtifactTextPiece>& artifact_text_pieces(const ContentObserver& observer) {
    return static_cast<const ContrastEngine&>(observer).artifact_pieces;
}

}  // namespace eqx::pacc
