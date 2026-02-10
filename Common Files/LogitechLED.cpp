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
	, m_method(METHOD_NONE)
	, m_ledFeatureIdx(0)
	, m_ledFunctionId(0)
	, m_deviceIdx(0xFF)
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
	m_method = METHOD_NONE;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

// --- I/O helpers (overlapped for discovery handles) ---

bool LogitechLED::SendReport(HANDLE h, const BYTE* report, USHORT len)
{
	OVERLAPPED ov = {0};
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ov.hEvent) return false;

	DWORD written = 0;
	BOOL ok = WriteFile(h, report, len, &written, &ov);

	if (!ok && GetLastError() == ERROR_IO_PENDING)
	{
		DWORD wait = WaitForSingleObject(ov.hEvent, 2000);
		if (wait == WAIT_OBJECT_0)
			ok = GetOverlappedResult(h, &ov, &written, FALSE);
		else
		{
			CancelIo(h);
			GetOverlappedResult(h, &ov, &written, TRUE);
			ok = FALSE;
		}
	}

	CloseHandle(ov.hEvent);
	return ok && written > 0;
}

bool LogitechLED::ReadReport(HANDLE h, BYTE* report, USHORT len, DWORD timeoutMs)
{
	OVERLAPPED ov = {0};
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!ov.hEvent) return false;

	DWORD bytesRead = 0;
	BOOL ok = ReadFile(h, report, len, &bytesRead, &ov);

	if (!ok && GetLastError() == ERROR_IO_PENDING)
	{
		DWORD wait = WaitForSingleObject(ov.hEvent, timeoutMs);
		if (wait == WAIT_OBJECT_0)
			ok = GetOverlappedResult(h, &ov, &bytesRead, FALSE);
		else
		{
			CancelIo(h);
			GetOverlappedResult(h, &ov, &bytesRead, TRUE);
			ok = FALSE;
		}
	}

	CloseHandle(ov.hEvent);
	return ok && bytesRead > 0;
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
			c.inputReportLen = caps.InputReportByteLength;
			c.usagePage = caps.UsagePage;
		}

		free(detail);
	}

	SetupDiDestroyDeviceInfoList(devInfo);
}

// --- HID++ 2.0 Feature Discovery ---

