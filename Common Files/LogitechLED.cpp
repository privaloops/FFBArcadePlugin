#include "LogitechLED.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// Logitech VID
static const USHORT LOGITECH_VID = 0x046D;

// Supported PIDs
static const USHORT KNOWN_PIDS[] = {
	0xC26E,  // G923 Xbox/PC
	0xC26D,  // G923 Xbox/PC (alt)
	0xC267,  // G923 PS/PC
	0xC24F,  // G29
};
static const int NUM_PIDS = sizeof(KNOWN_PIDS) / sizeof(KNOWN_PIDS[0]);

// Legacy LED command bytes
static const BYTE CMD_LED    = 0xF8;
static const BYTE SUBCMD_LED = 0x12;

// --- Logging ---

static FILE* g_logFile = NULL;

static void Log(const char* fmt, ...)
{
	if (!g_logFile)
		g_logFile = fopen("FFBPlugin_LED.log", "w");

	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (g_logFile) { fprintf(g_logFile, "%s\n", buf); fflush(g_logFile); }
	OutputDebugStringA(buf);
	OutputDebugStringA("\n");
}

static bool IsKnownPID(USHORT pid)
{
	for (int i = 0; i < NUM_PIDS; i++)
		if (KNOWN_PIDS[i] == pid) return true;
	return false;
}

// --- LogitechLED ---

LogitechLED::LogitechLED()
	: m_handle(INVALID_HANDLE_VALUE)
	, m_available(false)
	, m_methodFound(false)
	, m_reportLen(0)
	, m_reportId(0)
	, m_cmdOffset(0)
	, m_useSetReport(false)
{
}

LogitechLED::~LogitechLED()
{
	Close();
}

bool LogitechLED::Init()
{
	Log("LogitechLED::Init() called");
	if (m_available)
		return true;

	if (ProbeAllDevices())
	{
		m_available = true;
		Log("Init SUCCESS - method found: %s reportId=0x%02X cmdOffset=%u reportLen=%u",
			m_useSetReport ? "SetOutputReport" : "WriteFile",
			m_reportId, m_cmdOffset, m_reportLen);
	}
	else
	{
		Log("Init FAILED - no working LED method found on any collection");
		m_available = false;
	}

	return m_available;
}

void LogitechLED::Close()
{
	if (m_handle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_handle);
		m_handle = INVALID_HANDLE_VALUE;
	}
	m_available = false;
	m_methodFound = false;
}

bool LogitechLED::IsAvailable() const
{
	return m_available && m_methodFound;
}

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || !m_methodFound || m_handle == INVALID_HANDLE_VALUE)
		return false;

	return TryWrite(m_handle, m_reportLen, m_reportId, m_cmdOffset,
	                m_useSetReport, ledMask, NULL);
}

bool LogitechLED::SetLEDsFromPercent(double percent)
{
	if (percent < 0.0) percent = 0.0;
	if (percent > 1.0) percent = 1.0;

	BYTE mask = 0;
	if (percent >= 0.2)  mask |= 0x01;
	if (percent >= 0.4)  mask |= 0x02;
	if (percent >= 0.6)  mask |= 0x04;
	if (percent >= 0.8)  mask |= 0x08;
	if (percent >= 0.95) mask |= 0x10;

	return SetLEDs(mask);
}

bool LogitechLED::ClearLEDs()
{
	return SetLEDs(0x00);
}

// -------------------------------------------------------
// Try a specific write method on an open handle.
// Returns true if the write succeeds (no error from OS).
// desc is for logging during probe; NULL = silent mode.
// -------------------------------------------------------

bool LogitechLED::TryWrite(HANDLE h, USHORT reportLen, BYTE reportId,
                            BYTE cmdOffset, bool useSetReport,
                            BYTE ledMask, const char* desc)
{
	BYTE* rpt = (BYTE*)calloc(reportLen, 1);
	if (!rpt) return false;

	rpt[0] = reportId;
	rpt[cmdOffset]     = CMD_LED;       // 0xF8
	rpt[cmdOffset + 1] = SUBCMD_LED;    // 0x12
	rpt[cmdOffset + 2] = ledMask & 0x1F;
	// rpt[cmdOffset + 3..5] = 0x00
	if (cmdOffset + 6 < reportLen)
		rpt[cmdOffset + 6] = 0x01;

	BOOL ok = FALSE;
	DWORD err = 0;

	if (useSetReport)
	{
		ok = HidD_SetOutputReport(h, rpt, reportLen);
		if (!ok) err = GetLastError();
	}
	else
	{
		DWORD written = 0;
		ok = WriteFile(h, rpt, reportLen, &written, NULL);
		if (!ok) err = GetLastError();
		else if (written == 0) { ok = FALSE; err = 0; }
	}

	if (desc)
	{
		Log("  Probe %s: rptId=0x%02X cmdOff=%u %s -> %s%s",
			desc, reportId, cmdOffset,
			useSetReport ? "SetReport" : "WriteFile",
			ok ? "OK" : "FAIL",
			ok ? "" : "");
		if (!ok)
			Log("    err=%lu", err);
	}

	free(rpt);
	return ok == TRUE;
}

// -------------------------------------------------------
// Enumerate ALL Logitech wheel HID collections, try every
// plausible write method on each, keep the first that works.
// -------------------------------------------------------

