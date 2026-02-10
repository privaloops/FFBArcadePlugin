#include "LogitechLED.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

static const USHORT LOGITECH_VID = 0x046D;
static const USHORT KNOWN_PIDS[] = { 0xC26E, 0xC26D, 0xC267, 0xC266, 0xC24F };
static const int NUM_PIDS = sizeof(KNOWN_PIDS) / sizeof(KNOWN_PIDS[0]);

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
	, m_reportLen(0)
	, m_reportId(0)
{
}

LogitechLED::~LogitechLED()
{
	Close();
}

void LogitechLED::Close()
{
	if (m_handle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_handle);
		m_handle = INVALID_HANDLE_VALUE;
	}
	m_available = false;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

bool LogitechLED::SendReport(HANDLE h, const BYTE* report, USHORT len)
{
	DWORD written = 0;
	BOOL ok = WriteFile(h, report, len, &written, NULL);
	return ok && written > 0;
}

// --- Enumerate writable HID collections ---

void LogitechLED::EnumerateCandidates(HIDCandidate* out, int* count)
{
	*count = 0;

	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	HDEVINFO devInfo = SetupDiGetClassDevsA(
		&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devInfo == INVALID_HANDLE_VALUE) return;

	SP_DEVICE_INTERFACE_DATA ifData;
	ifData.cbSize = sizeof(ifData);

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); i++)
	{
		DWORD reqSize = 0;
		SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, NULL, 0, &reqSize, NULL);

		PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail =
			(PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(reqSize);
		if (!detail) continue;
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, detail, reqSize, NULL, NULL))
		{ free(detail); continue; }

		HANDLE h = CreateFileA(detail->DevicePath,
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE) { free(detail); continue; }

		HIDD_ATTRIBUTES attrs;
		attrs.Size = sizeof(attrs);
		if (!HidD_GetAttributes(h, &attrs) ||
		    attrs.VendorID != LOGITECH_VID || !IsKnownPID(attrs.ProductID))
		{ CloseHandle(h); free(detail); continue; }

		PHIDP_PREPARSED_DATA pp = NULL;
		if (!HidD_GetPreparsedData(h, &pp))
		{ CloseHandle(h); free(detail); continue; }

		HIDP_CAPS caps;
		if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS)
		{ HidD_FreePreparsedData(pp); CloseHandle(h); free(detail); continue; }

		Log("  HID: PID=0x%04X UP=0x%04X U=0x%04X In=%u Out=%u",
			attrs.ProductID, caps.UsagePage, caps.Usage,
			caps.InputReportByteLength, caps.OutputReportByteLength);

		HidD_FreePreparsedData(pp);
		CloseHandle(h);

		if (caps.OutputReportByteLength > 0 && *count < MAX_CANDIDATES)
		{
			HIDCandidate& c = out[(*count)++];
			strcpy_s(c.path, detail->DevicePath);
			c.outputReportLen = caps.OutputReportByteLength;
			c.usagePage = caps.UsagePage;
		}

		free(detail);
	}

	SetupDiDestroyDeviceInfoList(devInfo);
}

// --- Probe one collection ---

