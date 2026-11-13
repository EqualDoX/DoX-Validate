// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
// distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/ (also: LICENSES/MPL-2.0.txt).
//
// This file is a C++ translation of parts of veraPDF 1.30.2 (veraPDF Parser and veraPDF Validation,
// https://github.com/veraPDF), Copyright (c) 2015-2026, veraPDF Consortium <info@verapdf.org>. veraPDF is offered
// under GPL-3.0-or-later or MPL-2.0-or-later; this file is used and distributed under MPL-2.0.
// Translation and modifications: Copyright (c) 2026 EqualDoX (https://equaldox.de).
// The EqualDoX Attribution License does not apply to this file (THIRD_PARTY_NOTICES.md).
//
#include "fonts/charstrings.h"

#include <string>

#include "fonts/ps.h"
#include "fonts/util.h"

namespace eqx::fontimpl {

CffNumber CffNumber::of_real(float v) { return {java_d2l(static_cast<double>(v)), v, false}; }

bool CffIndex::get(int n, std::string& out) const {
    if (n < 0 || n >= count) return false;
    std::int32_t a = offsets[static_cast<std::size_t>(n)], b = offsets[static_cast<std::size_t>(n) + 1];
    if (a <= 0 || b <= 0) return false;
    std::int64_t from = a - 1, to = b - 1;
    if (from > to) return false;                                        // IllegalArgumentException
    if (from > static_cast<std::int64_t>(data.size())) return false;     // ArrayIndexOutOfBoundsException
    out.assign(static_cast<std::size_t>(to - from), '\0');               // Arrays.copyOfRange pads with zeros
    std::int64_t avail = std::min<std::int64_t>(to, static_cast<std::int64_t>(data.size())) - from;
    for (std::int64_t k = 0; k < avail; ++k) out[static_cast<std::size_t>(k)] = data[static_cast<std::size_t>(from + k)];
    return true;
}

namespace {

class Walker {
public:
    explicit Walker(std::string_view cs) { streams_.push_back({std::string(cs), 0}); }

    // readStreams(buffer, size): bytes read (partial), -1 when nothing could be read
    int read(unsigned char* buf, int size) {
        if (streams_.empty()) return -1;
        int idx = 0, left = size;
        while (left != 0 && !streams_.empty()) {
            auto& s = streams_.back();
            if (s.pos >= s.data.size()) {
                streams_.pop_back();
                continue;
            }
            std::size_t k = std::min<std::size_t>(static_cast<std::size_t>(left), s.data.size() - s.pos);
            for (std::size_t i = 0; i < k; ++i) buf[idx++] = static_cast<unsigned char>(s.data[s.pos + i]);
            s.pos += k;
            left -= static_cast<int>(k);
        }
        int r = size - left;
        return r == 0 ? -1 : r;
    }
    void push_stream(std::string s) {
        if (streams_.size() > 64) throw ps::Error("subroutines nested too deeply");
        streams_.push_back({std::move(s), 0});
    }

    std::vector<CffNumber> stack;
    std::optional<CffNumber> width;

