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
static const USHORT KNOWN_PIDS[] = { 0xC26E, 0xC26D, 0xC267, 0xC24F };
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
	, m_templateLen(0)
{
	memset(m_reportTemplate, 0, sizeof(m_reportTemplate));
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
	m_templateLen = 0;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
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
// Send a raw report via WriteFile (interrupt OUT)
// -------------------------------------------------------

bool LogitechLED::SendReport(HANDLE h, const BYTE* report, USHORT len)
{
	DWORD written = 0;
	BOOL ok = WriteFile(h, report, len, &written, NULL);
	return ok && written > 0;
}

// -------------------------------------------------------
// SetLEDs: apply LED mask to the stored template
// The template has 0x1F at the mask position; we replace it.
// -------------------------------------------------------

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || m_handle == INVALID_HANDLE_VALUE || m_templateLen == 0)
		return false;

	// Copy template and patch the mask byte
	BYTE rpt[64];
	memcpy(rpt, m_reportTemplate, m_templateLen);

	// Find and replace the 0x1F placeholder with actual mask
	for (int i = 0; i < m_templateLen; i++)
	{
		if (i > 0 && rpt[i] == 0x1F)
		{
			// Verify this is the mask byte: preceded by 0x12 (SUBCMD_LED)
			if (i >= 2 && rpt[i - 1] == 0x12 && rpt[i - 2] == 0xF8)
			{
				rpt[i] = ledMask & 0x1F;
				break;
			}
		}
	}

	return SendReport(m_handle, rpt, (USHORT)m_templateLen);
}

// -------------------------------------------------------
// Enumerate all Logitech wheel HID collections with output
// -------------------------------------------------------

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

		Log("Found PID=0x%04X UP=0x%04X U=0x%04X In=%u Out=%u Path=%s",
			attrs.ProductID, caps.UsagePage, caps.Usage,
			caps.InputReportByteLength, caps.OutputReportByteLength,
			detail->DevicePath);

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

// -------------------------------------------------------
// Probe a single collection with all LED command formats.
// For each successful write, flash LEDs for 300ms so the
// user can see which one actually works.
// Returns true if any write succeeded (OS-level).
// -------------------------------------------------------

