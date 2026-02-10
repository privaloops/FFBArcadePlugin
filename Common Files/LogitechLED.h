#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller via HID
// G923 Xbox (0xC26E/0xC26D): HID++ protocol on vendor collection
// G923 PS (0xC267), G29 (0xC24F): Legacy protocol

class LogitechLED
{
public:
	LogitechLED();
	~LogitechLED();

	bool Init();
	void Close();

	// Set RPM LEDs using a bitmask (5 LEDs: bits 0-4)
	bool SetLEDs(BYTE ledMask);

	// Set RPM LEDs based on a percentage (0.0 to 1.0)
	bool SetLEDsFromPercent(double percent);

	// Turn off all LEDs
	bool ClearLEDs();

	bool IsAvailable() const;

private:
	HANDLE m_writeHandle;    // Sync handle for HidD_SetOutputReport
	HANDLE m_readHandle;     // Overlapped handle for ReadFile with timeout
	bool m_available;
	USHORT m_outputReportLength;
	USHORT m_inputReportLength;
	USHORT m_productId;
	bool m_useHIDPP;
	BYTE m_ledFeatureIndex;

	bool FindAndOpenDevice();

	// Legacy protocol (G29, G923 PS)
	bool SetLEDsLegacy(BYTE ledMask);

	// HID++ protocol (G923 Xbox)
	bool InitHIDPP();
	bool SetLEDsHIDPP(BYTE ledMask);
	bool HIDPPSend(BYTE featureIdx, BYTE funcSwId, const BYTE* params, int paramLen);
	bool HIDPPRecv(BYTE* response, DWORD timeoutMs);
	BYTE HIDPPGetFeatureIndex(USHORT featureId);
	void HIDPPLogAllFeatures();
};
