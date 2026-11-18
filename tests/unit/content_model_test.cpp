// Unit tests of the content model's storage: the views that cm.items[i] and cm.texts[i] build must read back exactly
// what the interpreter stored, whatever the width of the text, the size of a run or the number of runs.
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/content.h"
#include "unit.h"

using namespace eqx;

namespace {

content_detail::TextStyle style_of(double size, int tr = 0) {
    content_detail::TextStyle s;
    s.size = size;
    s.size_user = size * 2;
    s.tr = tr;
    return s;
}

// Deterministic pseudo-random numbers (the tests must not depend on the platform's generator).
struct Lcg {
    std::uint64_t state = 0x2545F4914F6CDD1Dull;
    std::uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    }
};

std::u32string read(const ContentModel& cm, std::size_t run) { return cm.texts[run].text.str(); }

}  // namespace

EQX_TEST(content_model_text_roundtrip_all_widths) {
    ContentModel cm;
    const std::vector<std::u32string> texts = {
        U"plain ASCII",
        U"",
        U"Latin-1 äöüÿ",
        std::u32string{0x20AC, 0x4E2D, 0xFFFD, 0xD800},     // a lone surrogate value stays what it is
        U"astral \U0001F600\U0010FFFF",
        std::u32string{0x41, 0x110000, 0xFFFFFFFFu, 0},     // values that are no code points, and U+0000
        U"x",
    };
    for (std::size_t i = 0; i < texts.size(); ++i) {
        const int item = cm.items.add(ContentItem::Kind::Text, 0, -1, static_cast<std::uint32_t>(i), nullptr);
        cm.texts.begin_run(item, style_of(10));
        // one code point at a time, the way the interpreter appends the Unicode value of each glyph
        for (char32_t c : texts[i]) cm.texts.append(&c, 1);
        cm.texts.end_run(static_cast<std::uint32_t>(texts[i].size()), 0);
    }
    EXPECT_EQ(cm.texts.size(), texts.size());
    for (std::size_t i = 0; i < texts.size(); ++i) {
        const TextRun& run = cm.texts[i];
        EXPECT(read(cm, i) == texts[i]);
        EXPECT_EQ(run.text.size(), texts[i].size());
        EXPECT_EQ(run.text.empty(), texts[i].empty());
        EXPECT(run.text == std::u32string_view(texts[i]));
        EXPECT_EQ(to_utf8(run.text), to_utf8(texts[i]));
        EXPECT_EQ(is_whitespace_only(run.text), is_whitespace_only(std::u32string_view(texts[i])));
        std::u32string by_iteration;
        for (char32_t c : run.text) by_iteration.push_back(c);
        EXPECT(by_iteration == texts[i]);
        if (!texts[i].empty()) {
            EXPECT(run.text.front() == texts[i].front());
            EXPECT(run.text.back() == texts[i].back());
        }
        std::u32string appended = U"<";
        appended += run.text;
        EXPECT(appended == U"<" + texts[i]);
        EXPECT_EQ(cm.items[static_cast<std::size_t>(run.item)].text, static_cast<int>(i));
    }
}

EQX_TEST(content_model_text_widens_an_open_run) {
    ContentModel cm;
    // narrow, then wider, then widest code points in one run: what was stored narrow must survive both widenings
    std::u32string expect;
    const int item = cm.items.add(ContentItem::Kind::Text, 0, -1, 0, nullptr);
    cm.texts.begin_run(item, style_of(12));
    for (int round = 0; round < 3; ++round) {
        for (int k = 0; k < 5000; ++k) {
            const char32_t c = round == 0 ? char32_t('a' + k % 26) : round == 1 ? char32_t(0x400 + k % 200) : char32_t(0x1F300 + k % 500);
            cm.texts.append(&c, 1);
            expect.push_back(c);
        }
    }
    EXPECT(cm.texts[0].text.empty());           // not stored yet
    cm.texts.end_run(15000, 7);
    EXPECT(read(cm, 0) == expect);
    EXPECT_EQ(cm.texts[0].glyphs, 15000u);
    EXPECT_EQ(cm.texts[0].unmapped, 7u);
    // a second run after it starts clean and narrow
    const int item2 = cm.items.add(ContentItem::Kind::Text, 0, -1, 1, nullptr);
    cm.texts.begin_run(item2, style_of(12));
    const std::u32string tail = U"tail";
    cm.texts.append(tail.data(), tail.size());
    cm.texts.end_run(4, 0);
    cm.texts.end_run(99, 99);                   // nothing is open: no effect
    EXPECT(read(cm, 1) == tail);
    EXPECT_EQ(cm.texts[1].glyphs, 4u);
    EXPECT(read(cm, 0) == expect);
}

