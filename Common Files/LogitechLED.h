#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller.
// Phase 0: G Hub LED SDK (requires G Hub running + kernel drivers)
// Phase 1: HID++ 2.0 feature discovery (G923 Xbox, requires G Hub closed)
// Phase 2: Legacy [F8 12] (G29/G923-PS)

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

	enum LEDMethod { METHOD_NONE, METHOD_LEGACY, METHOD_HIDPP, METHOD_SDK };
	LEDMethod m_method;
	BYTE m_ledFeatureIdx;
	BYTE m_ledFunctionId;
	BYTE m_deviceIdx;

	// G Hub SDK
	HMODULE m_sdkDll;

	void EnumerateCandidates(HIDCandidate* out, int* count);
	bool SendReport(HANDLE h, const BYTE* report, USHORT len);
	bool ReadReport(HANDLE h, BYTE* report, USHORT len, DWORD timeoutMs);
	bool TrySDK();
	bool TryHIDPPDiscovery(HANDLE h, USHORT outLen, USHORT inLen);
	bool TryLegacy(HANDLE h, USHORT outLen);
};
