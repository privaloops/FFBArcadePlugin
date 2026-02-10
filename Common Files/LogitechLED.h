#pragma once

#include <windows.h>

// Logitech G923 RPM LED controller via HID
// Supports both Xbox/PC (PID 0xC26E) and PS/PC (PID 0xC267) variants

class LogitechLED
{
public:
	LogitechLED();
	~LogitechLED();

	// Initialize: find and open the G923 HID device
	// Returns true if device found and opened
	bool Init();

	// Close the HID device handle
	void Close();

	// Set RPM LEDs using a bitmask (5 LEDs: bits 0-4)
	// Bit 0 = LED 1 (green), Bit 4 = LED 5 (red)
	bool SetLEDs(BYTE ledMask);

	// Set RPM LEDs based on a percentage (0.0 to 1.0)
	// Progressively lights LEDs: 0-20% = 1 LED, 20-40% = 2, etc.
	bool SetLEDsFromPercent(double percent);

	// Turn off all LEDs
	bool ClearLEDs();

	// Returns true if a G923 device was found and handle is valid
	bool IsAvailable() const;

private:
	HANDLE m_deviceHandle;
	bool m_available;

	// Find the G923 HID device path and open it
	bool FindAndOpenDevice();
};
