// Resource identifiers. Kept in one place so the .rc and the C++ that loads
// them cannot drift apart.
#pragma once

#define IDI_APPICON             101
#define IDI_TRAYICON            102

// Menu command ids.
//
// The wallpaper list is dynamic, so it takes a range rather than a constant:
// the nth library item is IDM_WALLPAPER_FIRST + n. 4096 entries is far past
// what anyone will import and still leaves the rest of the WM_COMMAND space
// clear.
#define IDM_QUIT                40001
#define IDM_ADD_VIDEO           40002
#define IDM_REMOVE_CURRENT      40003
#define IDM_REVEAL_LIBRARY      40004
#define IDM_PAUSE_ON_BATTERY    40005
#define IDM_START_AT_LOGIN      40006
#define IDM_PROCEDURAL          40007
#define IDM_ABOUT               40008

#define IDM_PRESET_FIRST        40100   // + preset index
#define IDM_PRESET_LAST         40119

#define IDM_FIT_FIRST           40120   // + FitMode raw value
#define IDM_FIT_LAST            40129

#define IDM_WALLPAPER_FIRST     41000
#define IDM_WALLPAPER_LAST      45095

#define VER_FILEVERSION         1,0,1,0
#define VER_FILEVERSION_STR     "1.0.1.0"
#define VER_PRODUCTVERSION      1,0,1,0
#define VER_PRODUCTVERSION_STR  "1.0.1"
