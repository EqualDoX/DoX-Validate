#include "core/document.h"

#include <qpdf/QPDFExc.hh>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

#include "core/recover.h"
#include "core/source_file.h"
#include "util/unread.h"

namespace eqx {

namespace {

// The last 1024 bytes hold the startxref keyword (ISO 32000 7.5.5). qpdf looks a little further and reconstructs without one,
// so a file with more behind its end than that is reported here: veraPDF refuses such a file outright.
std::string read_tail(const std::string& path, std::uint64_t size) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    const std::uint64_t n = std::min<std::uint64_t>(size, 1024);
    in.seekg(static_cast<std::streamoff>(size - n));
    std::string buf(static_cast<std::size_t>(n), '\0');
    in.read(buf.data(), static_cast<std::streamsize>(n));
    buf.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return buf;
}

std::string read_head(const std::string& path, std::uint64_t* size) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    *size = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::string buf(4096, '\0');
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(in.gcount()));
    return buf;
}


}  // namespace

bool Document::table_rebuilt() {
    for (const std::string& w : warnings())
        if (w.find("reconstruct cross-reference table") != std::string::npos) return true;
    return false;
}

// qpdf's reconstruction keeps only what its scan and any readable cross-reference stream say: objects in object streams
// and the object streams themselves can be lost, so that a structure tree or a font disappears. The rules then read the
// file through an image with a complete table; the warnings stay, so the verdict stays open.
bool Document::read_recovered(bool have_first_reading) {
    recovered_ = true;
    std::string bytes;
    if (source_) {
        bytes = source_->read_all();
    } else {
        std::ifstream in(path_, std::ios::binary);
        if (!in) return false;
        bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    QPDFObjectHandle trailer = QPDFObjectHandle::newNull();
    if (have_first_reading) trailer = qpdf_->getTrailer();
    warnings();  // keeps what the first reading reported
    std::string image = recovered_image(bytes, trailer);
    if (image.empty()) return false;
    auto next = std::make_unique<QPDF>();
    next->setSuppressWarnings(true);
    next->processMemoryFile(path_.c_str(), image.data(), image.size());
    image_ = std::move(image);
    next.swap(qpdf_);
    retired_ = std::move(next);
    warnings_.push_back("read through a rebuilt table: reconstruct cross-reference table");
    return true;
}

// Reads the bytes and looks for objects qpdf could not see in a file without damage. Only with a chain of cross-reference
// sections or an /XRefStm: other files cannot hide anything.
template <class ReadModel>
void Document::read_hidden_objects(ReadModel&& read_model) {
    QPDFObjectHandle trailer = qpdf_->getTrailer();
    if (!trailer.hasKey("/XRefStm") && !trailer.hasKey("/Prev")) return;
    std::string bytes;
    if (source_) {
        // Most files with a /Prev chain (every linearized file) have no /XRefStm: the file is searched, not loaded.
        if (!source_->contains("/XRefStm")) return;
        bytes = source_->read_all();
    } else {
        std::ifstream in(path_, std::ios::binary);
        if (!in) return;
        bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.find("/XRefStm") == std::string::npos) return;
    }
    std::string image = hidden_objects_image(bytes, trailer, qpdf_->getXRefTable());
    if (image.empty()) return;
    warnings();
    auto next = std::make_unique<QPDF>();
    next->setSuppressWarnings(true);
    try {
        next->processMemoryFile(path_.c_str(), image.data(), image.size());
        image_ = std::move(image);
        next.swap(qpdf_);
        retired_ = std::move(next);
        read_model();
    } catch (std::exception& e) {
        note_unread(std::string("objects hidden in a hybrid-reference file could not be read: ") + e.what());
        if (retired_) {
            qpdf_.swap(retired_);
            read_model();
        }
    }
}

