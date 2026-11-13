// What the engine did not read. Every internal limit or give-up that leaves part of a document unseen and has no
// more specific report calls note_unread(); validate_file() then marks every rule incomplete, so a limit can never
// turn into a pass. Per thread: one file is processed by one thread (ARCHITECTURE.md section 3).
#pragma once

#include <string>
#include <vector>

namespace eqx {

void note_unread(std::string what);
// The notes since the last call (at most a few dozen distinct ones), and clears them.
std::vector<std::string> take_unread();

}  // namespace eqx
