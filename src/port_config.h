// Per-game settings.
#pragma once

// File name of the original executable, looked up in the game directory at start-up.
#define GAME_EXE        "uplink.exe"
// Window / page title.
#define GAME_TITLE      "Uplink"
// Environment variable that may point at the game directory (native builds).
#define GAME_DIR_ENV    "UPLINK_DIR"
// Size of the flat guest address space. Must exceed image + peak heap (watch the "perf:" log line).
#ifndef GUEST_SIZE_MB
#define GUEST_SIZE_MB   384
#endif
