#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller.
// Uses HID++ 2.0 feature discovery to find LED control on the G923 Xbox,
// falls back to legacy [F8 12] for G29/G923-PS.

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

	struct HIDCandidate
	{
		char path[512];
		USHORT outputReportLen;
		USHORT inputReportLen;
		USHORT usagePage;
	};

	HANDLE m_handle;
	bool m_available;
	USHORT m_reportLen;

	// LED method found during discovery
	enum LEDMethod { METHOD_NONE, METHOD_LEGACY, METHOD_HIDPP };
	LEDMethod m_method;
	BYTE m_ledFeatureIdx;    // HID++ feature index for LED control
	BYTE m_ledFunctionId;    // HID++ function ID for setting LEDs
	BYTE m_deviceIdx;        // HID++ device index (0xFF=USB, 0x01=receiver)

	void EnumerateCandidates(HIDCandidate* out, int* count);
	bool SendReport(HANDLE h, const BYTE* report, USHORT len);
	bool ReadReport(HANDLE h, BYTE* report, USHORT len, DWORD timeoutMs);
	bool TryHIDPPDiscovery(HANDLE h, USHORT outLen, USHORT inLen);
	bool TryLegacy(HANDLE h, USHORT outLen);
};