    void pop_n(int n) {
        for (int i = 0; i < n && !stack.empty(); ++i) stack.pop_back();
    }
    CffNumber next_integer(int b, bool type2) {
        unsigned char buf[4] = {0, 0, 0, 0};
        if (b > 31 && b < 247) return CffNumber::of_int(b - 139);
        if (b > 246 && b < 251) {
            read(buf, 1);
            return CffNumber::of_int(((b - 247) << 8) + buf[0] + 108);
        }
        if (b > 250 && b < 255) {
            read(buf, 1);
            return CffNumber::of_int(-((b - 251) << 8) - buf[0] - 108);
        }
        return next_number(b, type2);
    }
    CffNumber next_number(int b, bool type2) {
        unsigned char buf[4] = {0, 0, 0, 0};
        if (type2 && b == 28) {
            read(buf, 2);
            return CffNumber::of_int(static_cast<std::int16_t>((buf[0] << 8) | buf[1]));
        }
        read(buf, 4);
        std::int32_t v = static_cast<std::int32_t>((static_cast<std::uint32_t>(buf[0]) << 24) | (buf[1] << 16) |
                                                   (buf[2] << 8) | buf[3]);
        if (type2) return CffNumber::of_real(static_cast<float>(v) / 65536.0f);
        return CffNumber::of_int(v);
    }

private:
    struct S {
        std::string data;
        std::size_t pos;
    };
    std::vector<S> streams_;
};

}  // namespace

std::optional<CffNumber> type1_charstring_width(std::string_view cs,
                                                const std::unordered_map<int, CffNumber>* subr_widths) {
    Walker w(cs);
    unsigned char b;
    int cont = w.read(&b, 1);
    while (cont != -1) {
        int x = b;
        if (x > 31) {
            w.stack.push_back(w.next_integer(x, false));
        } else if (x != 12) {
            bool stop = false;
            switch (x) {
                case 14: case 9: case 11: break;
                case 6: case 22: case 7: case 4: w.pop_n(1); break;
                case 5: case 21: case 1: case 3: w.pop_n(2); break;
                case 31: case 30: w.pop_n(4); break;
                case 8: w.pop_n(6); break;
                case 13:
                    if (!w.stack.empty()) {
                        w.width = w.stack.back();
                        w.stack.pop_back();
                        w.pop_n(1);
                    }
                    stop = true;
                    break;
                case 10:
                    if (!w.stack.empty()) {
                        CffNumber n = w.stack.back();
                        w.stack.pop_back();
                        if (subr_widths) {
                            auto it = subr_widths->find(static_cast<std::int32_t>(n.integer));
                            if (it != subr_widths->end()) {
                                w.width = it->second;
                                stop = true;
                            }
                        }
                    }
                    break;
                default: break;
            }
            if (stop) break;
        } else {
            unsigned char e = 0;
            w.read(&e, 1);
            bool stop = false;
            switch (e) {
                case 0: break;
                case 33: w.pop_n(2); break;
                case 6: w.pop_n(5); break;
                case 2: case 1: w.pop_n(6); break;
                case 16: break;
                case 17: w.pop_n(1); break;
                case 7:
                    w.pop_n(1);
                    if (!w.stack.empty()) {
                        w.width = w.stack.back();
                        w.stack.pop_back();
                        w.pop_n(2);
                    }
                    stop = true;
                    break;
                case 12:
                    if (w.stack.size() > 1) {
                        double n2 = w.stack.back().real;
                        w.stack.pop_back();
                        double n1 = w.stack.back().real;
                        w.stack.pop_back();
                        w.stack.push_back(CffNumber::of_real(static_cast<float>(n1 / n2)));
                    }
                    break;
                default: break;
            }
            if (stop) break;
        }
        cont = w.read(&b, 1);
    }
    return w.width;
}

std::optional<CffNumber> type2_charstring_width(std::string_view cs, const CffIndex* local, int bias,
                                                const CffIndex* global, int gbias) {
    Walker w(cs);
    auto exec_subr = [&](const CffIndex* subrs, int sb) {
        if (w.stack.empty()) return;
        std::int32_t num = static_cast<std::int32_t>(w.stack.back().integer);
        w.stack.pop_back();
        int count = subrs ? subrs->count : 0;
        if (count > std::max(num + sb, 0)) {
            std::string s;
            if (!subrs->get(num + sb, s)) throw ps::Error("subroutine index out of bounds");
            w.push_stream(std::move(s));
        }
    };
    unsigned char b;
    int cont = w.read(&b, 1);
    while (cont != -1) {
        int x = b;
        if (x > 31) {
            w.stack.push_back(w.next_integer(x, true));
        } else {
            bool stop = true;
            switch (x) {
                case 19: case 20:
                    if (w.stack.size() % 2 == 1) w.width = w.stack.front();
                    break;
                case 14:
                    if (!w.stack.empty() && w.stack.size() != 4) w.width = w.stack.front();
                    break;
                case 4: case 22:
                    if (w.stack.size() > 1) w.width = w.stack.front();
                    break;
                case 21:
                    if (w.stack.size() > 2) w.width = w.stack.front();
                    break;
                case 1: case 3: case 18: case 23:
                    if (w.stack.size() % 2 == 1) w.width = w.stack.front();
                    break;
                case 28:
                    w.stack.push_back(w.next_number(28, true));
                    stop = false;
                    break;
                case 10:
                    exec_subr(local, bias);
                    stop = false;
                    break;
                case 29:
                    exec_subr(global, gbias);
                    stop = false;
                    break;
                case 11: stop = false; break;
                default: break;
            }
            if (stop) break;
        }
        cont = w.read(&b, 1);
    }
    return w.width;
}

}  // namespace eqx::fontimpl
