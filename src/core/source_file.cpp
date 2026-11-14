#include "core/source_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <qpdf/QPDFExc.hh>
#include <qpdf/QUtil.hh>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "util/sha256.h"

namespace eqx {

namespace {

// Window of a file that is not held completely: it starts small (objects are read in any order, a few hundred bytes
// each) and doubles while the reads continue where the window ended (stream data, cross-reference tables).
constexpr std::size_t kWindowMin = std::size_t(1) << 13;
constexpr std::size_t kWindowMax = std::size_t(1) << 18;
// The window starts this far before the position asked for: the tokenizer steps back a few bytes.
constexpr std::size_t kWindowBack = 256;

}  // namespace

std::shared_ptr<SourceFile> SourceFile::open(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        ::close(fd);
        return nullptr;
    }
    std::shared_ptr<SourceFile> f(new SourceFile());
    f->name_ = path;
    f->fd_ = fd;
    f->size_ = static_cast<std::uint64_t>(st.st_size);
    if (f->size_ <= kWholeFile) {
        f->buf_cap_ = static_cast<std::size_t>(f->size_);
        f->buf_.reset(new char[f->buf_cap_ + 1]);
        std::size_t got = 0;
        if (!f->read_at(0, f->buf_.get(), f->buf_cap_, &got) || got != f->buf_cap_) return nullptr;   // not what fstat said
        f->buf_len_ = got;
        f->whole_ = true;
    } else {
        f->next_fill_ = kWindowMin;
    }
    return f;
}

SourceFile::~SourceFile() {
    if (fd_ >= 0) ::close(fd_);
}

bool SourceFile::read_at(std::uint64_t offset, char* out, std::size_t n, std::size_t* got) {
    std::size_t done = 0;
    while (done < n) {
        const ssize_t r = ::pread(fd_, out + done, n - done, static_cast<off_t>(offset + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            *got = done;
            return false;
        }
        if (r == 0) break;   // end of the file
        done += static_cast<std::size_t>(r);
    }
    *got = done;
    return true;
}

// Loads a window that starts at or shortly before `offset`. False on a read error.
bool SourceFile::fill(std::uint64_t offset) {
    if (!buf_) {
        buf_cap_ = kWindowMax;
        buf_.reset(new char[buf_cap_]);
    }
    const bool sequential = buf_len_ > 0 && offset == buf_start_ + buf_len_;
    const std::size_t want = next_fill_;
    next_fill_ = sequential ? std::min(next_fill_ * 2, kWindowMax) : kWindowMin;
    std::uint64_t start = offset;
    if (!sequential) start = offset < kWindowBack ? 0 : offset - kWindowBack;
    std::size_t got = 0;
    const bool ok = read_at(start, buf_.get(), want, &got);
    buf_start_ = start;
    buf_len_ = got;
    return ok;
}

size_t SourceFile::read(char* buffer, size_t length) {
    // As FileInputSource::read: last_offset is where the read starts; a read at or behind the end moves to the end.
    last_offset = pos_;
    if (length == 0) return 0;
    const std::uint64_t pos = static_cast<std::uint64_t>(pos_);
    if (pos >= size_) {
        pos_ = static_cast<qpdf_offset_t>(size_);
        last_offset = pos_;
        return 0;
    }
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - pos));
    std::size_t done = 0;
    bool failed = false;
    while (done < want) {
        const std::uint64_t at = pos + done;
        if (at >= buf_start_ && at < buf_start_ + buf_len_) {
            const std::size_t n = std::min<std::uint64_t>(want - done, buf_start_ + buf_len_ - at);
            std::memcpy(buffer + done, buf_.get() + (at - buf_start_), n);
            done += n;
            continue;
        }
        if (want - done >= kWindowMax) {   // larger than a window: straight into the caller's buffer
            std::size_t got = 0;
            failed = !read_at(at, buffer + done, want - done, &got);
            done += got;
            break;
        }
        if (!fill(at)) {
            failed = true;
            break;
        }
        if (at < buf_start_ || at >= buf_start_ + buf_len_) break;   // the file became shorter
    }
    if (done == 0) {
        if (failed)
            throw QPDFExc(qpdf_e_system, name_, "", last_offset, std::string("read ") + std::to_string(length) + " bytes");
        pos_ = static_cast<qpdf_offset_t>(size_);
        last_offset = pos_;
        return 0;
    }
    pos_ = static_cast<qpdf_offset_t>(pos + done);
    return done;
}