EQX_TEST(content_model_text_many_runs_and_long_runs) {
    ContentModel cm;
    Lcg rng;
    std::vector<std::u32string> expect;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> counts;
    // Runs of very different lengths, so that runs end exactly at, shortly before and far beyond the end of a block
    // of the text arena, and a run outgrows the block it started in.
    for (int i = 0; i < 4000; ++i) {
        std::size_t len = rng.next() % 40;
        if (i % 997 == 0) len = 70000 + rng.next() % 200000;
        if (i % 131 == 0) len = 0;
        const unsigned width = rng.next() % 10;   // mostly narrow text
        std::u32string t;
        for (std::size_t k = 0; k < len; ++k) {
            const std::uint32_t r = rng.next();
            t.push_back(width < 7 ? char32_t(0x20 + r % 0x5f) : width < 9 ? char32_t(0x100 + r % 0xff00) : char32_t(0x10000 + r % 0x100000));
        }
        const int item = cm.items.add(ContentItem::Kind::Text, i % 7, -1, static_cast<std::uint32_t>(i), nullptr);
        cm.texts.begin_run(item, style_of(8 + i % 3));
        // in pieces of varying size
        for (std::size_t at = 0; at < t.size();) {
            const std::size_t n = std::min<std::size_t>(t.size() - at, 1 + rng.next() % 9);
            cm.texts.append(t.data() + at, n);
            at += n;
        }
        // counts on both sides of what an item record holds itself
        const std::uint32_t glyphs = i % 97 == 0 ? 65534u + static_cast<std::uint32_t>(i % 5) : static_cast<std::uint32_t>(t.size());
        const std::uint32_t unmapped = i % 89 == 0 ? 0x10000u + static_cast<std::uint32_t>(i) : static_cast<std::uint32_t>(i % 3);
        cm.texts.end_run(glyphs, unmapped);
        counts.emplace_back(glyphs, unmapped);
        expect.push_back(std::move(t));
    }
    EXPECT_EQ(cm.texts.size(), expect.size());
    bool all = true;
    for (std::size_t i = 0; i < expect.size(); ++i) {
        const TextRun& run = cm.texts[i];
        all = all && run.text.str() == expect[i] && run.glyphs == counts[i].first && run.unmapped == counts[i].second &&
              run.text.size() == expect[i].size();
    }
    EXPECT(all);
}

EQX_TEST(content_model_text_styles_keep_every_bit) {
    ContentModel cm;
    Font* const font_a = reinterpret_cast<Font*>(std::uintptr_t(0x1000));
    Font* const font_b = reinterpret_cast<Font*>(std::uintptr_t(0x2000));
    std::vector<content_detail::TextStyle> styles;
    content_detail::TextStyle s = style_of(10);
    s.font = font_a;
    styles.push_back(s);
    styles.push_back(s);                    // the same again
    s.size = -0.0;                          // 0.0 and -0.0 compare equal and must stay apart
    styles.push_back(s);
    s.size = 0.0;
    styles.push_back(s);
    s.size_user = std::nan("");
    styles.push_back(s);
    styles.push_back(s);
    s.size_user = 3.25;
    s.font = font_b;
    styles.push_back(s);
    s.fill.family = ColorFamily::RGB;
    s.fill.rgb[1] = 0.5f;
    styles.push_back(s);
    s.stroke.family = ColorFamily::CMYK;
    s.stroke.rgb[2] = 0.25f;
    s.fill_alpha = 0.75;
    s.tr = 3;
    styles.push_back(s);
    styles.push_back(styles[0]);            // an earlier style again, after others
    for (std::size_t i = 0; i < styles.size(); ++i) {
        const int item = cm.items.add(ContentItem::Kind::Text, 0, -1, static_cast<std::uint32_t>(i), nullptr);
        cm.texts.begin_run(item, styles[i]);
        cm.texts.end_run(0, 0);
    }
    const auto bits = [](double v) {
        std::uint64_t b;
        std::memcpy(&b, &v, sizeof b);
        return b;
    };
    for (std::size_t i = 0; i < styles.size(); ++i) {
        const TextRun& run = cm.texts[i];
        const content_detail::TextStyle& e = styles[i];
        EXPECT(run.font == e.font);
        EXPECT_EQ(bits(run.size), bits(e.size));
        EXPECT_EQ(bits(run.size_user), bits(e.size_user));
        EXPECT_EQ(bits(run.fill_alpha), bits(e.fill_alpha));
        EXPECT_EQ(run.tr, e.tr);
        EXPECT(run.fill.family == e.fill.family && run.stroke.family == e.stroke.family);
        for (int k = 0; k < 3; ++k) EXPECT(run.fill.rgb[k] == e.fill.rgb[k] && run.stroke.rgb[k] == e.stroke.rgb[k]);
    }
}

