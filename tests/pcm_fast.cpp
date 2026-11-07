#include "scalar/AudioUtils.h"
extern "C" int16_t pcmWithFastMath(float sample) { return scalar::toPcm(sample); }
