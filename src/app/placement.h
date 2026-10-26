// Where the program's own windows and message boxes appear. A message box (and a small prompt) is centred on the
// window it belongs to (the client), kept inside that window's monitor work area. A window remembers its place
// (client, 2026-10-26: "when no position is known: centre it in the middle of the screen; after that remember every
// change of every window - position and size - and write it into the ini file").
#pragma once

#include "settings.h"

#include <windows.h>

#include <functional>
#include <map>
#include <string>

namespace ap::placement {

// Moves the top-level window `wnd` (keeping its size) so that its centre is on the centre of `owner` - also when the
// owner is minimised or hidden in the notification area (its normal position counts) - within the work area of the
// owner's monitor. Without an owner nothing moves.
void centreOn(HWND wnd, HWND owner);

// MessageBoxW, centred on `owner` the same way (a CBT hook moves the box when it is first activated).
int messageBox(HWND owner, const wchar_t* text, const wchar_t* caption, UINT type);

// The remembered places: the program sets them once, before its first window (its Settings' windows and the
// function that saves the settings file). Without them (the plug-in: it never writes the program's settings) a
// window is only centred on the screen and nothing is remembered.
void setStore(std::map<std::string, WindowPlace>* places, std::function<void()> save);

// Called once after creating a window, with its name (the ini key: Main, Setup, Loudness, Colour, Analysis,
// Spectrum, About) and its owner: the window goes to its remembered place - the size too when it can be resized -
// if that place is still on a monitor; else it is centred on the screen (the owner's monitor, or the main one).
// From then on every move, resize, maximise or restore is remembered and saved (a drag once, when it ends).
void remember(HWND wnd, const char* name, HWND owner);

// The ShowWindow command that shows the window as remembered: restored when minimised, maximised the first time
// when it was maximised last, else normal (a window already shown stays as it is).
int showCommand(HWND wnd);

} // namespace ap::placement
