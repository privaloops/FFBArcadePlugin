#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller via Logitech Steering Wheel SDK.
// Requires Logitech G Hub to be running.
// Uses dynamic loading: no hard dependency on the SDK DLL.

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
	HMODULE m_sdkModule;
	bool m_available;

	// SDK function pointers
	typedef bool (__cdecl *PFN_LogiSteeringInitialize)(bool);
	typedef bool (__cdecl *PFN_LogiUpdate)();
	typedef bool (__cdecl *PFN_LogiIsConnected)(int);
	typedef bool (__cdecl *PFN_LogiPlayLeds)(int, float, float, float);
	typedef void (__cdecl *PFN_LogiSteeringShutdown)();

	PFN_LogiSteeringInitialize m_pfnInit;
	PFN_LogiUpdate m_pfnUpdate;
	PFN_LogiIsConnected m_pfnIsConnected;
	PFN_LogiPlayLeds m_pfnPlayLeds;
	PFN_LogiSteeringShutdown m_pfnShutdown;
};
