// SPDX-License-Identifier: GPL-3.0-or-later
// The plug-in's identity and parameter ids. Never change them: a host stores them in its projects and automation.
#pragma once

#include "params.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace ap::plugin {

// the plug-in's class id (generated once, 2026-10-26)
static const Steinberg::FUID kClassId(0xFBA1438F, 0xA2F24621, 0xB4F73039, 0x21B40D54);

// Parameters that are not one of the chain's values (params.h).
enum ExtraId : Steinberg::Vst::ParamID {
    kPresetId = 1, // the preset list (DJAP.ini's presets): choosing one loads its values
    kColourId = 2, // the colour list
    kBypassId = 3, // the deck's Bypass (the input delayed by the look-ahead)
    kLookId = 4,   // the limiter look-ahead in ms: the plug-in's latency
    kOversamplingId = 5, // the soft clipper's and the exciter's oversampling: 2x, 4x or 8x (since 0.1.145)
    kMonoId = 6,         // mono: left and right mixed from the input on (since 0.1.245; DeckControl::mono)
};

// The id of a chain value: a hash of its ini key, so values can be added or reordered in params.h without breaking a
// host's project. Always >= 1000 (the extra ids stay below).
inline Steinberg::Vst::ParamID paramId(int p) {
    uint32_t h = 2166136261u; // FNV-1a
    for (const char* k = paramInfo(p).key; *k; ++k) h = (h ^ static_cast<uint8_t>(*k)) * 16777619u;
    return 1000u + (h % 0x3FFF0000u);
}

// The chain value of an id, -1 if it is not one.
inline int paramIndex(Steinberg::Vst::ParamID id) {
    for (int p = 0; p < kParamCount; ++p)
        if (paramId(p) == id) return p;
    return -1;
}

} // namespace ap::plugin
