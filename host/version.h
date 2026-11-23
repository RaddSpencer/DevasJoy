// Build identification. tools\make_release.ps1 writes generated\build_version.h (ASURA_VERSION, ASURA_COMMIT) for a release build and removes it
// afterwards, so ordinary builds say "dev". Shown in the window titles, the first log line and every problem report, so a tester's report
// names the exact build.
#pragma once
#if __has_include("generated/build_version.h")
#include "generated/build_version.h"
#endif
#ifndef ASURA_VERSION
#define ASURA_VERSION "dev"
#endif
#ifndef ASURA_COMMIT
#define ASURA_COMMIT ""
#endif
// The longest file under the data folder is 114 characters (game\dlc\<content id>\DLCCOLLABO01\CookedXbox360\...xxx, 4 more while setup
// unpacks it), and the host opens files with ANSI paths limited to 259 characters, so the data folder's own path may be at most this long.
#define ASURA_MAX_ROOT_LEN 140
#define ASURA_TITLE "Asura's Wrath (recomp) " ASURA_VERSION
#define ASURA_VERSION_LINE ASURA_VERSION " (commit " ASURA_COMMIT ", built " __DATE__ ")"
