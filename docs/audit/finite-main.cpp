#include <cstdio>
#include <cmath>
#include <limits>
extern "C" float guard(float);
int main() { float y=guard(std::numeric_limits<float>::quiet_NaN()); std::printf("guard_nan finite=%d value=%g\n", std::isfinite(y),y); }
