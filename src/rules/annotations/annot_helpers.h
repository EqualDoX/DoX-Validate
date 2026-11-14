// Pure helpers of the annotations module: veraPDF 1.30.2 COS-layer conversions (COSObject.getInteger /
// getReal / getString, COSDictionary.getStringKey, TypeConverter.getRealArray), page boxes as
// org.verapdf.pd.PDPage computes them, number/name-tree lookups with the exact search semantics of
// PDNumberTreeNode.getObject / PDNameTreeNode.getObject, and the rich-text flattening of
// DictionaryKeysHelper.getRichTextStringOrStreamEntryStringRepresentation.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/cos.h"
#include "core/tree_shape.h"

namespace eqx::annot {

// COSObject.getInteger(): integer -> value, real -> (long) truncation, string -> its byte length
// (COSString.getInteger), anything else -> null.
std::optional<std::int64_t> vera_integer(const Obj& h);
// COSObject.getReal(): integer/real -> value, string -> byte length, else null.
std::optional<double> vera_real(const Obj& h);
// COSDictionary.getStringKey(): a string decoded like COSString.getString; names and other types -> null.
std::optional<std::string> vera_string_key(const Obj& dict, std::string_view key);
// COSObject.getString(): string decoded, a name's value, else null (keys of name-tree /Names arrays).
std::optional<std::string> vera_string(const Obj& h);
// TypeConverter.getRealArray(obj, n, name): every element must have a getReal() value, any size.
std::optional<std::vector<double>> vera_real_array(const Obj& h);

using Box = std::array<double, 4>;
// PDPage.getCropBox(): inherited /CropBox (an inherited non-array value ends the search) converted with
// getDoubleArrayForBox (non-numbers 0, missing entries 0) and clipped to the MediaBox; else the MediaBox.
// nullopt when neither box exists (veraPDF would fail with a NullPointerException).
std::optional<Box> vera_crop_box(const Obj& page);
// GFPDAnnot.getisOutsideCropBox (raw, non-normalised rectangles); nullopt = property null.
std::optional<bool> outside_crop_box(const std::optional<Box>& crop, const std::optional<std::vector<double>>& rect);

// Lookups with the search semantics of the veraPDF parser (Limits respected, /Nums or /Names before /Kids,
// later duplicates win, loops end the branch). Parsed leaf maps are cached per indirect node.
class NumberTree {
public:
    explicit NumberTree(Obj root) : root_(std::move(root)) {}
    // Null handle when the key is not found.
    Obj get(std::int64_t key);

private:
    using Nums = std::unordered_map<std::int64_t, Obj>;
    const Nums& nums_of(const Obj& node, Nums& scratch);
    Obj root_;
    std::unordered_map<std::uint64_t, Nums> cache_;
    std::optional<Nums> root_cache_;
    std::unique_ptr<TreeShape<std::int64_t>> shape_;   // the /Kids and /Limits, as lookups reach them
};

class NameTree {
public:
    explicit NameTree(Obj root) : root_(std::move(root)) {}
    Obj get(const std::string& key);

private:
    using Names = std::unordered_map<std::string, Obj>;
    const Names& names_of(const Obj& node, Names& scratch);
    Obj root_;
    std::unordered_map<std::uint64_t, Names> cache_;
    std::optional<Names> root_cache_;
    std::unique_ptr<TreeShape<std::string>> shape_;    // the /Kids and /Limits (as UTF-8), as lookups reach them
};

// DictionaryKeysHelper.getRichTextStringOrStreamEntryStringRepresentation on the XML bytes: the
// concatenated values of all text, CDATA and processing-instruction nodes (comments are ignored by the
// namespace-aware JAXP builder veraPDF uses); nullopt when the bytes are not well-formed namespace XML.
std::optional<std::string> rich_text_plain(std::string_view xml);

// The value veraPDF compares for a rich-text entry (string or stream) / plain text entry (string or text
// stream decoded like COSString.getString). nullopt = property null.
std::optional<std::string> rich_text_entry(const Obj& dict, std::string_view key);
std::optional<std::string> text_or_stream_entry(const Obj& dict, std::string_view key);

// GFPDMediaClip.gethasCorrectAlt: /Alt is an array of an even number of strings whose odd entries
// (the texts) are not empty.
bool media_clip_has_correct_alt(const Obj& clip);

}  // namespace eqx::annot
