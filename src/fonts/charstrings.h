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
// Width extraction from Type 1 / Type 2 charstrings (org.verapdf.pd.font.type1.BaseCharStringParser,
// Type1CharStringParser, cff.Type2CharStringParser). Only the advance width is computed, by the same
// operator walk as veraPDF (stops at the first operator that can carry the width).
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eqx::fontimpl {

struct CffNumber {
    std::int64_t integer = 0;
    float real = 0;
    bool is_integer = true;
    static CffNumber of_int(std::int32_t v) { return {v, static_cast<float>(v), true}; }
    static CffNumber of_real(float v);
};

// CFF INDEX (cff.CFFIndex)
struct CffIndex {
    int count = 0;
    int offset_shift = 0;
    std::vector<std::int32_t> offsets;
    std::string_view data;   // view into the program bytes
    // CFFIndex.get: false where Java throws ArrayIndexOutOfBoundsException (or IllegalArgumentException)
    bool get(int n, std::string& out) const;
};

// Type1CharStringParser(stream, subrWidths): nullopt = no width found.
std::optional<CffNumber> type1_charstring_width(std::string_view cs,
                                                const std::unordered_map<int, CffNumber>* subr_widths);
// Type2CharStringParser(stream, localSubrs, bias, globalSubrs, gBias); throws ps::Error where Java throws
// (index access with a negative subroutine number).
std::optional<CffNumber> type2_charstring_width(std::string_view cs, const CffIndex* local, int bias,
                                                const CffIndex* global, int gbias);

}  // namespace eqx::fontimpl
