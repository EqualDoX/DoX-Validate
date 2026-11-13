#include "util/unread.h"

#include <algorithm>

namespace eqx {

namespace {
constexpr std::size_t kMaxNotes = 32;
thread_local std::vector<std::string> notes;
}  // namespace

void note_unread(std::string what) {
    if (notes.size() >= kMaxNotes) return;   // the first note already decides; the rest is only explanation
    if (std::find(notes.begin(), notes.end(), what) == notes.end()) notes.push_back(std::move(what));
}

std::vector<std::string> take_unread() {
    std::vector<std::string> out;
    out.swap(notes);
    return out;
}

}  // namespace eqx
