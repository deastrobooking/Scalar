#include "gliese/Filter/FilterModels.h"
extern "C" float guard(float x) { return FilterModelDetail::clampAudio(x); }