bool LogitechLED::TryHIDPPDiscovery(HANDLE h, USHORT outLen, USHORT inLen)
{
	BYTE reportId = (outLen <= 20) ? 0x11 : 0x12;
	Log("  HID++ probe: reportId=0x%02X", reportId);

	BYTE devIdx = 0xFF;  // USB direct

	// Query IRoot for IFeatureSet (0x0001)
	BYTE req[64] = {0};
	req[0] = reportId;
	req[1] = devIdx;
	req[2] = 0x00;              // IRoot at index 0
	req[3] = (0 << 4) | 0x01;  // function 0, swId 1
	req[4] = 0x00;
	req[5] = 0x01;              // feature ID 0x0001

	if (!SendReport(h, req, outLen)) { Log("  Send failed"); return false; }

	BYTE resp[64] = {0};
	if (!ReadReport(h, resp, inLen, 2000)) { Log("  No response"); return false; }

	Log("  RX: %02X %02X %02X %02X | %02X %02X %02X %02X",
		resp[0], resp[1], resp[2], resp[3], resp[4], resp[5], resp[6], resp[7]);

	if (resp[2] == 0xFF) { Log("  HID++ error: 0x%02X", resp[5]); return false; }

	BYTE ifsIdx = resp[4];
	if (ifsIdx == 0) { Log("  IFeatureSet not found"); return false; }

	Log("  IFeatureSet at index %d - HID++ 2.0 confirmed!", ifsIdx);

	// Get feature count
	memset(req, 0, sizeof(req));
	req[0] = reportId; req[1] = devIdx; req[2] = ifsIdx;
	req[3] = (0 << 4) | 0x01;
	if (!SendReport(h, req, outLen)) return false;
	memset(resp, 0, sizeof(resp));
	if (!ReadReport(h, resp, inLen, 2000)) return false;

	int featureCount = resp[4];
	Log("  Device has %d features:", featureCount);

	// Enumerate ALL features
	BYTE enableIdx = 0;   // 0x1E00 EnableHiddenFeatures
	BYTE feat807AIdx = 0; // 0x807A (suspected LED)

	for (int fi = 1; fi <= featureCount && fi < 128; fi++)
	{
		memset(req, 0, sizeof(req));
		req[0] = reportId; req[1] = devIdx; req[2] = ifsIdx;
		req[3] = (1 << 4) | 0x01;
		req[4] = (BYTE)fi;

		if (!SendReport(h, req, outLen)) continue;
		memset(resp, 0, sizeof(resp));
		if (!ReadReport(h, resp, inLen, 500)) continue;

		USHORT fid = ((USHORT)resp[4] << 8) | resp[5];

		const char* name = "";
		switch (fid)
		{
			case 0x0001: name = " (IFeatureSet)"; break;
			case 0x0003: name = " (DeviceInfo)"; break;
			case 0x0005: name = " (DeviceName)"; break;
			case 0x00C1: name = " (DfuControlUnsigned)"; break;
			case 0x1800: name = " (GenericTest)"; break;
			case 0x1802: name = " (DeviceReset)"; break;
			case 0x1BC0: name = " (ReportHIDUsage)"; break;
			case 0x1E00: name = " (EnableHiddenFeatures)"; break;
			case 0x1F1F: name = " (FirmwareProperties)"; break;
			case 0x8120: name = " (GamingAttachments)"; break;
			case 0x8123: name = " (ForceFeedback)"; break;
			case 0x8127: name = " (ForceFeedbackG923)"; break;
			case 0x807A: name = " *** SUSPECTED LED ***"; break;
		}

		Log("    [%02d] 0x%04X type=0x%02X%s", fi, fid, resp[6], name);

		if (fid == 0x1E00) enableIdx = (BYTE)fi;
		if (fid == 0x807A) feat807AIdx = (BYTE)fi;
	}

	// --- Step 1: Enable hidden features ---
	if (enableIdx > 0)
	{
		Log("");
		Log("  === Enabling hidden features (0x1E00 at idx %d) ===", enableIdx);

		// func0 = getEnableHiddenFeatures
		memset(req, 0, sizeof(req));
		req[0] = reportId; req[1] = devIdx; req[2] = enableIdx;
		req[3] = (0 << 4) | 0x01;
		if (SendReport(h, req, outLen))
		{
			memset(resp, 0, sizeof(resp));
			if (ReadReport(h, resp, inLen, 500))
				Log("  getEnable: %02X %02X", resp[4], resp[5]);
		}

		// func1 = setEnableHiddenFeatures(0x01) - enable
		memset(req, 0, sizeof(req));
		req[0] = reportId; req[1] = devIdx; req[2] = enableIdx;
		req[3] = (1 << 4) | 0x01;
		req[4] = 0x01;  // enable
		if (SendReport(h, req, outLen))
		{
			memset(resp, 0, sizeof(resp));
			if (ReadReport(h, resp, inLen, 500))
			{
				if (resp[2] == 0xFF)
					Log("  setEnable(1): ERR 0x%02X", resp[5]);
				else
					Log("  setEnable(1): OK %02X %02X", resp[4], resp[5]);
			}
		}

		// func1 with 0xFF (max enable)
		memset(req, 0, sizeof(req));
		req[0] = reportId; req[1] = devIdx; req[2] = enableIdx;
		req[3] = (1 << 4) | 0x01;
		req[4] = 0xFF;
		if (SendReport(h, req, outLen))
		{
			memset(resp, 0, sizeof(resp));
			if (ReadReport(h, resp, inLen, 500))
			{
				if (resp[2] == 0xFF)
					Log("  setEnable(0xFF): ERR 0x%02X", resp[5]);
				else
					Log("  setEnable(0xFF): OK %02X %02X", resp[4], resp[5]);
			}
		}

		// Re-enumerate to see if new features appeared
		Log("");
		Log("  Re-enumerating features...");
		memset(req, 0, sizeof(req));
		req[0] = reportId; req[1] = devIdx; req[2] = ifsIdx;
		req[3] = (0 << 4) | 0x01;
		if (SendReport(h, req, outLen))
		{
			memset(resp, 0, sizeof(resp));
			if (ReadReport(h, resp, inLen, 500))
			{
				int newCount = resp[4];
				Log("  Feature count after enable: %d (was %d)", newCount, featureCount);

				if (newCount > featureCount)
				{
					for (int fi = featureCount + 1; fi <= newCount && fi < 128; fi++)
					{
						memset(req, 0, sizeof(req));
						req[0] = reportId; req[1] = devIdx; req[2] = ifsIdx;
						req[3] = (1 << 4) | 0x01;
						req[4] = (BYTE)fi;
						if (!SendReport(h, req, outLen)) continue;
						memset(resp, 0, sizeof(resp));
						if (!ReadReport(h, resp, inLen, 500)) continue;
						USHORT fid = ((USHORT)resp[4] << 8) | resp[5];
						Log("    NEW [%02d] 0x%04X type=0x%02X", fi, fid, resp[6]);
						if (fid == 0x807A) feat807AIdx = (BYTE)fi;
					}
				}
			}
		}
	}

	// --- Step 2: Probe 0x807A ---
	if (feat807AIdx > 0)
	{
		Log("");
		Log("  === Probing 0x807A at index %d ===", feat807AIdx);

		// func0 getInfo
		memset(req, 0, sizeof(req));
		req[0] = reportId; req[1] = devIdx; req[2] = feat807AIdx;
		req[3] = (0 << 4) | 0x01;
		if (SendReport(h, req, outLen))
		{
			memset(resp, 0, sizeof(resp));
			if (ReadReport(h, resp, inLen, 500))
				Log("  func0: %02X %02X %02X %02X", resp[4], resp[5], resp[6], resp[7]);
		}

		// Read current state
		memset(req, 0, sizeof(req));
		req[0] = reportId; req[1] = devIdx; req[2] = feat807AIdx;
		req[3] = (1 << 4) | 0x01;
		if (SendReport(h, req, outLen))
		{
			memset(resp, 0, sizeof(resp));
			if (ReadReport(h, resp, inLen, 500))
				Log("  func1 state: %02X %02X %02X %02X", resp[4], resp[5], resp[6], resp[7]);
		}

		// Try func2 with targeted params, then check state after each
		Log("");
		Log("  === func2 attempts + state check (WATCH WHEEL!) ===");

		struct { BYTE p[8]; const char* desc; } tries[] = {
			{{0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, "mask=0x1F"},
			{{0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00}, "5x 0x01"},
			{{0x01,0x02,0x03,0x04,0x05,0x00,0x00,0x00}, "1,2,3,4,5"},
			{{0x05,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x00}, "n=5,5xFF"},
			{{0x00,0x05,0x1F,0x00,0x00,0x00,0x00,0x00}, "0,5,mask"},
			{{0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, "mode=1"},
		};

		for (int t = 0; t < 6; t++)
		{
			memset(req, 0, sizeof(req));
			req[0] = reportId; req[1] = devIdx; req[2] = feat807AIdx;
			req[3] = (2 << 4) | 0x01;
			memcpy(&req[4], tries[t].p, 8);

			bool ok = SendReport(h, req, outLen);
			BYTE r[64] = {0};
			bool got = ok ? ReadReport(h, r, inLen, 300) : false;

			// Now read state
			memset(req, 0, sizeof(req));
			req[0] = reportId; req[1] = devIdx; req[2] = feat807AIdx;
			req[3] = (1 << 4) | 0x01;
			SendReport(h, req, outLen);
			BYTE st[64] = {0};
			ReadReport(h, st, inLen, 300);

			Log("  func2(%-12s) -> state: %02X %02X %02X %02X",
				tries[t].desc, st[4], st[5], st[6], st[7]);

			Sleep(500);
		}
	}

	// --- Step 3: Try sending LED command via col0 (report 0x11) ---
	// Discovery works on col1 (0x12), but LEDs might be wired to col0 (0x11)
	if (feat807AIdx > 0)
	{
		Log("");
		Log("  === Trying 0x807A commands via col0 (report 0x11) ===");

		// Find col0 path (UP=0xFF43, Out=20)
		// We need to open it separately - caller passes the col1 handle
		// So we search candidates again for the 20-byte collection
		HIDCandidate col0cands[MAX_CANDIDATES];
		int col0count = 0;
		EnumerateCandidates(col0cands, &col0count);

		for (int ci = 0; ci < col0count; ci++)
		{
			if (col0cands[ci].usagePage != 0xFF43) continue;
			if (col0cands[ci].outputReportLen != 20) continue;

			HANDLE h0 = CreateFileA(col0cands[ci].path,
				GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
			if (h0 == INVALID_HANDLE_VALUE)
			{
				Log("  Cannot open col0 (err=%lu)", GetLastError());
				continue;
			}

			Log("  col0 opened (Out=20)");

			// Try HID++ commands on col0 using feature index from col1
			BYTE cmd0x11[20] = {0};

			// func2 with bitmask via 0x11
			cmd0x11[0] = 0x11;
			cmd0x11[1] = devIdx;
			cmd0x11[2] = feat807AIdx;
			cmd0x11[3] = (2 << 4) | 0x01;
			cmd0x11[4] = 0x1F;

			bool ok = SendReport(h0, cmd0x11, 20);
			Log("  col0 func2(0x1F): %s", ok ? "OK" : "FAIL");

			// Try reading response from col0
			BYTE r0[20] = {0};
			if (ok && ReadReport(h0, r0, 20, 500))
				Log("  col0 resp: %02X %02X %02X %02X %02X %02X",
					r0[0], r0[1], r0[2], r0[3], r0[4], r0[5]);
			else
				Log("  col0 no response");

			Sleep(500);

			// Try func1 on col0
			memset(cmd0x11, 0, sizeof(cmd0x11));
			cmd0x11[0] = 0x11;
			cmd0x11[1] = devIdx;
			cmd0x11[2] = feat807AIdx;
			cmd0x11[3] = (1 << 4) | 0x01;
			cmd0x11[4] = 0x1F;

			ok = SendReport(h0, cmd0x11, 20);
			Log("  col0 func1(0x1F): %s", ok ? "OK" : "FAIL");

			memset(r0, 0, sizeof(r0));
			if (ok && ReadReport(h0, r0, 20, 500))
				Log("  col0 resp: %02X %02X %02X %02X %02X %02X",
					r0[0], r0[1], r0[2], r0[3], r0[4], r0[5]);

			Sleep(500);

			// Also try legacy [F8 12] on col0 AFTER enabling hidden features
			BYTE leg[20] = {0};
			leg[0] = 0x11;
			leg[1] = 0xF8;
			leg[2] = 0x12;
			leg[3] = 0x1F;

			ok = SendReport(h0, leg, 20);
			Log("  col0 legacy [F8 12 1F]: %s", ok ? "OK" : "FAIL");

			Sleep(500);

			// Try legacy with extra byte at [7] = 0x01 (some implementations)
			memset(leg, 0, sizeof(leg));
			leg[0] = 0x11;
			leg[1] = 0xF8;
			leg[2] = 0x12;
			leg[3] = 0x1F;
			leg[7] = 0x01;

			ok = SendReport(h0, leg, 20);
			Log("  col0 legacy [F8 12 1F .. 01]: %s", ok ? "OK" : "FAIL");

			CloseHandle(h0);
			break;
		}
	}

	// Save whatever we found for runtime attempts
	if (feat807AIdx > 0)
	{
		m_ledFeatureIdx = feat807AIdx;
		m_ledFunctionId = 2;
		m_deviceIdx = devIdx;
		Log("");
		Log("  Feature 0x807A saved at index %d", feat807AIdx);
		return true;
	}

	return false;
}

// --- Legacy [F8 12] command (G29/G920/G923-PS) ---

bool LogitechLED::TryLegacy(HANDLE h, USHORT outLen)
{
	BYTE reportId = (outLen <= 20) ? 0x11 : 0x12;

	BYTE rpt[64] = {0};
	rpt[0] = reportId;
	rpt[1] = 0xF8;
	rpt[2] = 0x12;
	rpt[3] = 0x1F;

	DWORD written = 0;
	BOOL ok = WriteFile(h, rpt, outLen, &written, NULL);
	bool success = ok && written > 0;

	Log("  Legacy [%02X F8 12 1F] -> %s", reportId, success ? "OK" : "FAIL");
	if (!success) { Log("    err=%lu", GetLastError()); return false; }

	Sleep(500);
	rpt[3] = 0x00;
	WriteFile(h, rpt, outLen, &written, NULL);

	m_method = METHOD_LEGACY;
	return true;
}

// --- Init ---

bool LogitechLED::Init()
{
	Log("=== LogitechLED Init (HID++ Discovery v3) ===");
	Log("");

	if (m_available) return true;

	HIDCandidate candidates[MAX_CANDIDATES];
	int numCandidates = 0;
	EnumerateCandidates(candidates, &numCandidates);

	Log("");
	Log("Found %d writable collection(s)", numCandidates);

	// Phase 1: HID++ discovery on 64-byte collection (col1, report 0x12)
	for (int ci = 0; ci < numCandidates; ci++)
	{
		if (candidates[ci].usagePage != 0xFF43) continue;
		if (candidates[ci].inputReportLen == 0) continue;
		if (candidates[ci].outputReportLen < 64) continue;  // Only col1

		Log("");
		Log("=== HID++ Discovery on col%d (Out=%u In=%u) ===",
			ci, candidates[ci].outputReportLen, candidates[ci].inputReportLen);

		HANDLE h = CreateFileA(candidates[ci].path,
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (h == INVALID_HANDLE_VALUE) { Log("  Open failed"); continue; }

		bool found = TryHIDPPDiscovery(h,
			candidates[ci].outputReportLen, candidates[ci].inputReportLen);

		CloseHandle(h);

		if (found)
		{
			// Reopen for runtime
			m_handle = CreateFileA(candidates[ci].path,
				GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL, OPEN_EXISTING, 0, NULL);

			if (m_handle == INVALID_HANDLE_VALUE) continue;

			m_reportLen = candidates[ci].outputReportLen;
			m_method = METHOD_HIDPP;
			m_available = true;

			Log("");
			Log("=== LED CONTROL ACTIVE (HID++) ===");
			SetLEDs(0x1F);
			Sleep(1000);
			ClearLEDs();
			return true;
		}
	}

	// Phase 2: Legacy on all collections
	Log("");
	Log("=== Trying legacy LED commands ===");

	for (int ci = 0; ci < numCandidates; ci++)
	{
		Log("--- col%d: UP=0x%04X OutLen=%u ---",
			ci, candidates[ci].usagePage, candidates[ci].outputReportLen);

		HANDLE h = CreateFileA(candidates[ci].path,
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE) continue;

		if (TryLegacy(h, candidates[ci].outputReportLen))
		{
			m_handle = h;
			m_reportLen = candidates[ci].outputReportLen;
			m_available = true;
			Log("=== LED CONTROL ACTIVE (legacy) ===");
			return true;
		}

		CloseHandle(h);
	}

	Log("");
	Log("=== NO WORKING METHOD FOUND ===");
	return false;
}

// --- Runtime LED control ---

static int g_setLedsCallCount = 0;

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || m_handle == INVALID_HANDLE_VALUE)
		return false;

	BYTE rpt[64] = {0};
	BYTE reportId = (m_reportLen <= 20) ? 0x11 : 0x12;

	if (m_method == METHOD_HIDPP)
	{
		rpt[0] = reportId;
		rpt[1] = m_deviceIdx;
		rpt[2] = m_ledFeatureIdx;
		rpt[3] = (m_ledFunctionId << 4) | 0x01;
		rpt[4] = ledMask & 0x1F;
	}
	else
	{
		rpt[0] = reportId;
		rpt[1] = 0xF8;
		rpt[2] = 0x12;
		rpt[3] = ledMask & 0x1F;
	}

	DWORD written = 0;
	BOOL ok = WriteFile(m_handle, rpt, m_reportLen, &written, NULL);
	bool success = ok && written > 0;

	g_setLedsCallCount++;
	if (g_setLedsCallCount <= 20 || !success)
	{
		Log("SetLEDs(0x%02X) [%s] -> %s (#%d)",
			ledMask,
			m_method == METHOD_HIDPP ? "HID++" : "legacy",
			success ? "OK" : "FAIL",
			g_setLedsCallCount);
		if (!success) Log("  err=%lu", GetLastError());
	}

	return success;
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
