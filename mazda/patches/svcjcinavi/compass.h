// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Load-time entry point for the svcjcinavi compass hook.

#ifndef LIBPATCH_SVCJCINAVI_COMPASS_H
#define LIBPATCH_SVCJCINAVI_COMPASS_H

// Defined in compass.cpp. Arms the compass hook. Called once from the
// library constructor (main.cpp), which owns the launcher-process
// detection and the compass_always_on gate, so this module carries no
// duplicated guard of its own.
//
// It deliberately resolves nothing: at constructor time sm_svclauncher
// has not dlopen'd svcjcinavi.so yet, so neither it nor its DT_NEEDED
// libjcivbsbcmclient.so is mapped. The "are we in the nav PID" check and
// the real-implementation lookup therefore stay deferred to the first
// interposed call — arming here only keeps the libpatch.conf read off
// that path, which runs inside svcjcinavi's own D-Bus setup.
void compass_init(void);

#endif // LIBPATCH_SVCJCINAVI_COMPASS_H