// PDF/UA-1 7.1-2 (Matterhorn 01-004) is about tagged content that begins inside an Artifact; an Artifact nested in
// tagged content is the opposite nesting (7.1-1, Matterhorn 01-003). tagged_from_inside_artifact tells them apart by
// the place the structure parent comes from.
EQX_TEST(content_model_tagged_from_inside_artifact) {
    const Obj elem = QPDFObjectHandle::newDictionary();   // stands for a structure element
    ContentModel cm;
    auto stream = [&](int parent, int parent_frame, bool inherited_artifact, bool origin_artifact) {
        StreamInstance s;
        s.parent = parent;
        s.parent_frame = parent_frame;
        s.inherited_artifact = inherited_artifact;
        s.struct_origin_artifact = origin_artifact;
        cm.streams.push_back(std::move(s));
        return static_cast<int>(cm.streams.size()) - 1;
    };
    auto frame = [&](int s, int parent, const char* tag, bool own) {
        McFrame f;
        f.stream = s;
        f.parent = parent;
        f.tag = tag;
        f.own_struct_elem = own ? elem : Obj::newNull();
        f.struct_elem = own ? elem : parent >= 0 ? cm.frames[static_cast<std::size_t>(parent)].struct_elem : Obj::newNull();
        cm.frames.push_back(std::move(f));
        return static_cast<int>(cm.frames.size()) - 1;
    };
    const int page = stream(-1, -1, false, false);

    // /P <</MCID>> BDC  /Artifact BMC  /Span BMC ... : an Artifact, and a sequence in it, inside tagged content
    const int p = frame(page, -1, "P", true);
    const int art_in_p = frame(page, p, "Artifact", false);
    const int span_in_art_in_p = frame(page, art_in_p, "Span", false);
    EXPECT(!cm.tagged_from_inside_artifact(page, p));
    EXPECT(!cm.tagged_from_inside_artifact(page, art_in_p));
    EXPECT(!cm.tagged_from_inside_artifact(page, span_in_art_in_p));
    EXPECT(cm.parents_tags_contain(page, art_in_p, "Artifact"));           // what veraPDF's parentsTags test sees
    EXPECT(cm.parents_tags_contain(page, span_in_art_in_p, "Artifact"));

    // /Artifact BMC  /P <</MCID>> BDC  /Span BMC ... : tagged content inside an Artifact, and what it encloses
    const int art = frame(page, -1, "Artifact", false);
    const int p_in_art = frame(page, art, "P", true);
    const int span_in_p_in_art = frame(page, p_in_art, "Span", false);
    EXPECT(!cm.tagged_from_inside_artifact(page, art));                    // not tagged content at all
    EXPECT(cm.tagged_from_inside_artifact(page, p_in_art));
    EXPECT(cm.tagged_from_inside_artifact(page, span_in_p_in_art));

    // tagged > Artifact > tagged: the inner MCID sequence begins inside the Artifact
    const int inner = frame(page, art_in_p, "Span", true);
    EXPECT(cm.tagged_from_inside_artifact(page, inner));

    // /Artifact <</MCID>> BDC: the sequence is an Artifact and tagged content at once
    const int both = frame(page, -1, "Artifact", true);
    EXPECT(cm.tagged_from_inside_artifact(page, both));

    // A form painted inside tagged content: /Figure <</MCID>> BDC /Fm0 Do EMC, with /Artifact BDC /PlacedGraphic BMC
    // in the form. The structure parent was established outside any Artifact.
    const int figure = frame(page, -1, "Figure", true);
    const int form = stream(page, figure, false, false);
    const int art_in_form = frame(form, -1, "Artifact", false);
    const int placed = frame(form, art_in_form, "PlacedGraphic", false);
    EXPECT(!cm.tagged_from_inside_artifact(form, art_in_form));
    EXPECT(!cm.tagged_from_inside_artifact(form, placed));
    EXPECT(cm.parents_tags_contain(form, placed, "Artifact"));

    // The same form painted inside an Artifact that lies in tagged content: /P BDC /Artifact BMC /Fm1 Do. The form
    // inherits the Artifact tag, but its structure parent still comes from the P outside it.
    const int form_in_art_in_p = stream(page, art_in_p, true, false);
    const int span_in_form = frame(form_in_art_in_p, -1, "Span", false);
    EXPECT(!cm.tagged_from_inside_artifact(form_in_art_in_p, span_in_form));
    EXPECT(cm.parents_tags_contain(form_in_art_in_p, span_in_form, "Artifact"));

    // A form whose structure parent was established inside an Artifact (the interpreter sets the flag at the Do):
    // everything in it is tagged content inside that Artifact, and so is a form it paints in turn.
    const int form_in_art = stream(page, art, true, true);
    const int span_in_form_in_art = frame(form_in_art, -1, "Span", false);
    EXPECT(cm.tagged_from_inside_artifact(form_in_art, span_in_form_in_art));
    EXPECT(cm.tagged_from_inside_artifact(form_in_art, -1));
    // ... but a sequence of the form with an MCID of its own is judged where it stands
    const int own_in_form = frame(form_in_art, -1, "P", true);
    EXPECT(cm.tagged_from_inside_artifact(form_in_art, own_in_form));       // the form was painted inside an Artifact
    const int own_in_plain_form = frame(form, -1, "P", true);
    EXPECT(!cm.tagged_from_inside_artifact(form, own_in_plain_form));
}

