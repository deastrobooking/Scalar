#pragma once
#include "scalar/ControlTransport.h"

namespace scalar
{
// One producer (control task), one consumer (audio task).
using ParamBridge = ControlTransport;
ParamBridge& paramBridge() noexcept;
}
