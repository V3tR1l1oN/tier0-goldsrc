// tier0 -- clean-room functional reconstruction of GoldSrc tier0.dll (GPL-3.0).
#ifndef FASTTIMER_H
#define FASTTIMER_H

#include "platform.h"

PLATFORM_INTERFACE int64 g_ClockSpeed;
PLATFORM_INTERFACE unsigned long g_dwClockSpeed;
PLATFORM_INTERFACE double g_ClockSpeedMicrosecondsMultiplier;
PLATFORM_INTERFACE double g_ClockSpeedMillisecondsMultiplier;
PLATFORM_INTERFACE double g_ClockSpeedSecondsMultiplier;

PLATFORM_INTERFACE int64 g_ulLastCycleSample;
PLATFORM_INTERFACE int g_cBadCycleCountReceived;

uint64 CalculateCPUFreq();

#endif // FASTTIMER_H