EQX_TEST(content_model_item_views) {
    ContentModel cm;
    const Rect box{1, 2, 3, 4};
    const int text = cm.items.add(ContentItem::Kind::Text, 3, 7, 11, nullptr);
    cm.texts.begin_run(text, style_of(9));
    cm.texts.end_run(5, 2);
    Color red;
    red.family = ColorFamily::RGB;
    red.rgb[0] = 1;
    const int path = cm.items.add_line_art(3, -1, 12, &box, 3, red, 0.5f, true);
    const int path2 = cm.items.add_line_art(3, -1, 13, nullptr, 1, red, 0.5f, false);   // the same paint as the path before it
    const int shading = cm.items.add(ContentItem::Kind::Shading, 4, 2, 14, nullptr);
    const int image = cm.items.add_image(4, 2, 15, box, ObjRef{42, 1});
    const int inline_image = cm.items.add(ContentItem::Kind::InlineImage, 5, -1, 16, &box);
    EXPECT_EQ(cm.items.size(), std::size_t(6));

    const ContentItem& t = cm.items[static_cast<std::size_t>(text)];
    EXPECT(t.kind == ContentItem::Kind::Text);
    EXPECT_EQ(t.stream, 3);
    EXPECT_EQ(t.frame, 7);
    EXPECT_EQ(t.op_index, 11u);
    EXPECT(!t.bbox_valid);
    EXPECT(t.bbox.llx == 0 && t.bbox.lly == 0 && t.bbox.urx == 0 && t.bbox.ury == 0);
    EXPECT_EQ(t.text, 0);
    EXPECT(!t.xobject.valid());
    EXPECT(t.fill.family == ColorFamily::Gray && t.fill.rgb[0] == 0 && t.fill_alpha == 1.0f && t.paint == 0 && !t.exact_rect);
    const TextRun& run = cm.texts[0];
    EXPECT_EQ(run.item, text);
    EXPECT_EQ(run.glyphs, 5u);
    EXPECT_EQ(run.unmapped, 2u);
    EXPECT(run.size == 9 && run.size_user == 18);

    const ContentItem& p = cm.items[static_cast<std::size_t>(path)];
    EXPECT(p.kind == ContentItem::Kind::LineArt);
    EXPECT(p.bbox_valid && p.bbox.llx == 1 && p.bbox.ury == 4);
    EXPECT_EQ(p.text, -1);
    EXPECT(p.fill.family == ColorFamily::RGB && p.fill.rgb[0] == 1 && p.fill_alpha == 0.5f);
    EXPECT_EQ(int(p.paint), 3);
    EXPECT(p.exact_rect);
    const ContentItem& p2 = cm.items[static_cast<std::size_t>(path2)];
    EXPECT(!p2.bbox_valid && p2.fill.rgb[0] == 1 && p2.fill_alpha == 0.5f && p2.paint == 1 && !p2.exact_rect);

    const ContentItem& s = cm.items[static_cast<std::size_t>(shading)];
    EXPECT(s.kind == ContentItem::Kind::Shading && s.text == -1 && !s.bbox_valid && s.frame == 2 && s.paint == 0);
    EXPECT(s.fill.family == ColorFamily::Gray && s.fill_alpha == 1.0f);
    const ContentItem& im = cm.items[static_cast<std::size_t>(image)];
    EXPECT(im.kind == ContentItem::Kind::ImageXObject && im.xobject == (ObjRef{42, 1}) && im.text == -1 && im.bbox_valid);
    const ContentItem& ii = cm.items[static_cast<std::size_t>(inline_image)];
    EXPECT(ii.kind == ContentItem::Kind::InlineImage && !ii.xobject.valid() && ii.stream == 5);

    // The boxes and colours of a view are the model's: they stay valid and current when the view is gone.
    const Rect* kept_box = &cm.items[static_cast<std::size_t>(path)].bbox;
    const float* kept_fill = cm.items[static_cast<std::size_t>(path)].fill.rgb;
    for (int i = 0; i < 5000; ++i) cm.items.add(ContentItem::Kind::Shading, 0, -1, 100 + i, nullptr);
    EXPECT(kept_box->urx == 3 && kept_fill[0] == 1);

    std::size_t n = 0;
    int ops = 0;
    for (const ContentItem& it : cm.items) {
        if (n < 6) ops += static_cast<int>(it.op_index);
        ++n;
    }
    EXPECT_EQ(n, cm.items.size());
    EXPECT_EQ(ops, 11 + 12 + 13 + 14 + 15 + 16);
}

