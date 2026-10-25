// Where the program's own windows and message boxes appear (the client): centred on the window they belong to, not in the
// middle of the screen, and kept inside that window's monitor work area.
#pragma once

#include <windows.h>

namespace ap::placement {

// Moves the top-level window `wnd` (keeping its size) so that its centre is on the centre of `owner` - also when the
// owner is minimised or hidden in the notification area (its normal position counts) - within the work area of the
// owner's monitor. Without an owner nothing moves.
void centreOn(HWND wnd, HWND owner);

// MessageBoxW, centred on `owner` the same way (a CBT hook moves the box when it is first activated).
int messageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type);

} // namespace ap::placement
