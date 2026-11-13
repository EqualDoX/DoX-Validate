#include "fonts/font.h"

#include "core/document.h"
#include "fonts/model.h"

namespace eqx {

FontRegistry::FontRegistry(Document& doc) : doc_(doc), cache_(std::make_shared<fontimpl::DocCache>()) {}
FontRegistry::~FontRegistry() = default;

const Font* FontRegistry::get(const Obj& font_dict) {
    if (!font_dict.isDictionary()) return nullptr;
    ObjRef r = ref_of(font_dict);
    if (r.valid()) {
        auto it = by_ref_.find(r.key());
        if (it != by_ref_.end()) return it->second;
    } else {
        // Direct dictionaries (rare): identity of the underlying qpdf object.
        for (auto& [h, f] : direct_)
            if (h.isSameObjectAs(font_dict)) return f;
    }
    std::unique_ptr<Font> f = fontimpl::load_vera_font(doc_, cache_, font_dict);
    if (!f) return nullptr;
    Font* raw = f.get();
    fonts_.push_back(std::move(f));
    if (r.valid()) by_ref_.emplace(r.key(), raw);
    else direct_.emplace_back(font_dict, raw);
    return raw;
}

// Stand-alone loader (a private cache per font).
std::unique_ptr<Font> load_font(Document& doc, const Obj& font_dict) {
    if (!font_dict.isDictionary()) return nullptr;
    return fontimpl::load_vera_font(doc, std::make_shared<fontimpl::DocCache>(), font_dict);
}

}  // namespace eqx
