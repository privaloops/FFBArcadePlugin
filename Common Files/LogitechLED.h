#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller via direct HID.
// G Hub must be CLOSED for direct HID access to work.

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
		USHORT usagePage;
	};

	HANDLE m_handle;
	bool m_available;
	USHORT m_reportLen;
	BYTE m_reportId;

	void EnumerateCandidates(HIDCandidate* out, int* count);
	bool ProbeCandidate(const HIDCandidate& c, int candidateIdx);
	bool SendReport(HANDLE h, const BYTE* report, USHORT len);
};
