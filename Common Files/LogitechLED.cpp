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

// --- I/O helpers ---
// SendReport/ReadReport use overlapped I/O for discovery (handle opened with FILE_FLAG_OVERLAPPED)

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
// Queries the device for supported features via IRoot/IFeatureSet,
// then probes any LED-related features found.

bool LogitechLED::TryHIDPPDiscovery(HANDLE h, USHORT outLen, USHORT inLen)
{
	BYTE reportId = (outLen <= 20) ? 0x11 : 0x12;
	Log("  HID++ probe: reportId=0x%02X outLen=%u inLen=%u", reportId, outLen, inLen);

	BYTE devIdxList[] = { 0xFF, 0x01 };

	for (int dii = 0; dii < 2; dii++)
	{
		BYTE devIdx = devIdxList[dii];
		Log("");
		Log("  --- Device index 0x%02X ---", devIdx);

		// Query IRoot (always at index 0) for IFeatureSet (0x0001)
		// IRoot.getFeature(featureId): function 0
		BYTE req[64] = {0};
		req[0] = reportId;
		req[1] = devIdx;
		req[2] = 0x00;                   // IRoot at index 0
		req[3] = (0 << 4) | 0x01;        // function 0, swId 1
		req[4] = 0x00;                   // feature ID high (0x0001)
		req[5] = 0x01;                   // feature ID low

		Log("  TX: %02X %02X %02X %02X %02X %02X",
			req[0], req[1], req[2], req[3], req[4], req[5]);

		if (!SendReport(h, req, outLen))
		{
			Log("  Send failed (err=%lu)", GetLastError());
			continue;
		}

		BYTE resp[64] = {0};
		if (!ReadReport(h, resp, inLen, 2000))
		{
			Log("  No response (timeout 2s)");
			continue;
		}

		Log("  RX: %02X %02X %02X %02X | %02X %02X %02X %02X",
			resp[0], resp[1], resp[2], resp[3],
			resp[4], resp[5], resp[6], resp[7]);

		// HID++ error check
		if (resp[2] == 0xFF)
		{
			Log("  HID++ error: code=0x%02X", resp[5]);
			continue;
		}

		BYTE ifsIdx = resp[4];
		if (ifsIdx == 0)
		{
			Log("  IFeatureSet not supported");
			continue;
		}

		Log("  IFeatureSet at index %d - HID++ 2.0 confirmed!", ifsIdx);

		// --- Get feature count ---
		memset(req, 0, sizeof(req));
		req[0] = reportId;
		req[1] = devIdx;
		req[2] = ifsIdx;
		req[3] = (0 << 4) | 0x01;  // function 0 = getCount

		if (!SendReport(h, req, outLen)) { Log("  getCount send failed"); continue; }

		memset(resp, 0, sizeof(resp));
		if (!ReadReport(h, resp, inLen, 2000)) { Log("  getCount no response"); continue; }

		int featureCount = resp[4];
		Log("  Device has %d features:", featureCount);

		// --- Enumerate ALL features ---
		BYTE bestLedIdx = 0;
		USHORT bestLedFid = 0;
		BYTE probeIdx[8] = {0};
		USHORT probeFid[8] = {0};
		int numProbe = 0;

		for (int fi = 1; fi <= featureCount && fi < 128; fi++)
		{
			memset(req, 0, sizeof(req));
			req[0] = reportId;
			req[1] = devIdx;
			req[2] = ifsIdx;
			req[3] = (1 << 4) | 0x01;  // function 1 = getFeatureID
			req[4] = (BYTE)fi;

			if (!SendReport(h, req, outLen)) continue;

			memset(resp, 0, sizeof(resp));
			if (!ReadReport(h, resp, inLen, 500))
			{ Log("    [%02d] no response", fi); continue; }

			USHORT fid = ((USHORT)resp[4] << 8) | resp[5];
			BYTE ftype = resp[6];

			const char* name = "";
			bool isKnown = true;
			switch (fid)
			{
				case 0x0001: name = " (IFeatureSet)"; break;
				case 0x0003: name = " (DeviceInfo)"; break;
				case 0x0005: name = " (DeviceName)"; break;
				case 0x0007: name = " (FriendlyName)"; break;
				case 0x00C1: name = " (DfuControlUnsigned)"; break;
				case 0x00C2: name = " (DfuControl)"; break;
				case 0x00D0: name = " (Dfu)"; break;
				case 0x1000: name = " (BatteryStatus)"; break;
				case 0x1300: name = " (LEDControl)"; break;
				case 0x1800: name = " (GenericTest)"; break;
				case 0x1802: name = " (DeviceReset)"; break;
				case 0x1814: name = " (ChangeHost)"; break;
				case 0x1BC0: name = " (ReportHIDUsage)"; break;
				case 0x1E00: name = " (EnableHiddenFeatures)"; break;
				case 0x1F1F: name = " (FirmwareProperties)"; break;
				case 0x1982: name = " (Backlight2)"; break;
				case 0x1B04: name = " (SpecialKeys)"; break;
				case 0x2201: name = " (AdjDPI)"; break;
				case 0x8040: name = " (BrightnessCtrl)"; break;
				case 0x8060: name = " (AdjReportRate)"; break;
				case 0x8070: name = " (ColorLEDEffects)"; break;
				case 0x8071: name = " (RGBEffects)"; break;
				case 0x8081: name = " (PerKeyLighting)"; break;
				case 0x8100: name = " (OnboardProfiles)"; break;
				case 0x8120: name = " (GamingAttachments)"; break;
				case 0x8123: name = " (ForceFeedback)"; break;
				case 0x8127: name = " (ForceFeedbackG923)"; break;
				default: isKnown = false; break;
			}

			Log("    [%02d] 0x%04X type=0x%02X%s", fi, fid, ftype, name);

			// Track candidate features to probe for LED control:
			// 1. Known LED features
			// 2. ANY unknown feature in 0x80xx range (could be wheel-specific LEDs)
			if (fid == 0x8070 || fid == 0x1300 || fid == 0x8071 ||
			    fid == 0x8040 || fid == 0x1982)
			{
				// Known LED features - highest priority
				if (bestLedIdx == 0 || fid == 0x8070 ||
				    (bestLedFid != 0x8070 && fid == 0x1300))
				{
					bestLedIdx = (BYTE)fi;
					bestLedFid = fid;
				}
			}

			// Store ALL unknown 0x8xxx features for probing
			if (!isKnown && (fid & 0xF000) == 0x8000 && numProbe < 8)
			{
				probeIdx[numProbe] = (BYTE)fi;
				probeFid[numProbe] = fid;
				numProbe++;
			}
		}

		Log("");

		// --- Probe known LED features first ---
		if (bestLedIdx != 0)
		{
			Log("  >>> Known LED feature: 0x%04X at index %d", bestLedFid, bestLedIdx);
		}
		else
		{
			Log("  No standard LED feature found");
			if (numProbe > 0)
				Log("  Will probe %d unknown 0x8xxx features", numProbe);
		}

		// If no known LED feature, use the first unknown 0x8xxx feature
		if (bestLedIdx == 0 && numProbe > 0)
		{
			bestLedIdx = probeIdx[0];
			bestLedFid = probeFid[0];
			Log("  >>> Trying unknown feature 0x%04X at index %d", bestLedFid, bestLedIdx);
		}

		if (bestLedIdx == 0)
		{
			Log("  Nothing to probe");
			continue;
		}

		// --- Probe ALL unknown 0x8xxx features with getInfo ---
		Log("");
		Log("  === Probing unknown features ===");
		for (int pi = 0; pi < numProbe; pi++)
		{
			memset(req, 0, sizeof(req));
			req[0] = reportId;
			req[1] = devIdx;
			req[2] = probeIdx[pi];
			req[3] = (0 << 4) | 0x01;  // func0 = getInfo

			if (SendReport(h, req, outLen))
			{
				memset(resp, 0, sizeof(resp));
				if (ReadReport(h, resp, inLen, 500))
				{
					if (resp[2] == 0xFF)
						Log("  0x%04X[%d] func0: ERR 0x%02X",
							probeFid[pi], probeIdx[pi], resp[5]);
					else
						Log("  0x%04X[%d] func0: %02X %02X %02X %02X %02X %02X %02X %02X",
							probeFid[pi], probeIdx[pi],
							resp[4], resp[5], resp[6], resp[7],
							resp[8], resp[9], resp[10], resp[11]);
				}
				else
					Log("  0x%04X[%d] func0: no response", probeFid[pi], probeIdx[pi]);
			}
		}

		// --- Try LED commands on each unknown 0x8xxx feature ---
		Log("");
		Log("  === LED attempts on unknown features (WATCH THE WHEEL!) ===");

		for (int pi = 0; pi < numProbe; pi++)
		{
			Log("");
			Log("  --- Feature 0x%04X at index %d ---", probeFid[pi], probeIdx[pi]);

			// Try func1-3 with LED bitmask 0x1F (all 5 LEDs)
			for (BYTE funcId = 1; funcId <= 3; funcId++)
			{
				memset(req, 0, sizeof(req));
				req[0] = reportId;
				req[1] = devIdx;
				req[2] = probeIdx[pi];
				req[3] = (funcId << 4) | 0x01;
				req[4] = 0x1F;  // all LEDs on

				bool ok = SendReport(h, req, outLen);
				BYTE tryResp[64] = {0};
				bool gotResp = ok ? ReadReport(h, tryResp, inLen, 300) : false;

				if (!ok)
					Log("    func%d(0x1F): SEND FAIL", funcId);
				else if (gotResp && tryResp[2] == 0xFF)
					Log("    func%d(0x1F): ERR 0x%02X", funcId, tryResp[5]);
				else if (gotResp)
					Log("    func%d(0x1F): OK resp=%02X %02X %02X %02X",
						funcId, tryResp[4], tryResp[5], tryResp[6], tryResp[7]);
				else
					Log("    func%d(0x1F): OK (no resp)", funcId);

				Sleep(400);
			}
		}

		// Save best guess
		m_method = METHOD_HIDPP;
		m_ledFeatureIdx = bestLedIdx;
		m_ledFunctionId = 1;
		m_deviceIdx = devIdx;
		Log("");
		Log("  HID++ saved: feat=0x%04X idx=%d func=%d devIdx=0x%02X",
			bestLedFid, bestLedIdx, m_ledFunctionId, devIdx);
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
	rpt[3] = 0x1F;  // all 5 LEDs on

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
	Log("=== LogitechLED Init (HID++ Discovery) ===");
	Log("G Hub must be CLOSED for direct HID access!");
	Log("");

	if (m_available) return true;

	HIDCandidate candidates[MAX_CANDIDATES];
	int numCandidates = 0;
	EnumerateCandidates(candidates, &numCandidates);

	Log("");
	Log("Found %d writable collection(s)", numCandidates);

	// Phase 1: HID++ discovery on Logitech vendor page collections
	for (int ci = 0; ci < numCandidates; ci++)
	{
		if (candidates[ci].usagePage != 0xFF43) continue;
		if (candidates[ci].inputReportLen == 0) continue;

		Log("");
		Log("=== HID++ Discovery: col%d (UP=0x%04X Out=%u In=%u) ===",
			ci, candidates[ci].usagePage,
			candidates[ci].outputReportLen, candidates[ci].inputReportLen);

		// Open with overlapped I/O for read support
		HANDLE h = CreateFileA(candidates[ci].path,
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (h == INVALID_HANDLE_VALUE)
		{
			Log("  Cannot open (err=%lu)", GetLastError());
			continue;
		}

		bool found = TryHIDPPDiscovery(h,
			candidates[ci].outputReportLen, candidates[ci].inputReportLen);

		CloseHandle(h);

		if (found)
		{
			// Reopen without overlapped for runtime use
			m_handle = CreateFileA(candidates[ci].path,
				GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL, OPEN_EXISTING, 0, NULL);

			if (m_handle == INVALID_HANDLE_VALUE)
			{
				Log("  Runtime reopen failed (err=%lu)", GetLastError());
				continue;
			}

			m_reportLen = candidates[ci].outputReportLen;
			m_available = true;

			Log("");
			Log("=== LED CONTROL ACTIVE (HID++) ===");

			// Quick test
			SetLEDs(0x1F);
			Sleep(1000);
			ClearLEDs();
			return true;
		}
	}

	// Phase 2: Legacy [F8 12] on all writable collections
	Log("");
	Log("=== Trying legacy LED commands ===");

	for (int ci = 0; ci < numCandidates; ci++)
	{
		Log("");
		Log("--- col%d: UP=0x%04X OutLen=%u ---",
			ci, candidates[ci].usagePage, candidates[ci].outputReportLen);

		HANDLE h = CreateFileA(candidates[ci].path,
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE)
		{
			Log("  Cannot open (err=%lu)", GetLastError());
			continue;
		}

		if (TryLegacy(h, candidates[ci].outputReportLen))
		{
			m_handle = h;
			m_reportLen = candidates[ci].outputReportLen;
			m_available = true;

			Log("");
			Log("=== LED CONTROL ACTIVE (legacy) ===");
			return true;
		}

		CloseHandle(h);
	}

	Log("");
	Log("=== NO WORKING METHOD FOUND ===");
	Log("Make sure G Hub is fully closed (check system tray).");
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
