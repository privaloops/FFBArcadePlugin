#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller via Logitech Steering Wheel SDK.
// Statically linked against LogitechSteeringWheelLib.lib.
// Requires LogitechSteeringWheelEnginesWrapper.dll alongside the game
// and Logitech G Hub running.

class LogitechLED
{
public:
	LogitechLED();
	~LogitechLED();

	bool Init();
	void Close();

	bool SetLEDs(BYTE ledMask);
	bool SetLEDsFromPercent(double percent);
	bool ClearLEDs();
	bool IsAvailable() const;

private:
	bool m_available;
};
