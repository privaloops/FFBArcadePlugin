#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller via HID
// Uses a diagnostic probe at init to find the working LED method
// by trying all collections and all command formats.

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

	// Working method (found during probe)
	HANDLE m_handle;
	bool m_available;
	USHORT m_reportLen;
	BYTE m_reportTemplate[64];  // The exact report bytes that worked
	int m_templateLen;

	void EnumerateCandidates(HIDCandidate* out, int* count);
	bool ProbeCandidate(const HIDCandidate& c, int candidateIdx);
	bool SendReport(HANDLE h, const BYTE* report, USHORT len);
};