EQX_TEST(content_model_opt_string) {
    OptString none;
    EXPECT(!none && !none.has_value());
    EXPECT_EQ(none.value_or("x"), std::string("x"));
    OptString some = std::optional<std::string>("abc");
    EXPECT(some && *some == "abc" && some->size() == 3 && some.value() == "abc");
    OptString copy = some;
    some = std::optional<std::string>();
    EXPECT(!some);
    EXPECT(copy && *copy == "abc");
    const std::optional<std::string> back = copy;
    EXPECT(back && *back == "abc");
    const std::optional<std::string> nothing = some;
    EXPECT(!nothing);
    McFrame f;
    f.lang = std::optional<std::string>("de");
    McFrame g = f;
    McFrame h = std::move(f);
    EXPECT(g.lang && *g.lang == "de" && h.lang && *h.lang == "de" && !g.alt);
}

namespace {
struct Counted {
    static int alive;
    int v;
    explicit Counted(int x) : v(x) { ++alive; }
    Counted(const Counted& o) : v(o.v) { ++alive; }
    ~Counted() { --alive; }
};
int Counted::alive = 0;
}  // namespace

EQX_TEST(content_model_chunked_vector) {
    {
        ChunkedVector<Counted, 16> v;
        EXPECT(v.empty());
        for (int i = 0; i < 100; ++i) v.emplace_back(i);
        const Counted* first = &v[0];
        for (int i = 100; i < 1000; ++i) v.push_back(Counted(i));
        EXPECT_EQ(v.size(), std::size_t(1000));
        EXPECT_EQ(Counted::alive, 1000);
        EXPECT(first == &v[0]);            // growing never moves an element
        int sum = 0;
        for (const Counted& c : v) sum += c.v;
        EXPECT_EQ(sum, 999 * 1000 / 2);
        EXPECT_EQ(v.back().v, 999);
        ChunkedVector<Counted, 16> w = std::move(v);
        EXPECT_EQ(w.size(), std::size_t(1000));
        EXPECT(v.empty());
        EXPECT_EQ(Counted::alive, 1000);
        w.clear();
        EXPECT_EQ(Counted::alive, 0);
        w.emplace_back(7);
        EXPECT_EQ(w[0].v, 7);
    }
    EXPECT_EQ(Counted::alive, 0);
}

