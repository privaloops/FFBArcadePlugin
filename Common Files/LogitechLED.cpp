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

// HID++ constants
static const BYTE HIDPP_LONG        = 0x11;  // 20 bytes
static const BYTE HIDPP_DEVICE_USB  = 0x01;  // USB-connected device index

// HID++ feature IDs
static const USHORT FEAT_FEATURE_SET = 0x0001;
static const USHORT FEAT_LED_CONTROL = 0x1300;
static const USHORT FEAT_LED_EFFECTS = 0x8070;
static const USHORT FEAT_RGB_STATE   = 0x8071;

// Legacy LED command
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

static bool IsG923Xbox(USHORT pid)
{
	return pid == 0xC26E || pid == 0xC26D;
}

// --- LogitechLED ---

LogitechLED::LogitechLED()
	: m_writeHandle(INVALID_HANDLE_VALUE)
	, m_readHandle(INVALID_HANDLE_VALUE)
	, m_available(false)
	, m_reportLen(0)
	, m_inputReportLen(0)
	, m_method(METHOD_NONE)
	, m_ledFeatureIndex(0)
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

	char path[512];
	USHORT outLen, inLen;
	bool isVendor;

	if (!FindDevice(path, sizeof(path), &outLen, &inLen, &isVendor))
	{
		Log("Init FAILED - no device found");
		return false;
	}

	// Open sync handle for writes
	m_writeHandle = CreateFileA(path,
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (m_writeHandle == INVALID_HANDLE_VALUE)
	{
		Log("Failed to open write handle, err=%lu", GetLastError());
		return false;
	}

	// Open overlapped handle for reads (HID++ responses)
	m_readHandle = CreateFileA(path,
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (m_readHandle == INVALID_HANDLE_VALUE)
	{
		Log("Failed to open read handle, err=%lu", GetLastError());
		CloseHandle(m_writeHandle);
		m_writeHandle = INVALID_HANDLE_VALUE;
		return false;
	}

	m_reportLen = outLen;
	m_inputReportLen = inLen;

	Log("Handles opened, OutLen=%u InLen=%u Vendor=%s", outLen, inLen, isVendor ? "Y" : "N");

	if (isVendor && outLen >= 20)
	{
		// HID++ collection -> try feature discovery via WriteFile
		Log("Vendor collection detected, attempting HID++ feature discovery...");
		if (InitHIDPP())
		{
			m_method = METHOD_HIDPP;
			m_available = true;
			Log("Init SUCCESS - HID++ LED feature index=%u", m_ledFeatureIndex);
			return true;
		}
		Log("HID++ feature discovery failed");
	}

	// Legacy fallback (G29, G923 PS, or HID++ failed)
	Log("Trying legacy protocol...");
	if (SetLEDsLegacy(0x1F))
	{
		m_method = METHOD_LEGACY;
		m_available = true;
		SetLEDsLegacy(0x00);  // Turn off after test
		Log("Init SUCCESS - legacy protocol");
		return true;
	}

	Log("Init FAILED - no working LED method");
	Close();
	return false;
}

void LogitechLED::Close()
{
	if (m_readHandle != INVALID_HANDLE_VALUE)
	{
		CancelIo(m_readHandle);
		CloseHandle(m_readHandle);
		m_readHandle = INVALID_HANDLE_VALUE;
	}
	if (m_writeHandle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_writeHandle);
		m_writeHandle = INVALID_HANDLE_VALUE;
	}
	m_available = false;
	m_method = METHOD_NONE;
	m_ledFeatureIndex = 0;
}

bool LogitechLED::IsAvailable() const
{
	return m_available && m_method != METHOD_NONE;
}

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || m_writeHandle == INVALID_HANDLE_VALUE)
		return false;

	if (m_method == METHOD_HIDPP)
		return SetLEDsHIDPP(ledMask);
	else if (m_method == METHOD_LEGACY)
		return SetLEDsLegacy(ledMask);

	return false;
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
// Device enumeration - find best Logitech wheel collection
// Prefers vendor collection (for HID++) if available
// -------------------------------------------------------

