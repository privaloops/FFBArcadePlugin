#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller via HID
// G923 Xbox: HID++ protocol via WriteFile on vendor collection
// G29/G923 PS: Legacy 0xF8 command

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
	static const int MAX_CANDIDATES = 8;

	struct HIDPath
	{
		char path[512];
		USHORT outputReportLen;
		USHORT inputReportLen;
		USHORT usagePage;
	};

	HANDLE m_writeHandle;     // Sync handle for WriteFile / HidD_SetOutputReport
	HANDLE m_readHandle;      // Overlapped handle for ReadFile with timeout
	bool m_available;
	USHORT m_reportLen;
	USHORT m_inputReportLen;

	// LED method (determined at init)
	enum LEDMethod { METHOD_NONE, METHOD_LEGACY, METHOD_HIDPP };
	LEDMethod m_method;
	BYTE m_ledFeatureIndex;   // HID++ feature index for LED control

	bool FindDevice(char* outPath, int pathSize, USHORT* outReportLen,
	                USHORT* outInputLen, bool* outIsVendor);

	// Legacy protocol
	bool SetLEDsLegacy(BYTE ledMask);

	// HID++ protocol via WriteFile
	bool InitHIDPP();
	bool SetLEDsHIDPP(BYTE ledMask);
	bool HIDPPSendLong(BYTE deviceIdx, BYTE featureIdx, BYTE funcSwId,
	                    const BYTE* params, int paramLen);
	bool HIDPPRecv(BYTE* response, int responseLen, DWORD timeoutMs);
	BYTE HIDPPDiscoverFeature(USHORT featureId);
	void HIDPPEnumerateAllFeatures();
};