EQX_TEST(content_model_release_keeps_indices_and_fails_loudly) {
    ContentModel cm;
    Lcg rng;
    std::vector<std::u32string> expect;
    const auto add_page = [&](int runs) {
        for (int i = 0; i < runs; ++i) {
            std::u32string t;
            const std::size_t len = (expect.size() % 211 == 0) ? 100000 + rng.next() % 50000 : rng.next() % 30;
            for (std::size_t k = 0; k < len; ++k) t.push_back(char32_t(0x20 + rng.next() % (expect.size() % 7 == 0 ? 0x3000 : 0x5f)));
            const int item = cm.items.add(ContentItem::Kind::Text, 0, -1, static_cast<std::uint32_t>(expect.size()), nullptr);
            cm.texts.begin_run(item, style_of(10));
            if (!t.empty()) cm.texts.append(t.data(), t.size());
            cm.texts.end_run(static_cast<std::uint32_t>(t.size()), 0);
            Color c;
            c.rgb[0] = 0.25f;
            cm.items.add_line_art(0, -1, 1, nullptr, 1, c, 1.0f, false);
            expect.push_back(std::move(t));
        }
    };
    const auto throws = [](auto&& f) {
        try {
            f();
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    };
    std::size_t released_items = 0, released_runs = 0;
    for (int page = 0; page < 12; ++page) {
        const std::size_t first_run = cm.texts.size(), first_item = cm.items.size();
        add_page(700 + page * 130);
        // the page reads back, with the indices of the whole document
        bool same = true;
        for (std::size_t r = first_run; r < cm.texts.size(); ++r) {
            const TextRun& run = cm.texts[r];
            same = same && run.text.str() == expect[r] && run.glyphs == expect[r].size() &&
                   run.item == static_cast<int>(first_item + 2 * (r - first_run)) && cm.items[static_cast<std::size_t>(run.item)].text == static_cast<int>(r);
            const ContentItem& path = cm.items[static_cast<std::size_t>(run.item) + 1];
            same = same && path.kind == ContentItem::Kind::LineArt && path.fill.rgb[0] == 0.25f && path.paint == 1;
        }
        EXPECT(same);
        if (page % 3 == 2) continue;   // some pages stay until the next release
        cm.release_items();
        released_items = cm.items.size();
        released_runs = cm.texts.size();
        EXPECT_EQ(cm.items.live_begin(), released_items);
        EXPECT_EQ(cm.texts.live_begin(), released_runs);
        EXPECT(throws([&] { (void)cm.items[released_items - 1]; }));
        EXPECT(throws([&] { (void)cm.items[0]; }));
        EXPECT(throws([&] { (void)cm.texts[released_runs - 1]; }));
        EXPECT(throws([&] { (void)cm.texts[0]; }));
        EXPECT(throws([&] { (void)cm.items.record(first_item); }));
    }
    EXPECT_EQ(cm.texts.size(), expect.size());
    EXPECT_EQ(cm.items.size(), 2 * expect.size());
    // range-for from the start reaches released items: it fails instead of reading something else
    EXPECT(throws([&] {
        for (const ContentItem& it : cm.items) (void)it;
    }));
}

EQX_TEST(content_model_chunked_vector_release) {
    {
        ChunkedVector<Counted, 16> v;
        for (int i = 0; i < 100; ++i) v.emplace_back(i);
        v.release_before(15);                 // no whole chunk below 15
        EXPECT_EQ(Counted::alive, 100);
        EXPECT_EQ(v.released_end(), std::size_t(0));
        v.release_before(40);                 // chunks 0 and 1
        EXPECT_EQ(Counted::alive, 68);
        EXPECT_EQ(v.released_end(), std::size_t(32));
        EXPECT_EQ(v[32].v, 32);
        EXPECT_EQ(v.back().v, 99);
        v.release_before(20);                 // nothing more
        EXPECT_EQ(Counted::alive, 68);
        for (int i = 100; i < 200; ++i) v.emplace_back(i);
        EXPECT_EQ(v[199].v, 199);
        v.release_before(1000);               // more than there is: whole chunks only, the last partly used one stays
        EXPECT_EQ(v.released_end(), std::size_t(192));
        EXPECT_EQ(Counted::alive, 8);
        EXPECT_EQ(v.size(), std::size_t(200));
        ChunkedVector<Counted, 16> w = std::move(v);
        EXPECT_EQ(w.released_end(), std::size_t(192));
        EXPECT_EQ(w[195].v, 195);
    }
    EXPECT_EQ(Counted::alive, 0);
}
