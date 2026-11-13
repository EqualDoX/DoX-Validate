// EqualDoX's WCAG colour checks (rules/pac_quality/wcag_color.h): the judgement, and whole validations of small generated PDFs.
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <filesystem>

#include "engine/validate.h"
#include "rules/pac_quality/wcag_color.h"
#include "unit.h"

using namespace eqx;

namespace {

wcag::TextFacts facts(double ratio, double size, int bold = 0) {
    wcag::TextFacts f;
    f.ratio = ratio;
    f.size = size;
    f.bold = bold;
    return f;
}

constexpr const char* kConfirmed = "EQX-WCAG-1.4.3-TextContrast";
constexpr const char* kReview = "EQX-WCAG-1.4.3-TextContrastReview";
constexpr const char* kNonText = "EQX-WCAG-1.4.11-NonTextContrastReview";
constexpr const char* kColour = "EQX-WCAG-1.4.1-UseOfColorReview";

struct Doc {
    std::string content;
    std::string resources;          // extra resource entries
    std::string annots;             // annotation dictionaries, page reference written as PAGE
    bool figure = false;            // tagged: one Figure element owning MCID 0
    std::string version = "1.7";
};

// The generated files live in one directory per test process, removed when the process ends.
const std::filesystem::path& scratch() {
    struct Dir {
        std::filesystem::path p = std::filesystem::temp_directory_path() / ("eqx_wcag_test_" + std::to_string(getpid()));
        ~Dir() {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    };
    static const Dir d;
    return d.p;
}

std::string write(const std::string& name, const Doc& d) {
    const auto& dir = scratch();
    std::filesystem::create_directories(dir);
    const std::string path = (dir / (name + ".pdf")).string();
    QPDF q;
    q.emptyPDF();
    auto font = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
    auto bold = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>"));
    auto res = QPDFObjectHandle::parse("<< /Font << >> " + d.resources + " >>");
    res.getKey("/Font").replaceKey("/F1", font);
    res.getKey("/Font").replaceKey("/F2", bold);
    auto page = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    page.replaceKey("/Resources", res);
    page.replaceKey("/Contents", q.makeIndirectObject(QPDFObjectHandle::newStream(&q, d.content)));
    QPDFPageDocumentHelper(q).addPage(page, false);
    page = q.getRoot().getKey("/Pages").getKey("/Kids").getArrayItem(0);
    if (!d.annots.empty()) {
        auto arr = QPDFObjectHandle::newArray();
        for (QPDFObjectHandle a : QPDFObjectHandle::parse("[" + d.annots + "]").getArrayAsVector()) {
            a.replaceKey("/P", page);
            arr.appendItem(q.makeIndirectObject(a));
        }
        page.replaceKey("/Annots", arr);
    }
    if (d.figure) {
        auto root = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /StructTreeRoot >>"));
        auto doc = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /StructElem /S /Document >>"));
        auto fig = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /StructElem /S /Figure /Alt (Chart) /K 0 >>"));
        doc.replaceKey("/P", root);
        doc.replaceKey("/K", QPDFObjectHandle::newArray({fig}));
        fig.replaceKey("/P", doc);
        fig.replaceKey("/Pg", page);
        root.replaceKey("/K", doc);
        auto nums = QPDFObjectHandle::newArray({QPDFObjectHandle::newInteger(0), QPDFObjectHandle::newArray({fig})});
        auto pt = QPDFObjectHandle::newDictionary();
        pt.replaceKey("/Nums", nums);
        root.replaceKey("/ParentTree", q.makeIndirectObject(pt));
        page.replaceKey("/StructParents", QPDFObjectHandle::newInteger(0));
        q.getRoot().replaceKey("/StructTreeRoot", root);
        q.getRoot().replaceKey("/MarkInfo", QPDFObjectHandle::parse("<< /Marked true >>"));
    }
    QPDFWriter w(q, path.c_str());
    w.setStaticID(true);
    w.forcePDFVersion(d.version);
    w.write();
    return path;
}

Result run(const std::string& name, const Doc& d) { return validate_file(write(name, d), Options{}); }

const FindingRecord* finding(const Result& r, const char* id) {
    for (const FindingRecord* f : r.findings.sorted())
        if (Catalog::get().rule(f->rule).id == std::string_view(id)) return f;
    return nullptr;
}

bool detail_has(const Result& r, const char* id, const char* text) {
    const FindingRecord* f = finding(r, id);
    if (!f) return false;
    for (const Location& l : f->locations)
        if (l.detail.find(text) != std::string::npos) return true;
    return false;
}

bool fails(const Result& r, int target, const char* id) {
    for (RuleIndex i : r.assessments.single[target].failed_rules)
        if (Catalog::get().rule(i).id == std::string_view(id)) return true;
    return false;
}

const std::string kText = "BT /F1 12 Tf 72 700 Td (Antrag auf Wohngeld) Tj ET\n";

}  // namespace

// --- the judgement ---------------------------------------------------------------------------------------------

EQX_TEST(wcag_judge_thresholds) {
    EXPECT(wcag::judge_text(facts(4.6, 10)).verdict == wcag::Verdict::Pass);
    EXPECT(wcag::judge_text(facts(4.5, 10)).verdict == wcag::Verdict::Pass);          // exactly the minimum
    EXPECT(wcag::judge_text(facts(4.49, 10)).verdict == wcag::Verdict::Violation);    // no rounding up
    EXPECT_EQ(wcag::judge_text(facts(4.49, 10)).required, std::string("4.5:1"));
    // large text: 18 pt, or 14 pt bold
    EXPECT(wcag::judge_text(facts(3.2, 24)).verdict == wcag::Verdict::Pass);
    EXPECT(wcag::judge_text(facts(3.2, 15, 1)).verdict == wcag::Verdict::Pass);
    EXPECT(wcag::judge_text(facts(3.2, 15, 0)).verdict == wcag::Verdict::Violation);
    EXPECT(wcag::judge_text(facts(2.9, 24)).verdict == wcag::Verdict::Violation);
    EXPECT_EQ(wcag::judge_text(facts(2.9, 24)).required, std::string("3:1 (large text)"));
}

EQX_TEST(wcag_judge_uncertain_is_review) {
    // between 3:1 and 4.5:1 the weight decides, and it cannot be told
    wcag::TextJudgement j = wcag::judge_text(facts(3.5, 15, -1));
    EXPECT(j.verdict == wcag::Verdict::Review);
    EXPECT(j.reasons.find("large text") != std::string::npos);
    EXPECT(wcag::judge_text(facts(3.5, 17.999)).verdict == wcag::Verdict::Pass);       // a scaled 18 pt font
    EXPECT(wcag::judge_text(facts(3.5, 17.9)).verdict == wcag::Verdict::Violation);
    // below 3:1 it fails either way
    EXPECT(wcag::judge_text(facts(2.5, 15, -1)).verdict == wcag::Verdict::Violation);
    for (int k = 0; k < 7; ++k) {
        wcag::TextFacts f = facts(2.5, 10);
        bool* flags[] = {&f.rendered, &f.device_colour, &f.transparency, &f.clipped, &f.stroked, &f.artifact, &f.in_figure};
        *flags[k] = true;
        EXPECT(wcag::judge_text(f).verdict == wcag::Verdict::Review);
    }
    EXPECT(wcag::judge_text(facts(1.0, 10)).verdict == wcag::Verdict::Review);         // the background's colour: hidden text?
}

EQX_TEST(wcag_ratio_is_cut_not_rounded) {
    EXPECT_EQ(wcag::ratio_str(4.4999), std::string("4.49:1"));
    const unsigned char black[3] = {0, 0, 0}, white[3] = {255, 255, 255};
    EXPECT(wcag::contrast8(black, white) > 20.99);
}

// --- whole validations -----------------------------------------------------------------------------------------

EQX_TEST(wcag_black_text_passes) {
    Result r = run("pass", {"0 g " + kText});
    EXPECT(r.processed);
    EXPECT(!finding(r, kConfirmed) && !finding(r, kReview) && !finding(r, kColour) && !finding(r, kNonText));
}

EQX_TEST(wcag_grey_text_confirmed) {
    // #999999 on the white page: 2.84:1, needs 4.5:1; it fails WCAG 2.1 AA
    Result r = run("grey", {"0.6 g " + kText});
    const FindingRecord* f = finding(r, kConfirmed);
    EXPECT(f && f->locations.size() == 1 && f->locations[0].page == 1);
    EXPECT(detail_has(r, kConfirmed, "WCAG 1.4.3 confirmed: contrast 2.84:1 (text #999999 on #ffffff, 12.0 pt), required 4.5:1"));
    EXPECT(detail_has(r, kConfirmed, "\"Antrag auf Wohngeld\""));
    EXPECT(!finding(r, kReview));
    EXPECT(fails(r, T_WCAG21_AA, kConfirmed));
    EXPECT(!fails(r, T_WCAG21_A, kConfirmed));   // 1.4.3 is a level AA criterion
}

EQX_TEST(wcag_grey_on_flat_rectangle_confirmed) {
    Result r = run("rect", {"0.2 g 60 690 300 30 re f 0.4 g " + kText});
    EXPECT(detail_has(r, kConfirmed, "on #333333"));
}

EQX_TEST(wcag_large_text_exception) {
    // #808080: 3.95:1, enough for 24 pt and for 14 pt bold, not for 12 pt
    EXPECT(!finding(run("large", {"0.5 g BT /F1 24 Tf 72 700 Td (Antrag) Tj ET"}), kConfirmed));
    EXPECT(!finding(run("bold", {"0.5 g BT /F2 15 Tf 72 700 Td (Antrag) Tj ET"}), kConfirmed));
    EXPECT(finding(run("small", {"0.5 g BT /F1 12 Tf 72 700 Td (Antrag) Tj ET"}), kConfirmed));
}

EQX_TEST(wcag_invisible_text_exempt) {
    // rendering mode 3, clipped away, covered by a later opaque rectangle: not visible to anyone
    EXPECT(!finding(run("tr3", {"0.6 g BT 3 Tr /F1 12 Tf 72 700 Td (Antrag) Tj ET"}), kConfirmed));
    Result c = run("clip", {"q 300 300 10 10 re W n 0.6 g " + kText + "Q"});
    EXPECT(!finding(c, kConfirmed) && !finding(c, kReview));
    Result h = run("covered", {"0.6 g " + kText + "0.9 g 60 690 300 30 re f"});
    EXPECT(!finding(h, kConfirmed) && !finding(h, kReview));
}

EQX_TEST(wcag_uncertain_measurements_are_review) {
    // background painted under half of the text: rendered, so for review only
    Result p = run("partial", {"0.85 g 60 690 60 30 re f 0.6 g " + kText});
    EXPECT(!finding(p, kConfirmed));
    EXPECT(detail_has(p, kReview, "WCAG 1.4.3 manual review"));
    EXPECT(detail_has(p, kReview, "background measured on the rendering"));
    EXPECT(!fails(p, T_WCAG21_AA, kReview));   // a review finding never fails a target
    // CMYK text: the colour depends on the output device
    Result k = run("cmyk", {"0 0 0 0.3 k " + kText});
    EXPECT(!finding(k, kConfirmed));
    EXPECT(detail_has(k, kReview, "device-dependent colour"));
    // artifact (page furniture or decoration)
    Result a = run("artifact", {"/Artifact BMC 0.6 g " + kText + "EMC"});
    EXPECT(!finding(a, kConfirmed));
    EXPECT(detail_has(a, kReview, "marked as artifact"));
    // semi-transparent black over white: #7f7f7f, PAC ignores the transparency
    Result t = run("alpha", {"/GS1 gs " + kText, "/ExtGState << /GS1 << /ca 0.5 >> >>"});
    EXPECT(detail_has(t, kConfirmed, "text #7f7f7f") || detail_has(t, kConfirmed, "text #808080"));
}

EQX_TEST(wcag_figure_text_and_graphics_review) {
    Doc d;
    d.figure = true;
    d.content = "/Figure << /MCID 0 >> BDC 0.85 g 100 100 100 50 re f 0.2 g 220 100 100 50 re f 0.6 g " + kText + "EMC";
    Result r = run("figure", d);
    EXPECT(!finding(r, kConfirmed));
    EXPECT(detail_has(r, kReview, "inside a Figure"));
    EXPECT(detail_has(r, kNonText, "WCAG 1.4.11 manual review: 1 of 2 graphical parts of this Figure"));
    EXPECT(detail_has(r, kNonText, "#d9d9d9 on #ffffff"));
    const FindingRecord* f = finding(r, kNonText);
    EXPECT(f && f->locations[0].page == 1 && f->locations[0].obj.valid());   // the Figure element
    // dark parts only (the outline of a bar is no part of its own): nothing to review
    d.content = "/Figure << /MCID 0 >> BDC 0.2 g 100 100 100 50 re f 0.22 G 100 100 100 50 re S EMC";
    EXPECT(!finding(run("figure_dark", d), kNonText));
}

EQX_TEST(wcag_form_field_boundary_review) {
    const std::string field = "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /F 4 /Rect [100 600 300 620] /MK << /BC [0.8] >> ";
    Result r = run("field", {"", "", field + ">>"});
    EXPECT(detail_has(r, kNonText, "form field \"Name\" (text field): its boundary has 1.60:1 against the page #ffffff"));
    EXPECT(!finding(run("field_dark", {"", "", "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name) /F 4 /Rect [100 600 300 620] /MK << /BC [0.3] >> >>"}), kNonText));
    EXPECT(!finding(run("field_readonly", {"", "", field + "/Ff 1 >>"}), kNonText));   // inactive component
    EXPECT(!finding(run("field_hidden", {"", "", "<< /Type /Annot /Subtype /Widget /FT /Tx /F 2 /Rect [100 600 300 620] /MK << /BC [0.8] >> >>"}), kNonText));
    // the page draws the frame itself: not judged from /MK
    EXPECT(!finding(run("field_frame", {"0 G 100 600 200 20 re S", "", field + ">>"}), kNonText));
}

EQX_TEST(wcag_colour_only_link_review) {
    const std::string line = "BT /F1 12 Tf 72 700 Td (Weitere Hinweise unter ) Tj 0 0 1 rg (www.example.de) Tj 0 g ( im Internet.) Tj ET\n";
    Result r = run("link", {line, "", "<< /Type /Annot /Subtype /Link /Border [0 0 0] /Rect [190 696 280 712] >>"});
    EXPECT(detail_has(r, kColour, "WCAG 1.4.1 manual review: \"www.example.de\" (#0000ff) is set apart from the adjacent text (#000000)"));
    EXPECT(detail_has(r, kColour, "it is a link"));
    EXPECT(!fails(r, T_WCAG21_A, kColour));
    // an underline, or another font, is a cue besides the colour
    EXPECT(!finding(run("link_underlined", {line + "0 0 1 rg 190 697 85 0.8 re f"}), kColour));
    EXPECT(!finding(run("link_bold", {"BT /F1 12 Tf 72 700 Td (Weitere Hinweise unter ) Tj /F2 12 Tf 0 0 1 rg (www.example.de) Tj ET"}), kColour));
    // one colour for the whole line: nothing set apart
    EXPECT(!finding(run("one_colour", {"BT /F1 12 Tf 72 700 Td (Weitere Hinweise unter ) Tj (www.example.de) Tj ET"}), kColour));
    // a link longer than the text beside it: the page's usual colour decides which text is set apart
    Result l = run("long_link", {"BT /F1 12 Tf 72 700 Td (Siehe ) Tj 0 0 1 rg (www.stadtverwaltung-musterstadt.example.de) Tj 0 g "
                                 "0 -14 Td (Antraege koennen schriftlich oder elektronisch gestellt werden.) Tj ET"});
    EXPECT(detail_has(l, kColour, "\"www.stadtverwaltung-musterstadt.example."));   // excerpts end at 40 bytes
    EXPECT(detail_has(l, kColour, "(#0000ff) is set apart from the adjacent text (#000000)"));
    EXPECT(!detail_has(l, kColour, "(#000000) is set apart"));
}

EQX_TEST(wcag_checks_pdf20) {
    // PAC's checks do not apply to PDF 2.0; the WCAG rules do
    Doc d{"0.6 g " + kText};
    d.version = "2.0";
    Result r = run("pdf20", d);
    EXPECT(finding(r, kConfirmed));
    EXPECT(!finding(r, "EQX-PAC-FailNotEnoughContrastForText"));
}

EQX_TEST(wcag_rule_selection_runs_only_the_wcag_rules) {
    // dox-validate --rules EQX-WCAG- (the dashboard's contrast pre-check): the same WCAG findings as a full validation,
    // nothing evaluated or reported outside the selection
    Options sel;
    sel.rule_prefix = "EQX-WCAG-";
    const std::string path = write("selection", {"0.6 g " + kText});
    Result full = validate_file(path, Options{});
    Result r = validate_file(path, sel);
    EXPECT(r.processed);
    EXPECT(finding(r, kConfirmed) && finding(full, kConfirmed));
    EXPECT_EQ(finding(r, kConfirmed)->occurrences, finding(full, kConfirmed)->occurrences);
    const Catalog& cat = Catalog::get();
    std::size_t evaluated = 0;
    for (RuleIndex i = 0; i < cat.size(); ++i) {
        const bool wcag = std::string_view(cat.rule(i).id).rfind("EQX-WCAG-", 0) == 0;
        EXPECT(r.in_selection(i) == wcag);
        if (r.evaluated[i]) {
            EXPECT(wcag);
            ++evaluated;
        }
    }
    EXPECT_EQ(evaluated, std::size_t(4));
    for (const FindingRecord* f : r.findings.sorted()) EXPECT(std::string_view(cat.rule(f->rule).id).rfind("EQX-WCAG-", 0) == 0);
    for (const auto& [rule, why] : r.incomplete_reasons) EXPECT(r.in_selection(rule));
    EXPECT(!full.findings.sorted().empty() && full.findings.sorted().size() > r.findings.sorted().size());
}

EQX_TEST(wcag_max_pages_refuses_before_validating) {
    EQX_REQUIRE_FIXTURES();
    // a two-page fixture: refused above the limit (not processed, nothing evaluated), validated at the limit
    const std::string path = eqx_test::fixture("U1-A01-annotations-forms.pdf");
    Options opt;
    opt.rule_prefix = "EQX-WCAG-";
    opt.max_pages = 1;
    Result r = validate_file(path, opt);
    EXPECT(!r.processed);
    EXPECT_EQ(r.error, std::string("too many pages: 2, the limit is 1"));
    EXPECT(r.findings.sorted().empty());
    for (RuleIndex i = 0; i < Catalog::get().size(); ++i) EXPECT(!r.evaluated[i]);
    opt.max_pages = 2;
    EXPECT(validate_file(path, opt).processed);
}
