// The input file of one validation: opened once, hashed and handed to qpdf as its input source.
//
// qpdf's own file source reads through stdio: every seek is an lseek system call and discards the 4 KiB buffer
// (1800 lseek calls for a 32 KiB document, 1.3 million for a 2000-page one). This source keeps the position itself
// and reads with pread: a file of up to kWholeFile bytes is read once and serves the hash, the header, the
// startxref test and qpdf from the same bytes; a larger file is read through a window that grows while the reads
// are sequential. Positions, end-of-file behaviour, last_offset and error messages are those of
// qpdf's FileInputSource, so qpdf sees the same file at the same offsets.
#pragma once

#include <qpdf/InputSource.hh>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace eqx {

class SourceFile final : public InputSource {
public:
    // A file that fits is held in memory completely (never more than the 1 MiB block the hash used to read through).
    static constexpr std::size_t kWholeFile = std::size_t(1) << 20;

    // nullptr unless `path` is a regular file that can be opened and read: the caller then reads the path the
    // way it did before (stdio), which keeps every message about unreadable input as it was.
    static std::shared_ptr<SourceFile> open(const std::string& path);
    ~SourceFile() override;

    std::uint64_t size() const { return size_; }
    // SHA-256 of the file, lower-case hex.
    std::string sha256_hex();
    // Up to n bytes at an offset (fewer at the end of the file; empty when nothing can be read there).
    std::string bytes_at(std::uint64_t offset, std::size_t n);
    // The whole file.
    std::string read_all() { return bytes_at(0, static_cast<std::size_t>(size_)); }
    // True when the bytes of `needle` occur in the file. The file is searched block by block, not loaded.
    bool contains(std::string_view needle);

    qpdf_offset_t findAndSkipNextEOL() override;
    std::string const& getName() const override { return name_; }
    qpdf_offset_t tell() override { return pos_; }
    void seek(qpdf_offset_t offset, int whence) override;
    void rewind() override { pos_ = 0; }
    size_t read(char* buffer, size_t length) override;
    void unreadCh(char ch) override;

private:
    SourceFile() = default;
    // Bytes at `offset` straight from the file (not through the window); false on a read error.
    bool read_at(std::uint64_t offset, char* out, std::size_t n, std::size_t* got);
    bool fill(std::uint64_t offset);

    std::string name_;
    int fd_ = -1;
    std::uint64_t size_ = 0;
    qpdf_offset_t pos_ = 0;
    // [buf_start_, buf_start_ + buf_len_) of the file; the whole file when whole_.
    std::unique_ptr<char[]> buf_;
    std::size_t buf_cap_ = 0;
    std::uint64_t buf_start_ = 0;
    std::size_t buf_len_ = 0;
    std::size_t next_fill_ = 0;   // bytes the next window reads
    bool whole_ = false;
};

}  // namespace eqx
