// A Figure whose standard type is remapped in the RoleMap (/Figure -> /P): veraPDF checks it as P and asks for no
// alternative text; PAC takes the standard /S as is and fails a Figure without /Alt and /ActualText. The merged rule
// EQX-UA1-7.3-1 reports it through PAC's definition only (detail "PAC: ...", non_pac_occurrences unchanged).
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <string_view>

#include "engine/validate.h"
#include "rules/catalog.h"
#include "unit.h"

using namespace eqx;

namespace {

std::string write_remapped_figure_pdf(bool with_alt_on_second) {
    static int counter = 0;
    QPDF q;
    q.emptyPDF();
    Obj page = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] >>"));
    q.addPage(page, true);
    Obj root = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /StructTreeRoot /RoleMap << /Figure /P >> >>"));
    Obj doc = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /StructElem /S /Document >>"));
    doc.replaceKey("/P", root);
    Obj fig1 = q.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /StructElem /S /Figure /K [] >>"));
    Obj fig2 = q.makeIndirectObject(QPDFObjectHandle::parse(
        with_alt_on_second ? "<< /Type /StructElem /S /Figure /K [] /Alt (A chart) >>" : "<< /Type /StructElem /S /Figure /K [] >>"));
    fig1.replaceKey("/P", doc);
    fig2.replaceKey("/P", doc);
    Obj kids = QPDFObjectHandle::newArray();
    kids.appendItem(fig1);
    kids.appendItem(fig2);
    doc.replaceKey("/K", kids);
    root.replaceKey("/K", doc);
    q.getRoot().replaceKey("/StructTreeRoot", root);
    q.getRoot().replaceKey("/MarkInfo", QPDFObjectHandle::parse("<< /Marked true >>"));
    q.getRoot().replaceKey("/Lang", QPDFObjectHandle::newUnicodeString("en"));
    auto path = std::filesystem::temp_directory_path() /
                ("eqx_figure_remap_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".pdf");
    QPDFWriter w(q, path.string().c_str());
    w.setStaticID(true);
    w.write();
    return path.string();
}

const FindingRecord* finding(const Result& r, std::string_view id) {
    for (const FindingRecord* f : r.findings.sorted())
        if (Catalog::get().rule(f->rule).id == id) return f;
    return nullptr;
}

}  // namespace

EQX_TEST(pac_figure_remapped_without_alt_fails_through_pac_only) {
    const std::string path = write_remapped_figure_pdf(true);
    Result r = validate_file(path, Options{});
    std::filesystem::remove(path);
    EXPECT(r.processed);
    const FindingRecord* remap = finding(r, "EQX-UA1-7.1-7");
    EXPECT(remap != nullptr);   // the remapping itself (veraPDF definition)
    const FindingRecord* alt = finding(r, "EQX-UA1-7.3-1");
    EXPECT(alt != nullptr);
    if (!alt) return;
    EXPECT_EQ(alt->occurrences, std::uint64_t(1));         // only the Figure without Alt
    EXPECT_EQ(alt->non_pac_occurrences, std::uint64_t(0)); // not a failure by veraPDF's definition
    EXPECT(!alt->locations.empty() && alt->locations.front().detail.rfind("PAC: Figure without Alt", 0) == 0);
}
