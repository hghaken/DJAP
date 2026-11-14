// SPDX-License-Identifier: GPL-3.0-or-later
// appstate_test: the plug-in's own settings file starts once as a copy of the program's (startPluginSettings) and is
// never overwritten after that. In a scratch folder in %TEMP%, never the user's settings.
#include "appstate.h"
#include "check.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace ap;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("djap-appstate-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path program = dir / "DJAP.ini", plugin = dir / "sub" / "DJAP-Plugin.ini";

    // no program file: nothing to start from, nothing made
    CHECK(!startPluginSettings(plugin, program));
    CHECK(!fs::exists(plugin));

    // the first start: a copy, its folder made
    write(program, "[Preset Mine]\nTarget=-12\n");
    CHECK(startPluginSettings(plugin, program));
    CHECK(read(plugin) == "[Preset Mine]\nTarget=-12\n");

    // from then on each keeps its own: a later program change does not reach the plug-in's file
    write(program, "[Preset Mine]\nTarget=-10\n");
    CHECK(!startPluginSettings(plugin, program));
    CHECK(read(plugin) == "[Preset Mine]\nTarget=-12\n");
    write(plugin, "[Preset Other]\nTarget=-16\n");
    CHECK(!startPluginSettings(plugin, program));
    CHECK(read(plugin) == "[Preset Other]\nTarget=-16\n");
    CHECK(read(program) == "[Preset Mine]\nTarget=-10\n");

    // no path (no AppData folder): nothing
    CHECK(!startPluginSettings(fs::path(), program));

    std::printf("-- the plug-in's file: copied once, then left alone\n");
    fs::remove_all(dir, ec);
    return test::finish("appstate_test");
}