std::unique_ptr<Document> Document::open(const std::string& path, OpenStatus* status, std::string* error, bool recover,
                                         std::shared_ptr<SourceFile> source) {
    std::unique_ptr<Document> d(new Document());
    d->path_ = path;
    d->source_ = source ? std::move(source) : SourceFile::open(path);
    if (d->source_) {
        d->file_size_ = d->source_->size();
        d->head_ = d->source_->bytes_at(0, 4096);
    } else {
        d->head_ = read_head(path, &d->file_size_);
    }
    if (!d->source_ && d->file_size_ == 0 && d->head_.empty()) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            *status = OpenStatus::NotFound;
            if (error) *error = "cannot read file";
            return nullptr;
        }
        std::fclose(f);
    }
    // Header line: "%PDF-" within the first 1024 bytes, up to the first EOL (veraPDF/PDF syntax).
    auto pos = d->head_.find("%PDF-");
    if (pos != std::string::npos && pos < 1024) {
        auto end = d->head_.find_first_of("\r\n", pos);
        d->header_ = d->head_.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        std::string v = d->header_.substr(5);
        std::size_t n = 0;
        while (n < v.size() && (std::isdigit(static_cast<unsigned char>(v[n])) || v[n] == '.')) ++n;
        d->header_version_ = v.substr(0, n);
    }
    d->qpdf_ = std::make_unique<QPDF>();
    d->qpdf_->setSuppressWarnings(true);
    bool opened = true;
    std::string open_error;
    try {
        if (d->source_) d->qpdf_->processInputSource(d->source_, nullptr);
        else d->qpdf_->processFile(path.c_str(), nullptr);
    } catch (QPDFExc& e) {
        if (e.getErrorCode() == qpdf_e_password) {
            *status = OpenStatus::PasswordRequired;
            if (error) *error = e.what();
            return nullptr;
        }
        opened = false;
        open_error = e.what();
    } catch (std::exception& e) {
        opened = false;
        open_error = e.what();
    }
    auto read_model = [&d]() {
        d->pages_.clear();
        d->page_by_ref_.clear();
        d->trailer_ = d->qpdf_->getTrailer();
        d->catalog_ = d->qpdf_->getRoot();
        d->info_ = cos::get(d->trailer_, "Info");
        d->encrypted_ = d->qpdf_->isEncrypted();
        d->encrypt_ = cos::get(d->trailer_, "Encrypt");
        d->load_pages();
        // qpdf >= 12.3 refuses a file without pages itself, 12.2 does not; no rule can pass on no content
        if (d->pages_.empty()) throw std::runtime_error("no pages found");
    };
    if (opened) {
        try {
            read_model();
        } catch (std::exception& e) {
            opened = false;
            open_error = e.what();
        }
    }
    if (!opened) {
        // What qpdf could not open may still open through a complete table (a Pages tree inside an object stream, say).
        bool usable = false;
        try {
            if (d->read_recovered(false) && (read_model(), true)) usable = true;
        } catch (std::exception&) {
        }
        if (!usable) {
            *status = OpenStatus::Unreadable;
            if (error) *error = open_error;
            return nullptr;
        }
        d->warnings_.push_back(std::string("the file opens only through a rebuilt table (reconstruct cross-reference table): ") + open_error);
    } else if (recover || d->table_rebuilt()) {
        bool replaced = false;
        try {
            replaced = d->read_recovered(true);
            if (replaced) read_model();
        } catch (std::exception& e) {
            d->warnings_.push_back(std::string("recovered view not usable: ") + e.what());
            replaced = false;
        }
        if (!replaced) {
            // qpdf's own reconstruction stays what the rules read.
            if (d->retired_) d->qpdf_.swap(d->retired_);
            d->image_.clear();
            try {
                read_model();
            } catch (std::exception& e) {
                *status = OpenStatus::Unreadable;
                if (error) *error = e.what();
                return nullptr;
            }
        }
    }
    if (!d->recovered_) d->read_hidden_objects(read_model);
    d->retired_.reset();
    d->tail_ = d->source_ ? d->source_->bytes_at(d->file_size_ - std::min<std::uint64_t>(d->file_size_, 1024), 1024)
                          : read_tail(path, d->file_size_);
    if (d->tail_.find("startxref") == std::string::npos) note_unread("no startxref in the last 1024 bytes of the file");
    *status = OpenStatus::Ok;
    return d;
}

std::string Document::effective_version() const {
    auto v = cos::name(catalog_, "Version");
    if (v && !header_version_.empty() && *v > header_version_) return *v;
    return header_version_;
}

int Document::page_index(const ObjRef& page) const {
    auto it = page_by_ref_.find(page.key());
    return it == page_by_ref_.end() ? -1 : it->second;
}

const std::vector<std::string>& Document::warnings() {
    for (auto& w : qpdf_->getWarnings()) warnings_.push_back(w.what());
    return warnings_;
}

void Document::load_pages() {
    struct Inherit {
        Obj resources;
        std::optional<Rect> media, crop;
        int rotate = 0;
    };
    // Explicit stack: page trees nest without a limit (a page dropped here would pass unread). A node met again is
    // skipped, so a cycle ends.
    struct Level {
        std::vector<Obj> kids;
        std::size_t next;
        Inherit inh;
    };
    std::vector<Level> stack;
    std::unordered_set<std::uint64_t> seen;
    auto visit = [&](const Obj& node, Inherit inh) {
        if (!node.isDictionary()) return;
        if (node.isIndirect() && !seen.insert(ref_of(node).key()).second) return;
        if (cos::has(node, "Resources")) inh.resources = cos::get(node, "Resources");
        if (auto r = cos::rect(cos::get(node, "MediaBox"))) inh.media = r;
        if (auto r = cos::rect(cos::get(node, "CropBox"))) inh.crop = r;
        if (auto r = cos::integer(node, "Rotate")) inh.rotate = static_cast<int>(*r);
        Obj kids = cos::get(node, "Kids");
        auto type = cos::name(node, "Type");
        if (kids.isArray() && (!type || *type != "Page")) {
            stack.push_back({cos::array_items(kids), 0, std::move(inh)});
            return;
        }
        Page p;
        p.index = static_cast<int>(pages_.size());
        p.dict = node;
        p.ref = ref_of(node);
        p.resources = inh.resources ? inh.resources : Obj::newNull();
        if (inh.media) p.media_box = *inh.media;
        p.crop_box = inh.crop ? *inh.crop : p.media_box;
        p.has_crop_box = inh.crop.has_value();
        p.rotate = inh.rotate;
        if (p.ref.valid()) page_by_ref_.emplace(p.ref.key(), p.index);
        pages_.push_back(std::move(p));
    };
    visit(cos::get(catalog_, "Pages"), Inherit{});
    while (!stack.empty()) {
        Level& top = stack.back();
        if (top.next >= top.kids.size()) {
            stack.pop_back();
            continue;
        }
        Obj kid = top.kids[top.next++];
        Inherit inh = top.inh;  // copies: visit may grow the stack
        visit(kid, std::move(inh));
    }
}

}  // namespace eqx