bool LogitechLED::FindDevice(char* outPath, int pathSize,
                              USHORT* outReportLen, USHORT* outInputLen,
                              bool* outIsVendor)
{
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	HDEVINFO devInfo = SetupDiGetClassDevsA(
		&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devInfo == INVALID_HANDLE_VALUE) return false;

	SP_DEVICE_INTERFACE_DATA ifData;
	ifData.cbSize = sizeof(ifData);

	char bestPath[512] = {0};
	USHORT bestOutLen = 0, bestInLen = 0;
	bool bestIsVendor = false;
	bool found = false;

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

		bool isVendor = (caps.UsagePage >= 0xFF00);

		Log("Found PID=0x%04X UP=0x%04X U=0x%04X In=%u Out=%u Path=%s",
			attrs.ProductID, caps.UsagePage, caps.Usage,
			caps.InputReportByteLength, caps.OutputReportByteLength,
			detail->DevicePath);

		HidD_FreePreparsedData(pp);
		CloseHandle(h);

		if (caps.OutputReportByteLength == 0) { free(detail); continue; }

		// Prefer: vendor collection with OutLen=20 (HID++ long), then largest output
		bool isBetter = false;
		if (isVendor && caps.OutputReportByteLength == 20)
		{
			// Ideal for HID++ long reports
			if (!found || !bestIsVendor || bestOutLen != 20)
				isBetter = true;
		}
		else if (!found || (!bestIsVendor && caps.OutputReportByteLength > bestOutLen))
		{
			if (caps.OutputReportByteLength >= 7)
				isBetter = true;
		}

		if (isBetter)
		{
			strcpy_s(bestPath, detail->DevicePath);
			bestOutLen = caps.OutputReportByteLength;
			bestInLen = caps.InputReportByteLength;
			bestIsVendor = isVendor;
			found = true;
		}

		free(detail);
	}

	SetupDiDestroyDeviceInfoList(devInfo);

	if (found)
	{
		strcpy_s(outPath, pathSize, bestPath);
		*outReportLen = bestOutLen;
		*outInputLen = bestInLen;
		*outIsVendor = bestIsVendor;
		Log("Selected OutLen=%u InLen=%u Vendor=%s", bestOutLen, bestInLen, bestIsVendor ? "Y" : "N");
	}

	return found;
}

// -------------------------------------------------------
// Legacy protocol (G29, G923 PS)
// -------------------------------------------------------

bool LogitechLED::SetLEDsLegacy(BYTE ledMask)
{
	BYTE* rpt = (BYTE*)calloc(m_reportLen, 1);
	if (!rpt) return false;

	// Try HidD_SetOutputReport with report ID 0xF8
	rpt[0] = CMD_LED;
	rpt[1] = SUBCMD_LED;
	rpt[2] = ledMask & 0x1F;
	rpt[7] = 0x01;

	if (HidD_SetOutputReport(m_writeHandle, rpt, m_reportLen))
	{
		free(rpt);
		return true;
	}

	// Try report ID 0x00
	memset(rpt, 0, m_reportLen);
	rpt[0] = 0x00;
	rpt[1] = CMD_LED;
	rpt[2] = SUBCMD_LED;
	rpt[3] = ledMask & 0x1F;
	rpt[8] = 0x01;

	BOOL ok = HidD_SetOutputReport(m_writeHandle, rpt, m_reportLen);
	free(rpt);
	return ok == TRUE;
}

// -------------------------------------------------------
// HID++ protocol via WriteFile (G923 Xbox)
// -------------------------------------------------------

bool LogitechLED::HIDPPSendLong(BYTE deviceIdx, BYTE featureIdx,
                                 BYTE funcSwId, const BYTE* params, int paramLen)
{
	BYTE rpt[20] = {0};
	rpt[0] = HIDPP_LONG;     // 0x11
	rpt[1] = deviceIdx;
	rpt[2] = featureIdx;
	rpt[3] = funcSwId;

	if (params && paramLen > 0)
	{
		int n = (paramLen > 16) ? 16 : paramLen;
		memcpy(&rpt[4], params, n);
	}

	// WriteFile on interrupt OUT endpoint (HidD_SetOutputReport fails with err=31)
	DWORD written = 0;
	BOOL ok = WriteFile(m_writeHandle, rpt, m_reportLen, &written, NULL);

	if (!ok || written == 0)
	{
		Log("HID++ TX FAIL [%02X %02X %02X %02X %02X %02X] err=%lu",
			rpt[0], rpt[1], rpt[2], rpt[3], rpt[4], rpt[5], GetLastError());
		return false;
	}

	Log("HID++ TX OK [%02X %02X %02X %02X %02X %02X %02X %02X]",
		rpt[0], rpt[1], rpt[2], rpt[3], rpt[4], rpt[5], rpt[6], rpt[7]);
	return true;
}

bool LogitechLED::HIDPPRecv(BYTE* response, int responseLen, DWORD timeoutMs)
{
	if (m_inputReportLen == 0 || m_readHandle == INVALID_HANDLE_VALUE)
		return false;

	BYTE* buf = (BYTE*)calloc(m_inputReportLen, 1);
	if (!buf) return false;

	OVERLAPPED ov = {0};
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	DWORD bytesRead = 0;

	BOOL ok = ReadFile(m_readHandle, buf, m_inputReportLen, &bytesRead, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING)
	{
		DWORD wait = WaitForSingleObject(ov.hEvent, timeoutMs);
		if (wait == WAIT_OBJECT_0)
			GetOverlappedResult(m_readHandle, &ov, &bytesRead, FALSE);
		else
		{
			CancelIo(m_readHandle);
			GetOverlappedResult(m_readHandle, &ov, &bytesRead, TRUE);
			bytesRead = 0;
		}
	}

	CloseHandle(ov.hEvent);

	if (bytesRead > 0)
	{
		int n = (bytesRead < (DWORD)responseLen) ? (int)bytesRead : responseLen;
		memcpy(response, buf, n);
		Log("HID++ RX [%02X %02X %02X %02X %02X %02X %02X %02X] (%lu bytes)",
			buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
			bytesRead);
	}
	else
	{
		Log("HID++ RX timeout (%lu ms)", timeoutMs);
	}

	free(buf);
	return bytesRead > 0;
}

