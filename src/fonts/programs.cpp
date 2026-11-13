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
// opentype.OpenTypeFontProgram: delegates to the TrueType / CFF / CIDFontType2 program it wraps.
#include "fonts/programs.h"

namespace eqx::fontimpl {

namespace {

class OpenTypeProgram final : public FontProgram {
public:
    explicit OpenTypeProgram(std::unique_ptr<FontProgram> inner) : in_(std::move(inner)) {}
    Kind kind() const override { return Kind::OpenType; }
    // successfullyParsed is set once the inner parseFont() returned without an exception.
    bool successful() const override { return in_ && in_->gf_parsed(); }
    float width(int code) override { return in_ ? in_->width(code) : -1.0f; }
    float width(std::string_view n) override { return in_ ? in_->width(n) : -1.0f; }
    bool contains_code(int code) override { return in_ && in_->contains_code(code); }
    bool contains_cid(int cid) override { return in_ && in_->contains_cid(cid); }
    bool contains_glyph(std::string_view n) override { return in_ && in_->contains_glyph(n); }
    std::optional<std::string> glyph_name(int code) override {
        return in_ ? in_->glyph_name(code) : std::nullopt;
    }
    std::vector<int> cid_list() override { return in_ ? in_->cid_list() : std::vector<int>{}; }
    FontProgram* inner() override { return in_ ? in_->inner() : nullptr; }

private:
    std::unique_ptr<FontProgram> in_;
};

}  // namespace

std::unique_ptr<FontProgram> make_opentype_program(std::unique_ptr<FontProgram> inner) {
    return std::make_unique<OpenTypeProgram>(std::move(inner));
}

}  // namespace eqx::fontimpl
