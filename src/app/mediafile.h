// SPDX-License-Identifier: GPL-3.0-or-later
// Reads an audio file for the command line: WAV with the core's reader, anything else (MP3, AAC / M4A, WMA, FLAC
// on Windows 10+) decoded by Windows Media Foundation into 32-bit float at the file's own rate. A mono file
// becomes two equal channels (as it is played).
#pragma once

#include "wavfile.h"

#include <filesystem>
#include <string>

namespace ap {

bool readAudioFile(const std::filesystem::path& file, WavData& out, std::string& error);

} // namespace ap
