// SPDX-License-Identifier: GPL-3.0-or-later
// Program-wide helpers shared by the GUI and the command line: where the settings live, text conversion.
#pragma once

#include <filesystem>
#include <string>

namespace ap {

// Folder of the running program.
std::filesystem::path programDir();

// The settings file (docs/design.md section 8): DJAP.ini next to the program; when there is none there but
// one in %APPDATA%\DJAP\, that one; for a first save, next to the program when that folder is writable,
// else in %APPDATA%\DJAP\ (created when needed). Settings of the program's old name (AudioProcessor.ini next to it
// or in %APPDATA%\AudioProcessor\) are read when there are no new ones.
std::filesystem::path settingsFile();
// The GUI at start: copies AudioProcessor.ini (next to the program, and in %APPDATA%\AudioProcessor\) to DJAP.ini in
// the same place, once, when there is no DJAP.ini yet; the old files stay. (Not the command line: it writes nothing.)
void adoptOldSettings();

// The plug-in's own settings file (client, 2026-11-14: "make the Windows and VST settings independent of each
// other"): %APPDATA%\DJAP\DJAP-Plugin.ini - its presets, EQs and sounds, each instance's choices, its windows' places,
// its tooltips and update check. The first time there is none it starts as a copy of the program's settings file in
// %APPDATA%\DJAP\ (startPluginSettings), so the own presets, EQs and sounds made so far are in both; from then on the
// program and the plug-in each change only their own. Empty when Windows names no AppData folder.
std::filesystem::path pluginSettingsFile();
// That start: copies `program` to `plugin` when `plugin` does not exist and `program` does. True when it copied.
bool startPluginSettings(const std::filesystem::path& plugin, const std::filesystem::path& program);

// Start with Windows (docs/design.md section 8): the value "DJAP" under HKCU\Software\Microsoft\Windows\
// CurrentVersion\Run starts the program with --autostart when the user logs on. On = the value is there, names this
// program (the setup writes the same) and is not switched off in Windows' list of startup apps.
bool startsWithWindows();
// Writes the value for this program (and clears a "disabled" mark of Windows' startup apps list), or removes it;
// false and `err` when the registry refuses.
bool setStartWithWindows(bool on, std::string& err);

// UTF-16 (Windows) <-> UTF-8 (settings, preset names).
std::string toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);

// UTF-8 names in the user's alphabetical order, ignoring case, numbers as numbers ("Line 2" before "Line 10").
bool lessIgnoringCase(const std::string& a, const std::string& b);

} // namespace ap
