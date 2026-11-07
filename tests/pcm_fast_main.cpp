#include <cstdint>
#include <limits>
extern "C" int16_t pcmWithFastMath(float);
int main()
{
    return pcmWithFastMath(std::numeric_limits<float>::quiet_NaN()) != 0
        || pcmWithFastMath(std::numeric_limits<float>::infinity()) != 0
        || pcmWithFastMath(-std::numeric_limits<float>::infinity()) != 0
        || pcmWithFastMath(1.f) != 32767;
}
