// Keeping something only you should read -- the Claude API key -- in a file.
//
// Sealed with Windows' own per-user encryption (DPAPI), so the file means
// nothing on another account or another machine, and nothing is stored in the
// clear for a stray backup or a shared folder to pass on. Raylib-free.
#pragma once

#include <string>

namespace sb::secret {

// As text fit for one line of a settings file. Empty if it could not be sealed.
std::string Seal(const std::string& plain);
// Back again. Empty if it was not sealed by this account.
std::string Open(const std::string& sealed);

}  // namespace sb::secret