bool LogitechLED::ProbeCandidate(const HIDCandidate& c, int candidateIdx)
{
	HANDLE h = CreateFileA(c.path,
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		Log("  Cannot open (err=%lu)", GetLastError());
		return false;
	}

	USHORT len = c.outputReportLen;
	int attempt = 0;
	bool found = false;

	// Determine report ID based on output length
	// col02 (OUT=20) uses report ID 0x11
	// col03 (OUT=64) uses report ID 0x12
	BYTE reportId = (len <= 20) ? 0x11 : 0x12;

	// Template: [reportId, F8, 12, mask, 00, 00, 00, 00, ...padding]
	// This is the Linux kernel lg4ff format with report ID prepended
	{
		BYTE rpt[64] = {0};
		rpt[0] = reportId;
		rpt[1] = 0xF8;
		rpt[2] = 0x12;
		rpt[3] = 0x1F;  // all 5 LEDs on

		attempt++;
		bool ok = SendReport(h, rpt, len);
		Log("  [%d.%d] WriteFile [%02X F8 12 1F 00..] -> %s",
			candidateIdx, attempt, reportId,
			ok ? "OK *** LOOK AT WHEEL ***" : "FAIL");
		if (!ok) Log("    err=%lu", GetLastError());

		if (ok)
		{
			Sleep(500);
			rpt[3] = 0x00;  // LEDs off
			SendReport(h, rpt, len);
			Sleep(200);

			if (!found)
			{
				found = true;
				// Remember this working config
				m_handle = h;
				m_reportLen = len;
				m_reportId = reportId;
				Log("  >>> SAVED as working method");
				return true;  // keep handle open
			}
		}
	}

	// Also try with 0x01 at byte 8 (some implementations add this)
	{
		BYTE rpt[64] = {0};
		rpt[0] = reportId;
		rpt[1] = 0xF8;
		rpt[2] = 0x12;
		rpt[3] = 0x1F;
		if (len > 8) rpt[8] = 0x01;

		attempt++;
		bool ok = SendReport(h, rpt, len);
		Log("  [%d.%d] WriteFile [%02X F8 12 1F 00 00 00 00 01..] -> %s",
			candidateIdx, attempt, reportId,
			ok ? "OK *** LOOK AT WHEEL ***" : "FAIL");
		if (!ok) Log("    err=%lu", GetLastError());

		if (ok)
		{
			Sleep(500);
			rpt[3] = 0x00;
			SendReport(h, rpt, len);
			Sleep(200);

			if (!found)
			{
				found = true;
				m_handle = h;
				m_reportLen = len;
				m_reportId = reportId;
				Log("  >>> SAVED as working method");
				return true;
			}
		}
	}

	// Try with device index byte after report ID: [reportId, 0x01, F8, 12, mask, ...]
	{
		BYTE rpt[64] = {0};
		rpt[0] = reportId;
		rpt[1] = 0x01;
		rpt[2] = 0xF8;
		rpt[3] = 0x12;
		rpt[4] = 0x1F;

		attempt++;
		bool ok = SendReport(h, rpt, len);
		Log("  [%d.%d] WriteFile [%02X 01 F8 12 1F 00..] -> %s",
			candidateIdx, attempt, reportId,
			ok ? "OK *** LOOK AT WHEEL ***" : "FAIL");
		if (!ok) Log("    err=%lu", GetLastError());

		if (ok)
		{
			Sleep(500);
			rpt[4] = 0x00;
			SendReport(h, rpt, len);
			Sleep(200);

			if (!found)
			{
				found = true;
				m_handle = h;
				m_reportLen = len;
				m_reportId = reportId;
				Log("  >>> SAVED as working method");
				return true;
			}
		}
	}

	CloseHandle(h);
	return false;
}

// --- Init: probe all collections ---

bool LogitechLED::Init()
{
	Log("=== LogitechLED HID PROBE ===");
	Log("G Hub must be CLOSED for this to work!");
	Log("Each successful write flashes LEDs for 500ms.");
	Log("");

	if (m_available)
		return true;

	HIDCandidate candidates[MAX_CANDIDATES];
	int numCandidates = 0;
	EnumerateCandidates(candidates, &numCandidates);

	Log("");
	Log("Found %d writable collection(s), probing...", numCandidates);

	for (int ci = 0; ci < numCandidates; ci++)
	{
		Log("");
		Log("--- Collection %d: UP=0x%04X OutLen=%u ---",
			ci, candidates[ci].usagePage, candidates[ci].outputReportLen);

		if (ProbeCandidate(candidates[ci], ci))
		{
			m_available = true;
			Log("");
			Log("=== LED CONTROL ACTIVE - SUSTAINED TEST ===");
			Log("Turning ALL LEDs on for 2 seconds...");

			// Sustained test: all LEDs on for 2s
			SetLEDs(0x1F);
			Sleep(2000);

			// Then cycle each LED individually
			for (int led = 0; led < 5; led++)
			{
				BYTE mask = (BYTE)(1 << led);
				Log("  LED %d (mask=0x%02X)", led + 1, mask);
				SetLEDs(mask);
				Sleep(500);
			}

			ClearLEDs();
			Log("LED test complete.");
			return true;
		}
	}

	Log("");
	Log("=== NO WORKING METHOD FOUND ===");
	Log("If G Hub was running, close it and retry.");
	return false;
}

// --- Runtime LED control ---

static int g_setLedsCallCount = 0;

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || m_handle == INVALID_HANDLE_VALUE)
		return false;

	BYTE rpt[64] = {0};
	rpt[0] = m_reportId;
	rpt[1] = 0xF8;
	rpt[2] = 0x12;
	rpt[3] = ledMask & 0x1F;

	bool ok = SendReport(m_handle, rpt, m_reportLen);

	g_setLedsCallCount++;
	if (g_setLedsCallCount <= 20 || !ok)
	{
		Log("SetLEDs(0x%02X) -> %s (call #%d)", ledMask, ok ? "OK" : "FAIL", g_setLedsCallCount);
		if (!ok) Log("  err=%lu", GetLastError());
	}

	return ok;
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
