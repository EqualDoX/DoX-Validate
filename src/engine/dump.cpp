#include "engine/dump.h"

#include <cstdio>

#include "core/content.h"
#include "core/document.h"
#include "core/structure.h"
#include "fonts/font.h"
#include "util/text.h"

namespace eqx {

namespace {

std::string esc(const std::string& s, std::size_t max = 60) {
    std::string out;
    for (unsigned char c : s) {
        if (out.size() >= max) {
            out += "...";
            break;
        }
        if (c < 0x20 || c == 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", c);
            out += b;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

const char* kind_name(StreamInstance::Kind k) {
    switch (k) {
        case StreamInstance::Kind::Page: return "page";
        case StreamInstance::Kind::Form: return "form";
        case StreamInstance::Kind::Annotation: return "annot-ap";
        case StreamInstance::Kind::Pattern: return "pattern";
        case StreamInstance::Kind::Type3Glyph: return "type3";
    }
    return "?";
}

const char* item_name(ContentItem::Kind k) {
    switch (k) {
        case ContentItem::Kind::Text: return "text";
        case ContentItem::Kind::LineArt: return "lineart";
        case ContentItem::Kind::Shading: return "shading";
        case ContentItem::Kind::InlineImage: return "inline-image";
        case ContentItem::Kind::ImageXObject: return "image";
    }
    return "?";
}

}  // namespace

std::string dump_model(const std::string& path) {
    OpenStatus status;
    std::string error;
    auto doc = Document::open(path, &status, &error);
    if (!doc) return "cannot open: " + error + "\n";
    std::string out;
    char b[1024];
    std::snprintf(b, sizeof b, "header '%s' version %s, %zu page(s), encrypted %d\n", esc(doc->header()).c_str(),
                  doc->header_version().c_str(), doc->pages().size(), doc->encrypted());
    out += b;
    for (const Page& p : doc->pages()) {
        std::snprintf(b, sizeof b, "page %d obj %s media [%g %g %g %g] crop [%g %g %g %g]\n", p.index + 1,
                      p.ref.str().c_str(), p.media_box.llx, p.media_box.lly, p.media_box.urx, p.media_box.ury,
                      p.crop_box.llx, p.crop_box.lly, p.crop_box.urx, p.crop_box.ury);
        out += b;
    }
    StructTree st;
    st.build(*doc);
    out += st.present() ? "structure tree:\n" : "no structure tree\n";
    for (const auto& [k, v] : st.role_map()) out += "  rolemap " + k + " -> " + v + "\n";
    for (const StructElem& e : st.elems()) {
        std::string kids;
        for (const StructKid& k : e.kids) {
            switch (k.kind) {
                case StructKid::Kind::Elem: kids += " E" + std::to_string(k.elem); break;
                case StructKid::Kind::MCID: kids += " mcid" + std::to_string(k.mcid); break;
                case StructKid::Kind::MCR: kids += " MCR" + std::to_string(k.mcid); break;
                case StructKid::Kind::OBJR: kids += " OBJR(" + ref_of(k.obj).str() + ")"; break;
            }
        }
        auto ns = e.type.ns_uri();
        std::snprintf(b, sizeof b, "  %*sE%d obj %s /S %s ns %s -> ua1 %s, ua2 %s | kids:%s\n", e.depth * 2, "", e.index,
                      e.ref.str().c_str(), e.S ? e.S->c_str() : "(none)", ns ? ns->c_str() : "(null)",
                      e.standard(Flavor::Pdf17).empty() ? "-" : e.standard(Flavor::Pdf17).c_str(),
                      e.standard(Flavor::Pdf20).empty() ? "-" : e.standard(Flavor::Pdf20).c_str(), kids.c_str());
        out += b;
    }
    FontRegistry fonts(*doc);
    ContentModel cm;
    interpret_document(*doc, st, fonts, cm, {});
    for (std::size_t i = 0; i < cm.streams.size(); ++i) {
        const StreamInstance& s = cm.streams[i];
        std::snprintf(b, sizeof b, "stream %zu %s obj %s page %d semantic %d parent %d ops %u errors %zu%s%s\n", i,
                      kind_name(s.kind), s.ref.str().c_str(), s.page + 1, s.semantic, s.parent, s.op_count,
                      s.errors.size(), s.decode_error.empty() ? "" : " decode-error: ",
                      s.decode_error.c_str());
        out += b;
        for (const LexError& e : s.errors) out += "    syntax @" + std::to_string(e.offset) + ": " + e.message + "\n";
    }
    for (std::size_t i = 0; i < cm.frames.size(); ++i) {
        const McFrame& f = cm.frames[i];
        std::snprintf(b, sizeof b, "frame %zu stream %d parent %d %s /%s mcid %s ops %u-%s elem %s%s%s\n", i, f.stream,
                      f.parent, f.bdc ? "BDC" : "BMC", f.tag.c_str(),
                      f.mcid ? std::to_string(*f.mcid).c_str() : "-", f.begin_op,
                      f.end_op == 0xffffffffu ? "open" : std::to_string(f.end_op).c_str(),
                      ref_of(f.struct_elem).str().c_str(), f.lang ? (" Lang=" + esc(*f.lang)).c_str() : "",
                      f.actual_text ? (" ActualText=" + esc(pdf_text_to_utf8(*f.actual_text))).c_str() : "");
        out += b;
    }
    for (std::size_t i = 0; i < cm.items.size(); ++i) {
        const ContentItem& it = cm.items[i];
        std::string txt;
        if (it.text >= 0) txt = " \"" + esc(to_utf8(cm.texts[it.text].text)) + "\"";
        bool tagged = st.reaches_root(cm.item_struct_parent(it));
        std::snprintf(b, sizeof b, "item %zu %s stream %d frame %d op %u tagged %d artifact %d bbox [%.1f %.1f %.1f %.1f]%s\n",
                      i, item_name(it.kind), it.stream, it.frame, it.op_index, tagged,
                      cm.parents_tags_contain(it.stream, it.frame, "Artifact"), it.bbox.llx, it.bbox.lly, it.bbox.urx,
                      it.bbox.ury, txt.c_str());
        out += b;
    }
    for (const auto& f : fonts.all()) {
        std::snprintf(b, sizeof b, "font obj %s /%s /%s\n", f->ref.str().c_str(), f->subtype.c_str(),
                      f->base_font.c_str());
        out += b;
    }
    return out;
}

}  // namespace eqx
