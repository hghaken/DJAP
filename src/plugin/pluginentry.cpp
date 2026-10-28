// SPDX-License-Identifier: GPL-3.0-or-later
// The plug-in's factory: one class, an audio effect (processor and controller in one).
#include "plugin.h"
#include "pluginids.h"
#include "version.h"

#include "public.sdk/source/main/pluginfactory.h"

#define DJAP_VENDOR "DJAP"
#define DJAP_URL "https://github.com/hghaken/DJAP"

BEGIN_FACTORY_DEF(DJAP_VENDOR, DJAP_URL, "")

DEF_CLASS2(INLINE_UID_FROM_FUID(ap::plugin::kClassId), Steinberg::PClassInfo::kManyInstances, kVstAudioEffectClass,
           "DJAP", 0, // processor and controller in one object: not distributable
           "Fx|Dynamics", AP_VERSION_STRING, kVstVersionString, ap::plugin::Plugin::createInstance)

END_FACTORY
