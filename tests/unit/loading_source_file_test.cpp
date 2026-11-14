// SourceFile against qpdf's FileInputSource: the same operations on the same file must give the same bytes, the
// same positions and the same last_offset, for a file held in memory and for one read through the window.
#include <qpdf/FileInputSource.hh>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/document.h"
#include "core/source_file.h"
#include "unit.h"
#include "util/sha256.h"

using namespace eqx;

namespace {

std::string temp_file(const std::string& tag, const std::string& content) {
    auto path = std::filesystem::temp_directory_path() / ("eqx_source_" + std::to_string(::getpid()) + "_" + tag);
    std::ofstream(path, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
    return path.string();
}

// Bytes with many line ends of every kind, so that findAndSkipNextEOL meets runs of them and block boundaries.
std::string test_bytes(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (char& c : s) {
        unsigned r = rng() % 100;
        c = r < 6 ? '\n' : r < 11 ? '\r' : static_cast<char>(rng() & 0xff);
    }
    return s;
}

// What an input source did in one step.
struct Step {
    std::string data;
    long long ret = 0, pos = 0, last = 0;
    bool threw = false;
    bool operator==(const Step& o) const {
        return data == o.data && ret == o.ret && pos == o.pos && last == o.last && threw == o.threw;
    }
};

Step apply(InputSource& in, int op, long long a, std::size_t n) {
    Step s;
    try {
        switch (op) {
            case 0: {
                s.data.assign(n, '\0');
                s.ret = static_cast<long long>(in.read(s.data.data(), n));
                s.data.resize(static_cast<std::size_t>(s.ret));
                break;
            }
            case 1: in.seek(a, SEEK_SET); break;
            case 2: in.seek(a, SEEK_CUR); break;
            case 3: in.seek(a, SEEK_END); break;
            case 4: s.ret = in.findAndSkipNextEOL(); break;
            case 5: {
                char ch = 0;
                s.ret = static_cast<long long>(in.read(&ch, 1));
                if (s.ret == 1) in.unreadCh(ch);
                s.data.assign(1, ch);
                break;
            }
            case 6: in.rewind(); break;
            case 7: s.data = in.readLine(n); break;
        }
    } catch (std::exception& e) {
        s.threw = true;
        s.data = e.what();
    }
    s.pos = in.tell();
    s.last = in.getLastOffset();
    return s;
}

void compare_sources(const std::string& tag, std::size_t size, unsigned seed) {
    const std::string content = test_bytes(size, seed);
    const std::string path = temp_file(tag, content);
    {
        FileInputSource reference(path.c_str());
        std::shared_ptr<SourceFile> source = SourceFile::open(path);
        EXPECT(source != nullptr);
        if (source) {
            EXPECT_EQ(source->size(), static_cast<std::uint64_t>(size));
            EXPECT(source->getName() == reference.getName());
            std::mt19937 rng(seed * 7919 + 1);
            const long long sz = static_cast<long long>(size);
            int mismatches = 0, refused = 0, read_bytes = 0, line_ends = 0;
            for (int i = 0; i < 6000 && mismatches == 0; ++i) {
                const int op = static_cast<int>(rng() % 8);
                long long a = 0;
                switch (rng() % 6) {
                    case 0: a = static_cast<long long>(rng() % static_cast<unsigned long>(sz + 1)); break;   // inside
                    case 1: a = sz - static_cast<long long>(rng() % 300); break;                             // near the end
                    case 2: a = sz + static_cast<long long>(rng() % 5000); break;                            // behind the end
                    case 3: a = -static_cast<long long>(rng() % 300); break;                                 // negative
                    case 4: a = static_cast<long long>(rng() % 200); break;                                  // near the start
                    default: a = static_cast<long long>(rng() % 20000) - 10000; break;
                }
                static const std::size_t lengths[] = {0, 1, 2, 7, 128, 129, 1000, 10240, 70000, 300000};
                const std::size_t n = lengths[rng() % (sizeof(lengths) / sizeof(lengths[0]))];
                const Step want = apply(reference, op, a, n);
                const Step got = apply(*source, op, a, n);
                refused += want.threw;
                read_bytes += op == 0 && want.ret > 0;
                line_ends += op == 4 && want.ret < sz;
                if (!(want == got)) {
                    ++mismatches;
                    eqx_test::fail(__FILE__, __LINE__,
                                   tag + ": step " + std::to_string(i) + " op " + std::to_string(op) + " arg " +
                                       std::to_string(a) + " n " + std::to_string(n) + ": ret " + std::to_string(want.ret) +
                                       "/" + std::to_string(got.ret) + " pos " + std::to_string(want.pos) + "/" +
                                       std::to_string(got.pos) + " last " + std::to_string(want.last) + "/" +
                                       std::to_string(got.last) + " threw " + std::to_string(want.threw) + "/" +
                                       std::to_string(got.threw) + (want.threw || got.threw ? " [" + want.data + "] [" + got.data + "]" : ""));
                }
            }
            // the comparison saw refused seeks, successful reads and line ends
            EXPECT(refused > 100);
            if (size > 1000) EXPECT(read_bytes > 100 && line_ends > 100);
            // the helpers of the document loader
            EXPECT(source->read_all() == content);
            EXPECT(source->bytes_at(0, 4096) == content.substr(0, 4096));
            EXPECT(source->bytes_at(size > 1024 ? size - 1024 : 0, 1024) == content.substr(size > 1024 ? size - 1024 : 0));
            EXPECT(source->bytes_at(size, 10).empty());
            Sha256 h;
            h.update(content.data(), content.size());
            EXPECT_EQ(source->sha256_hex(), h.hex());
            EXPECT_EQ(sha256_file(path), source->sha256_hex());
        }
    }
    std::filesystem::remove(path);
}

}  // namespace

EQX_TEST(source_file_matches_file_input_source_in_memory) {
    compare_sources("empty", 0, 1);
    compare_sources("one", 1, 2);
    compare_sources("small", 5000, 3);
    compare_sources("blocks", 10240 * 3, 4);
    compare_sources("limit", SourceFile::kWholeFile, 5);
}

EQX_TEST(source_file_matches_file_input_source_windowed) {
    compare_sources("window1", SourceFile::kWholeFile + 1, 6);
    compare_sources("window2", 3 * SourceFile::kWholeFile + 12345, 7);
}

EQX_TEST(source_file_contains) {
    std::string content = test_bytes(2 * SourceFile::kWholeFile, 8);
    for (char& c : content)
        if (c == '/') c = '.';
    const std::string needle = "/XRefStm";
    // across a 64 KiB block boundary, at the very end, and absent
    for (std::size_t at : {std::size_t(65536 - 3), std::size_t(3 * 65536 - 7), content.size() - needle.size()}) {
        std::string with = content;
        with.replace(at, needle.size(), needle);
        const std::string path = temp_file("contains", with);
        auto source = SourceFile::open(path);
        EXPECT(source && source->contains(needle));
        EXPECT(source && !source->contains("/XRefStn"));
        std::filesystem::remove(path);
    }
    const std::string small = temp_file("contains_small", "abc /XRefStm 12");
    auto source = SourceFile::open(small);
    EXPECT(source && source->contains(needle));
    EXPECT(source && !source->contains("/Prev"));
    std::filesystem::remove(small);
}

EQX_TEST(document_keeps_head_and_tail_of_the_file) {
    // a document of a few kilobytes: one page, and a padding comment so that head and tail differ
    std::string pdf = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    const auto object = [&](const std::string& body) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    object("<< /Type /Catalog /Pages 2 0 R >>");
    object("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>");
    pdf += "%" + std::string(6000, 'x') + "\n";
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 4\n0000000000 65535 f \n";
    for (std::size_t off : offsets) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", off);
        pdf += line;
    }
    pdf += "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    const std::string path = temp_file("doc.pdf", pdf);
    OpenStatus status;
    std::string error;
    // once through a SourceFile handed in, once opened by the document itself
    for (int pass = 0; pass < 2; ++pass) {
        auto doc = Document::open(path, &status, &error, false, pass == 0 ? SourceFile::open(path) : nullptr);
        EXPECT(doc != nullptr);
        if (!doc) continue;
        EXPECT_EQ(doc->file_size(), static_cast<std::uint64_t>(pdf.size()));
        EXPECT(doc->file_head() == pdf.substr(0, 4096));
        EXPECT(doc->file_tail() == pdf.substr(pdf.size() - 1024));
        EXPECT(doc->file_tail().find("startxref") != std::string::npos);
        EXPECT_EQ(doc->header_version(), std::string("1.7"));
        EXPECT_EQ(doc->pages().size(), std::size_t(1));
        EXPECT(doc->warnings().empty());
    }
    std::filesystem::remove(path);
}

EQX_TEST(source_file_refuses_what_is_no_regular_file) {
    EXPECT(SourceFile::open("/nonexistent/eqx/file.pdf") == nullptr);
    EXPECT(SourceFile::open(std::filesystem::temp_directory_path().string()) == nullptr);
    EXPECT(SourceFile::open("/dev/null") == nullptr);
}
