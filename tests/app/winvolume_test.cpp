// SPDX-License-Identifier: GPL-3.0-or-later
// winvolume_test: the volume of Windows' default playback device is read - never changed - as the main window does,
// and one read is cheap enough for its 15 ms timer. A computer without a playback device passes (nothing to read).
#include "check.h"
#include "winvolume.h"

#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>

using namespace ap;

int main() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // as the GUI thread
    CHECK(SUCCEEDED(co));
    {
        WindowsVolume volume;
        const bool have = volume.poll();
        if (!have) {
            std::printf("-- no default playback device: nothing to read\n");
            CHECK(!volume.valid());
        } else {
            CHECK(volume.valid());
            CHECK(volume.gain() >= 0.0 && volume.gain() <= 1.0);
            CHECK(volume.percent() >= 0.0 && volume.percent() <= 100.0);
            // the level without the mute: what the music gets again after it
            CHECK(volume.level() >= volume.gain() && volume.level() <= 1.0);
            CHECK(volume.muted() ? volume.gain() == 0.0 : volume.gain() == volume.level());
            CHECK(!volume.device().empty());
            // 300 reads, a look-up of the default device among them (the first, and after a second)
            const int reads = 300;
            double worstUs = 0.0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < reads; ++i) {
                const auto a = std::chrono::steady_clock::now();
                CHECK(volume.poll());
                const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
                if (us > worstUs) worstUs = us;
            }
            const double meanUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reads;
            std::printf("-- the Windows volume read %d times: %.1f microseconds per read on average, %.0f at most; at a "
                        "read every 15 ms that is %.3f %% of one core\n",
                        reads, meanUs, worstUs, 100.0 * meanUs / 15000.0);
            CHECK(meanUs < 1500.0); // under 10 % of the timer's period, whatever the machine
        }
        volume.close();
        CHECK(!volume.valid());
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return test::finish("winvolume_test");
}
