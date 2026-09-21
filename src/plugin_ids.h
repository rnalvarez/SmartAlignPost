#pragma once
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace SmartAlignPost {

static const Steinberg::FUID ProcessorUID (0x91C4B7E2, 0x5A183F62, 0xB7D2E914, 0x3C6A805F);
static const Steinberg::FUID ControllerUID(0x4B7E21A9, 0xC53D8F70, 0xA9E6142B, 0x72F0C83D);

static constexpr Steinberg::Vst::ParamID kResidualSamplesParam = 1001;
static constexpr Steinberg::Vst::ParamID kApplyResidualParam = 1002;

}
