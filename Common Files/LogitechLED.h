#pragma once

#include <windows.h>

// Logitech G923/G29 RPM LED controller via HID
// Probes ALL HID collections with multiple write methods at init time
// to find a working LED command path.

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

	// The working method found during Init
	HANDLE m_handle;
	bool m_available;
	bool m_methodFound;      // true = a working write method was found
	USHORT m_reportLen;      // output report length of the working path
	BYTE m_reportId;         // report ID that worked (first byte)
	BYTE m_cmdOffset;        // offset of 0xF8 command in the report
	bool m_useSetReport;     // true = HidD_SetOutputReport, false = WriteFile

	bool ProbeAllDevices();
	bool TryWrite(HANDLE h, USHORT reportLen, BYTE reportId, BYTE cmdOffset,
	              bool useSetReport, BYTE ledMask, const char* desc);
};
