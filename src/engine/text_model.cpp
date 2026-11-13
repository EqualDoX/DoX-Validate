#include "engine/text_model.h"

#include <map>

#include "core/content.h"
#include "core/document.h"
#include "core/structure.h"
#include "fonts/font.h"
#include "util/json.h"
#include "util/text.h"

namespace eqx {

namespace {

void text_field(JsonWriter& w, const char* key, Obj value) {
    if (!value.isString()) return;
    w.key(key).value(pdf_text_to_utf8(value.getStringValue()));
}

void box(JsonWriter& w, const Rect& r) {
    w.begin_array().value(r.llx).value(r.lly).value(r.urx).value(r.ury).end_array();
}

// The outermost /ActualText around a frame replaces everything inside it (-1: none).
int actual_text_frame(const ContentModel& cm, int frame) {
    int found = -1;
    for (int f = frame; f >= 0; f = cm.frames[f].parent)
        if (cm.frames[f].actual_text) found = f;
    return found;
}

}  // namespace

std::string text_model_json(const std::string& path, bool* ok) {
    OpenStatus status;
    std::string error;
    auto doc = Document::open(path, &status, &error);
    JsonWriter w;
    w.begin_object().key("schema").value("eqx-text-model/1");
    if (!doc) {
        *ok = false;
        w.key("error").value(error).end_object();
        return w.take();
    }
    *ok = true;
    std::map<std::string, int> page_index;
    w.key("pages").begin_array();
    for (const Page& p : doc->pages()) {
        page_index[p.ref.str()] = p.index;
        w.begin_object().key("page").value(p.index + 1).key("box");
        box(w, p.crop_box);
        w.end_object();
    }
    w.end_array();
    text_field(w, "lang", cos::get(doc->catalog(), "Lang"));
    text_field(w, "info_title", cos::get(doc->info(), "Title"));

    StructTree st;
    st.build(*doc);
    w.key("tagged").value(st.present());
    std::map<std::string, int> elem_index;
    w.key("elements").begin_array();
    for (const StructElem& e : st.elems()) {
        elem_index[e.ref.str()] = e.index;
        std::string role = e.standard(Flavor::Pdf17);
        if (role.empty()) role = e.standard(Flavor::Pdf20);
        w.begin_object().key("i").value(e.index).key("parent").value(e.parent).key("role").value(role);
        auto pg = page_index.find(e.inherited_page.str());
        if (pg != page_index.end()) w.key("page").value(pg->second + 1);
        text_field(w, "alt", cos::get(e.dict, "Alt"));
        text_field(w, "actual_text", cos::get(e.dict, "ActualText"));
        text_field(w, "lang", cos::get(e.dict, "Lang"));
        w.key("kids").begin_array();
        for (int k : e.elems) w.value(k);
        w.end_array().end_object();
    }
    w.end_array();

    FontRegistry fonts(*doc);
    ContentModel cm;
    interpret_document(*doc, st, fonts, cm, {});
    w.key("runs").begin_array();
    for (const ContentItem& it : cm.items) {
        if (it.text < 0 || !it.bbox_valid) continue;
        const StreamInstance& s = cm.streams[it.stream];
        if (!s.semantic || s.page < 0) continue;
        if (s.kind != StreamInstance::Kind::Page && s.kind != StreamInstance::Kind::Form) continue;
        const TextRun& run = cm.texts[it.text];
        int at = actual_text_frame(cm, it.frame);
        if (run.text.empty() && run.unmapped == 0 && at < 0) continue;
        auto el = elem_index.find(ref_of(cm.item_struct_parent(it)).str());
        w.begin_object().key("p").value(s.page + 1);
        w.key("e").value(el != elem_index.end() ? el->second : -1);
        if (cm.parents_tags_contain(it.stream, it.frame, "Artifact")) w.key("a").value(true);
        if (run.tr == 3 || run.tr == 7) w.key("inv").value(true);
        if (run.unmapped) w.key("u").value(static_cast<std::int64_t>(run.unmapped));
        if (at >= 0) {
            w.key("af").value(at);
            w.key("at").value(pdf_text_to_utf8(*cm.frames[at].actual_text));
        }
        w.key("b");
        box(w, it.bbox);
        w.key("t").value(to_utf8(run.text)).end_object();
    }
    w.end_array().end_object();
    return w.take();
}

}  // namespace eqx
