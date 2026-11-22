// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef LIBPATCH_SVCJCIMMUI_FALLBACK_H
#define LIBPATCH_SVCJCIMMUI_FALLBACK_H

namespace svcjcimmui_fallback {

// Start a background waiter that installs the activatePrevious hook once
// libjciuiasystem.so is mapped into this jciMMUI process. Safe to call
// once from the library constructor.
void start();

} // namespace svcjcimmui_fallback

#endif