bool LogitechLED::ProbeCandidate(const HIDCandidate& c, int candidateIdx)
{
	HANDLE h = CreateFileA(c.path,
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
	{
		Log("  Cannot open, err=%lu", GetLastError());
		return false;
	}

	USHORT len = c.outputReportLen;
	int attempt = 0;
	bool anySuccess = false;

	// Build LED command templates to try.
	// Each template is a full report with mask=0x1F (all LEDs on).
	// The structure: {reportId, [padding], 0xF8, 0x12, 0x1F, [padding], 0x01, ...}
	struct Template
	{
		BYTE data[64];
		const char* desc;
	};

	Template templates[20];
	int numTemplates = 0;

	// Helper lambda-style macro to add a template
	#define ADD_TEMPLATE(description) \
		templates[numTemplates].desc = description; \
		numTemplates++;

	// --- Format A: Legacy [0xF8, 0x12, mask, 0, 0, 0, 0, 0x01] ---
	if (len >= 8)
	{
		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0xF8;
		templates[numTemplates].data[1] = 0x12;
		templates[numTemplates].data[2] = 0x1F;
		templates[numTemplates].data[7] = 0x01;
		ADD_TEMPLATE("A1: [F8 12 1F .. 01]");

		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x00;
		templates[numTemplates].data[1] = 0xF8;
		templates[numTemplates].data[2] = 0x12;
		templates[numTemplates].data[3] = 0x1F;
		templates[numTemplates].data[8] = 0x01;
		ADD_TEMPLATE("A2: [00 F8 12 1F .. 01]");
	}

	// --- Format B: Report ID 0x11, cmd at various offsets ---
	if (len >= 20)
	{
		// B1: [11 F8 12 1F 00 00 00 01 ...]
		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x11;
		templates[numTemplates].data[1] = 0xF8;
		templates[numTemplates].data[2] = 0x12;
		templates[numTemplates].data[3] = 0x1F;
		templates[numTemplates].data[7] = 0x01;
		ADD_TEMPLATE("B1: [11 F8 12 1F .. 01]");

		// B2: [11 00 F8 12 1F 00 00 00 01 ...]
		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x11;
		templates[numTemplates].data[1] = 0x00;
		templates[numTemplates].data[2] = 0xF8;
		templates[numTemplates].data[3] = 0x12;
		templates[numTemplates].data[4] = 0x1F;
		templates[numTemplates].data[9] = 0x01;
		ADD_TEMPLATE("B2: [11 00 F8 12 1F .. 01]");

		// B3: [11 01 F8 12 1F 00 00 00 01 ...]
		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x11;
		templates[numTemplates].data[1] = 0x01;
		templates[numTemplates].data[2] = 0xF8;
		templates[numTemplates].data[3] = 0x12;
		templates[numTemplates].data[4] = 0x1F;
		templates[numTemplates].data[9] = 0x01;
		ADD_TEMPLATE("B3: [11 01 F8 12 1F .. 01]");

		// B4: [11 FF F8 12 1F 00 00 00 01 ...]
		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x11;
		templates[numTemplates].data[1] = 0xFF;
		templates[numTemplates].data[2] = 0xF8;
		templates[numTemplates].data[3] = 0x12;
		templates[numTemplates].data[4] = 0x1F;
		templates[numTemplates].data[9] = 0x01;
		ADD_TEMPLATE("B4: [11 FF F8 12 1F .. 01]");

		// B5: [11 01 00 00 F8 12 1F 00 00 00 01 ...] (HID++ params style)
		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x11;
		templates[numTemplates].data[1] = 0x01;
		templates[numTemplates].data[4] = 0xF8;
		templates[numTemplates].data[5] = 0x12;
		templates[numTemplates].data[6] = 0x1F;
		templates[numTemplates].data[11] = 0x01;
		ADD_TEMPLATE("B5: [11 01 00 00 F8 12 1F .. 01]");

		// B6: [11 FF 00 00 F8 12 1F 00 00 00 01 ...]
		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x11;
		templates[numTemplates].data[1] = 0xFF;
		templates[numTemplates].data[4] = 0xF8;
		templates[numTemplates].data[5] = 0x12;
		templates[numTemplates].data[6] = 0x1F;
		templates[numTemplates].data[11] = 0x01;
		ADD_TEMPLATE("B6: [11 FF 00 00 F8 12 1F .. 01]");
	}

	// --- Format C: Report ID 0x12 (very long) ---
	if (len >= 64)
	{
		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x12;
		templates[numTemplates].data[1] = 0xF8;
		templates[numTemplates].data[2] = 0x12;
		templates[numTemplates].data[3] = 0x1F;
		templates[numTemplates].data[7] = 0x01;
		ADD_TEMPLATE("C1: [12 F8 12 1F .. 01]");

		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x12;
		templates[numTemplates].data[1] = 0x01;
		templates[numTemplates].data[2] = 0xF8;
		templates[numTemplates].data[3] = 0x12;
		templates[numTemplates].data[4] = 0x1F;
		templates[numTemplates].data[9] = 0x01;
		ADD_TEMPLATE("C2: [12 01 F8 12 1F .. 01]");

		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x12;
		templates[numTemplates].data[1] = 0xFF;
		templates[numTemplates].data[2] = 0xF8;
		templates[numTemplates].data[3] = 0x12;
		templates[numTemplates].data[4] = 0x1F;
		templates[numTemplates].data[9] = 0x01;
		ADD_TEMPLATE("C3: [12 FF F8 12 1F .. 01]");

		memset(templates[numTemplates].data, 0, 64);
		templates[numTemplates].data[0] = 0x12;
		templates[numTemplates].data[1] = 0x01;
		templates[numTemplates].data[4] = 0xF8;
		templates[numTemplates].data[5] = 0x12;
		templates[numTemplates].data[6] = 0x1F;
		templates[numTemplates].data[11] = 0x01;
		ADD_TEMPLATE("C4: [12 01 00 00 F8 12 1F .. 01]");
	}

	#undef ADD_TEMPLATE

	// Try each template
	for (int t = 0; t < numTemplates; t++)
	{
		attempt++;
		bool ok = SendReport(h, templates[t].data, len);

		Log("  [%d.%d] %s -> %s%s",
			candidateIdx, attempt, templates[t].desc,
			ok ? "WRITE OK" : "WRITE FAIL",
			ok ? " *** LOOK AT WHEEL ***" : "");

		if (!ok)
		{
			Log("    err=%lu", GetLastError());
			continue;
		}

		// Write succeeded - flash for 300ms so user can see
		Sleep(300);

		// Turn off: same template but with mask=0x00
		BYTE offReport[64];
		memcpy(offReport, templates[t].data, 64);
		for (int i = 0; i < len; i++)
		{
			if (offReport[i] == 0x1F)
			{
				offReport[i] = 0x00;
				break;
			}
		}
		SendReport(h, offReport, len);
		Sleep(100);

		// If this is the first OS-level success, remember it
		if (!anySuccess)
		{
			anySuccess = true;
			// Don't save yet - we want to try all methods so user can see
		}
	}

	// Also try HidD_SetOutputReport for the basic legacy format
	if (len >= 8)
	{
		BYTE rpt[64] = {0};
		rpt[0] = 0xF8;
		rpt[1] = 0x12;
		rpt[2] = 0x1F;
		rpt[7] = 0x01;

		attempt++;
		BOOL ok = HidD_SetOutputReport(h, rpt, len);
		Log("  [%d.%d] SetReport [F8 12 1F .. 01] -> %s%s",
			candidateIdx, attempt,
			ok ? "WRITE OK" : "WRITE FAIL",
			ok ? " *** LOOK AT WHEEL ***" : "");
		if (!ok)
			Log("    err=%lu", GetLastError());
		else
		{
			Sleep(300);
			rpt[2] = 0x00;
			HidD_SetOutputReport(h, rpt, len);
			Sleep(100);
			anySuccess = true;
		}
	}

	CloseHandle(h);
	return anySuccess;
}

// -------------------------------------------------------
// Init: run the diagnostic probe on all collections
// -------------------------------------------------------

bool LogitechLED::Init()
{
	Log("LogitechLED::Init() - DIAGNOSTIC PROBE MODE");
	Log("Watch the RPM LEDs on the wheel during this probe!");
	Log("Each successful write will flash LEDs for 300ms.");
	Log("");

	if (m_available)
		return true;

	HIDCandidate candidates[MAX_CANDIDATES];
	int numCandidates = 0;
	EnumerateCandidates(candidates, &numCandidates);

	Log("");
	Log("Probing %d writable collection(s)...", numCandidates);
	Log("=========================================");

	for (int ci = 0; ci < numCandidates; ci++)
	{
		Log("");
		Log("=== Collection %d: UP=0x%04X OutLen=%u ===",
			ci, candidates[ci].usagePage, candidates[ci].outputReportLen);

		ProbeCandidate(candidates[ci], ci);
	}

	Log("");
	Log("=========================================");
	Log("PROBE COMPLETE. Check log for which attempt showed LEDs.");
	Log("Report the [X.Y] number of the flash you saw.");

	// For now, don't set m_available - this is diagnostic only.
	// Once we know which method works, we'll hardcode it.
	m_available = false;
	return false;
}

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || m_handle == INVALID_HANDLE_VALUE || m_templateLen == 0)
		return false;

	return false;  // Will be implemented once we know the working method
}