BYTE LogitechLED::HIDPPDiscoverFeature(USHORT featureId)
{
	// Query ROOT (feature index 0x00), function 0 (getFeatureIndex), SW ID 1
	BYTE params[16] = {0};
	params[0] = (BYTE)(featureId >> 8);
	params[1] = (BYTE)(featureId & 0xFF);

	if (!HIDPPSendLong(HIDPP_DEVICE_USB, 0x00, 0x01, params, 2))
		return 0;

	BYTE resp[20] = {0};
	if (!HIDPPRecv(resp, sizeof(resp), 3000))
		return 0;

	// Error response: byte[2] == 0xFF
	if (resp[2] == 0xFF)
	{
		Log("Feature 0x%04X -> HID++ error (code=0x%02X)", featureId, resp[5]);
		return 0;
	}

	BYTE idx = resp[4];
	Log("Feature 0x%04X -> index=%u", featureId, idx);
	return idx;
}

void LogitechLED::HIDPPEnumerateAllFeatures()
{
	BYTE fsIdx = HIDPPDiscoverFeature(FEAT_FEATURE_SET);
	if (fsIdx == 0)
	{
		Log("Cannot enumerate: IFeatureSet not found");
		return;
	}

	// getCount: function 0, SW ID 1
	if (!HIDPPSendLong(HIDPP_DEVICE_USB, fsIdx, 0x01, NULL, 0))
		return;

	BYTE resp[20] = {0};
	if (!HIDPPRecv(resp, sizeof(resp), 3000))
		return;

	int count = resp[4];
	Log("Device has %d features:", count);

	for (int i = 1; i <= count && i <= 64; i++)
	{
		BYTE p[16] = {0};
		p[0] = (BYTE)i;

		if (!HIDPPSendLong(HIDPP_DEVICE_USB, fsIdx, 0x11, p, 1))
			continue;

		BYTE r[20] = {0};
		if (!HIDPPRecv(r, sizeof(r), 3000))
			continue;

		USHORT fid = ((USHORT)r[4] << 8) | r[5];
		Log("  [%2d] Feature 0x%04X (type=0x%02X)", i, fid, r[6]);
	}
}

bool LogitechLED::InitHIDPP()
{
	Log("InitHIDPP: starting feature discovery via WriteFile");

	// Ping: ROOT (index 0), function 1, SW ID 1
	BYTE pingParams[16] = {0, 0, 0xAA};
	if (HIDPPSendLong(HIDPP_DEVICE_USB, 0x00, 0x11, pingParams, 3))
	{
		BYTE resp[20] = {0};
		if (HIDPPRecv(resp, sizeof(resp), 3000))
		{
			if (resp[2] != 0xFF)
				Log("Ping OK - HID++ v%u.%u", resp[4], resp[5]);
			else
				Log("Ping error (code=0x%02X)", resp[5]);
		}
		else
		{
			Log("Ping timeout");
		}
	}
	else
	{
		Log("Ping send failed");
		return false;
	}

	// Try known LED feature IDs
	struct { USHORT id; const char* name; } features[] = {
		{ FEAT_LED_CONTROL, "LED_CONTROL(0x1300)" },
		{ FEAT_LED_EFFECTS, "LED_EFFECTS(0x8070)" },
		{ FEAT_RGB_STATE,   "RGB_STATE(0x8071)"   },
	};

	for (int i = 0; i < 3; i++)
	{
		Log("Trying %s...", features[i].name);
		BYTE idx = HIDPPDiscoverFeature(features[i].id);
		if (idx > 0)
		{
			m_ledFeatureIndex = idx;
			Log("Found %s at index %u", features[i].name, idx);
			return true;
		}
	}

	// None found - enumerate ALL features for debugging
	Log("No known LED feature. Enumerating all...");
	HIDPPEnumerateAllFeatures();

	return false;
}

bool LogitechLED::SetLEDsHIDPP(BYTE ledMask)
{
	if (m_ledFeatureIndex == 0)
		return false;

	// Try function 3 (common setState), SW ID 1
	BYTE params[16] = {0};
	params[0] = ledMask & 0x1F;

	return HIDPPSendLong(HIDPP_DEVICE_USB, m_ledFeatureIndex, 0x31, params, 1);
}