void SourceFile::seek(qpdf_offset_t offset, int whence) {
    // As fseeko: a position before the start is refused, one behind the end is accepted when the file system accepts
    // it (the kernel decides, as it did for stdio); the message is FileInputSource's.
    qpdf_offset_t target = 0;
    bool ok = true;
    switch (whence) {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: ok = !__builtin_add_overflow(pos_, offset, &target); break;
        case SEEK_END: ok = !__builtin_add_overflow(static_cast<qpdf_offset_t>(size_), offset, &target); break;
        default: ok = false;
    }
    if (ok && target < 0) ok = false;
    if (!ok) errno = EINVAL;
    else if (static_cast<std::uint64_t>(target) > size_ && ::lseek(fd_, static_cast<off_t>(target), SEEK_SET) == -1) ok = false;
    if (!ok)
        QUtil::throw_system_error(std::string("seek to ") + name_ + ", offset " + std::to_string(offset) + " (" +
                                  std::to_string(whence) + ")");
    pos_ = target;
}

void SourceFile::unreadCh(char) {
    if (pos_ > 0) --pos_;
}

// FileInputSource::findAndSkipNextEOL, statement by statement: the position and last_offset it leaves are part of
// what qpdf's recovery code sees.
qpdf_offset_t SourceFile::findAndSkipNextEOL() {
    qpdf_offset_t result = 0;
    bool done = false;
    char buf[10240];
    while (!done) {
        const qpdf_offset_t cur_offset = pos_;
        const size_t len = this->read(buf, sizeof(buf));
        if (len == 0) {
            done = true;
            result = this->tell();
        } else {
            char* p1 = static_cast<char*>(std::memchr(buf, '\r', len));
            char* p2 = static_cast<char*>(std::memchr(buf, '\n', len));
            char* p = (p1 && p2) ? std::min(p1, p2) : p1 ? p1 : p2;
            if (p) {
                result = cur_offset + (p - buf);
                // We found \r or \n. Keep reading until we get past \r and \n characters.
                this->seek(result + 1, SEEK_SET);
                char ch;
                while (!done) {
                    if (this->read(&ch, 1) == 0) {
                        done = true;
                    } else if (!((ch == '\r') || (ch == '\n'))) {
                        this->unreadCh(ch);
                        done = true;
                    }
                }
            }
        }
    }
    return result;
}

std::string SourceFile::sha256_hex() {
    Sha256 s;
    if (whole_) {
        s.update(buf_.get(), buf_len_);
        return s.hex();
    }
    constexpr std::size_t kChunk = std::size_t(1) << 16;
    const std::unique_ptr<char[]> chunk(new char[kChunk]);
    for (std::uint64_t at = 0;;) {
        std::size_t got = 0;
        read_at(at, chunk.get(), kChunk, &got);
        if (got == 0) break;
        s.update(chunk.get(), got);
        at += got;
    }
    return s.hex();
}

bool SourceFile::contains(std::string_view needle) {
    if (needle.empty()) return true;
    if (whole_) return std::string_view(buf_.get(), buf_len_).find(needle) != std::string_view::npos;
    constexpr std::size_t kChunk = std::size_t(1) << 16;
    const std::size_t keep = needle.size() - 1;   // the end of a block is searched again with the next one
    const std::unique_ptr<char[]> chunk(new char[keep + kChunk]);
    std::size_t have = 0;
    for (std::uint64_t at = 0;;) {
        std::size_t got = 0;
        read_at(at, chunk.get() + have, kChunk, &got);
        if (got == 0) return false;
        at += got;
        have += got;
        if (std::string_view(chunk.get(), have).find(needle) != std::string_view::npos) return true;
        const std::size_t tail = std::min(have, keep);
        std::memmove(chunk.get(), chunk.get() + have - tail, tail);
        have = tail;
    }
}

std::string SourceFile::bytes_at(std::uint64_t offset, std::size_t n) {
    if (offset >= size_) return {};
    n = static_cast<std::size_t>(std::min<std::uint64_t>(n, size_ - offset));
    if (whole_) return std::string(buf_.get() + offset, n);
    std::string out(n, '\0');
    std::size_t got = 0;
    read_at(offset, out.data(), n, &got);
    out.resize(got);
    return out;
}

}  // namespace eqx
