// The parsed document: one qpdf instance per file, opened once and shared by every rule module.
#pragma once

#include <qpdf/QPDF.hh>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/cos.h"

namespace eqx {

struct Page {
    int index = 0;            // 0-based; reports use index + 1
    Obj dict;
    ObjRef ref;
    Obj resources;            // inherited /Resources (null handle when none)
    Rect media_box{0, 0, 612, 792};
    Rect crop_box{0, 0, 612, 792};  // inherited /CropBox, default MediaBox
    bool has_crop_box = false;
    int rotate = 0;
};

enum class OpenStatus {
    Ok,
    NotFound,            // file cannot be read
    PasswordRequired,    // encrypted with a non-empty user password (not processable)
    Unreadable,          // qpdf could not parse the file at all
};

class Document {
public:
    // Opens and parses the file structure. On failure returns nullptr and sets status/error.
    // `recover`: read through the recovered view from the start (validate_file, after qpdf rebuilt the table only late).
    static std::unique_ptr<Document> open(const std::string& path, OpenStatus* status, std::string* error, bool recover = false);

    QPDF& qpdf() { return *qpdf_; }
    const std::string& path() const { return path_; }
    std::uint64_t file_size() const { return file_size_; }

    Obj trailer() const { return trailer_; }
    Obj catalog() const { return catalog_; }
    Obj info() const { return info_; }

    // First bytes of the file (up to 4 KiB) and the header line ("%PDF-1.7"), as veraPDF reads it.
    const std::string& file_head() const { return head_; }
    const std::string& header() const { return header_; }
    // Version from the header ("1.7", "2.0"); empty when no header was found.
    const std::string& header_version() const { return header_version_; }
    // Catalog /Version overrides the header when larger (ISO 32000 7.7.2).
    std::string effective_version() const;

    bool encrypted() const { return encrypted_; }
    Obj encryption_dict() const { return encrypt_; }

    const std::vector<Page>& pages() const { return pages_; }
    // 0-based page index of a page object, -1 when the object is not a page of the page tree.
    int page_index(const ObjRef& page) const;

    // All warnings qpdf issued so far while reading (damaged xref, recovered objects, ...). qpdf's own list is
    // cleared on every read, so they are accumulated here.
    const std::vector<std::string>& warnings();
    // qpdf could not use the file's cross-reference table.
    bool table_rebuilt();
    // The rules read a recovered view of a damaged file (recover.h).
    bool recovered() const { return recovered_; }

private:
    Document() = default;
    void load_pages();
    bool read_recovered(bool have_first_reading);
    template <class ReadModel>
    void read_hidden_objects(ReadModel&& read_model);

    std::string image_;   // the recovered view; qpdf_ reads from it. Declared first: it outlives both instances.
    std::unique_ptr<QPDF> retired_;
    std::unique_ptr<QPDF> qpdf_;
    std::vector<std::string> warnings_;
    std::string path_;
    std::uint64_t file_size_ = 0;
    std::string head_, header_, header_version_;
    Obj trailer_, catalog_, info_, encrypt_;
    bool encrypted_ = false;
    bool recovered_ = false;
    std::vector<Page> pages_;
    std::unordered_map<std::uint64_t, int> page_by_ref_;
};

}  // namespace eqx
