#pragma once
#include <cstdint>

bool control_task_start();
// USB callbacks/tasks enqueue input; only the control task owns musical state.
bool control_post_midi(const uint8_t packet[4]);
void control_midi_disconnect();
namespace cv
{
// External ADCs supply calibrated input volts through the same owner task.
bool setPitchVolts(float volts);
bool useInternalPitch();
bool setPitchCalibration(float zeroMv, float millivoltsPerVolt);
}
