#pragma once

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

// Logitech G923/G29 RPM LED controller via DirectInput Escape().
// Uses the same mechanism as the Logitech SDK "Independent" sample.
// Requires Logitech G Hub running (provides the FF driver that handles Escape).
// No external DLL dependency.

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
	HMODULE m_dinputDll;
	LPDIRECTINPUT8A m_pDI;
	LPDIRECTINPUTDEVICE8A m_pDevice;
	bool m_available;

	struct EnumContext
	{
		GUID deviceGuid;
		bool found;
	};

	bool PlayLedsEscape(float currentRPM, float rpmFirstLed, float rpmRedLine);
	static BOOL CALLBACK EnumDevicesCallback(LPCDIDEVICEINSTANCEA lpddi, LPVOID pvRef);
};