bool LogitechLED::ProbeAllDevices()
{
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	HDEVINFO devInfo = SetupDiGetClassDevsA(
		&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devInfo == INVALID_HANDLE_VALUE)
	{
		Log("SetupDiGetClassDevs failed");
		return false;
	}

	SP_DEVICE_INTERFACE_DATA ifData;
	ifData.cbSize = sizeof(ifData);

	// Collect all writable collections
	HIDPath candidates[MAX_CANDIDATES];
	int numCandidates = 0;

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++)
	{
		DWORD reqSize = 0;
		SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, NULL, 0, &reqSize, NULL);

		PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail =
			(PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(reqSize);
		if (!detail) continue;
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, detail, reqSize, NULL, NULL))
		{
			free(detail);
			continue;
		}

		HANDLE h = CreateFileA(detail->DevicePath,
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE) { free(detail); continue; }

		HIDD_ATTRIBUTES attrs;
		attrs.Size = sizeof(attrs);
		if (!HidD_GetAttributes(h, &attrs) ||
		    attrs.VendorID != LOGITECH_VID ||
		    !IsKnownPID(attrs.ProductID))
		{
			CloseHandle(h);
			free(detail);
			continue;
		}

		PHIDP_PREPARSED_DATA pp = NULL;
		if (!HidD_GetPreparsedData(h, &pp))
		{
			CloseHandle(h);
			free(detail);
			continue;
		}

		HIDP_CAPS caps;
		if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS)
		{
			HidD_FreePreparsedData(pp);
			CloseHandle(h);
			free(detail);
			continue;
		}

		Log("Found PID=0x%04X UP=0x%04X U=0x%04X In=%u Out=%u Path=%s",
			attrs.ProductID, caps.UsagePage, caps.Usage,
			caps.InputReportByteLength, caps.OutputReportByteLength,
			detail->DevicePath);

		HidD_FreePreparsedData(pp);
		CloseHandle(h);

		if (caps.OutputReportByteLength > 0 && numCandidates < MAX_CANDIDATES)
		{
			HIDPath& c = candidates[numCandidates++];
			strcpy_s(c.path, detail->DevicePath);
			c.outputReportLen = caps.OutputReportByteLength;
			c.inputReportLen = caps.InputReportByteLength;
			c.usagePage = caps.UsagePage;
		}

		free(detail);
	}

	SetupDiDestroyDeviceInfoList(devInfo);

	Log("Found %d writable collection(s), probing each...", numCandidates);

	// For each collection, try multiple write strategies
	for (int ci = 0; ci < numCandidates; ci++)
	{
		HIDPath& c = candidates[ci];
		Log("--- Collection %d: UP=0x%04X OutLen=%u ---", ci, c.usagePage, c.outputReportLen);

		HANDLE h = CreateFileA(c.path,
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE)
		{
			Log("  Cannot open, err=%lu", GetLastError());
			continue;
		}

		// Define all methods to try:
		// {reportId, cmdOffset, useSetReport, description}
		struct Method {
			BYTE reportId;
			BYTE cmdOffset;
			bool useSetReport;
			const char* desc;
		};

		// Build method list based on report length
		Method methods[16];
		int numMethods = 0;

		if (c.outputReportLen >= 8)
		{
			// Legacy: report[0]=0xF8, cmd at offset 0
			methods[numMethods++] = {CMD_LED, 0, true,  "legacy-0xF8-SetReport"};
			methods[numMethods++] = {CMD_LED, 0, false, "legacy-0xF8-WriteFile"};

			// Legacy with report ID 0x00: report[0]=0x00, cmd at offset 1
			methods[numMethods++] = {0x00, 1, true,  "legacy-0x00-SetReport"};
			methods[numMethods++] = {0x00, 1, false, "legacy-0x00-WriteFile"};
		}

		if (c.outputReportLen >= 20)
		{
			// HID++ long: report[0]=0x11, cmd at offset 4 (after HID++ header)
			methods[numMethods++] = {0x11, 4, true,  "hidpp-0x11-SetReport"};
			methods[numMethods++] = {0x11, 4, false, "hidpp-0x11-WriteFile"};

			// HID++ long with cmd at offset 1 (device might interpret differently)
			methods[numMethods++] = {0x11, 1, true,  "hidpp-0x11-off1-SetReport"};
			methods[numMethods++] = {0x11, 1, false, "hidpp-0x11-off1-WriteFile"};
		}

		if (c.outputReportLen >= 64)
		{
			// HID++ very long: report[0]=0x12
			methods[numMethods++] = {0x12, 4, true,  "hidpp-0x12-SetReport"};
			methods[numMethods++] = {0x12, 4, false, "hidpp-0x12-WriteFile"};

			methods[numMethods++] = {0x12, 1, true,  "hidpp-0x12-off1-SetReport"};
			methods[numMethods++] = {0x12, 1, false, "hidpp-0x12-off1-WriteFile"};

			// Raw: report[0]=0x00, cmd at offset 1 (for 64-byte vendor endpoints)
			methods[numMethods++] = {0x00, 1, true,  "raw-0x00-64b-SetReport"};
			methods[numMethods++] = {0x00, 1, false, "raw-0x00-64b-WriteFile"};
		}

		// Try each method with ALL LEDs ON (0x1F)
		for (int mi = 0; mi < numMethods; mi++)
		{
			Method& m = methods[mi];
			if (TryWrite(h, c.outputReportLen, m.reportId, m.cmdOffset,
			             m.useSetReport, 0x1F, m.desc))
			{
				Log("*** SUCCESS on collection %d with %s ***", ci, m.desc);

				// Keep this handle and method
				m_handle = h;
				m_reportLen = c.outputReportLen;
				m_reportId = m.reportId;
				m_cmdOffset = m.cmdOffset;
				m_useSetReport = m.useSetReport;
				m_methodFound = true;

				// Turn off LEDs after test
				TryWrite(h, c.outputReportLen, m.reportId, m.cmdOffset,
				         m.useSetReport, 0x00, NULL);

				return true;
			}
		}

		CloseHandle(h);
	}

	return false;
}
