#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller.
// Phase 0: Steering Wheel SDK (LogiPlayLeds - works with G Hub)
// Phase 1: G Hub LED SDK (keyboard/mouse backlighting fallback)
// Phase 2: HID++ 2.0 feature discovery (diagnostic only)
// Phase 3: Legacy [F8 12] (G29/G923-PS)

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

	enum LEDMethod { METHOD_NONE, METHOD_LEGACY, METHOD_HIDPP, METHOD_SDK, METHOD_STEERING_SDK };
	LEDMethod m_method;
	BYTE m_ledFeatureIdx;
	BYTE m_ledFunctionId;
	BYTE m_deviceIdx;

	// G Hub LED SDK
	HMODULE m_sdkDll;
	// Steering Wheel SDK
	HMODULE m_steeringDll;

	void EnumerateCandidates(HIDCandidate* out, int* count);
	bool SendReport(HANDLE h, const BYTE* report, USHORT len);
	bool ReadReport(HANDLE h, BYTE* report, USHORT len, DWORD timeoutMs);
	bool TrySteeringSDK();
	bool TrySDK();
	bool TryHIDPPDiscovery(HANDLE h, USHORT outLen, USHORT inLen);
	bool TryLegacy(HANDLE h, USHORT outLen);
};
