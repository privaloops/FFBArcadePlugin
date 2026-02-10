#include "LogitechLED.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// Logitech Vendor ID
static const USHORT LOGITECH_VID = 0x046D;

// Supported Product IDs
static const USHORT G923_PID_XBOX  = 0xC26E;
static const USHORT G923_PID_XBOX2 = 0xC26D;
static const USHORT G923_PID_PS    = 0xC267;
static const USHORT G29_PID        = 0xC24F;

// HID++ report IDs
static const BYTE HIDPP_LONG = 0x11;   // 20 bytes total

// HID++ USB device index
static const BYTE HIDPP_DEV = 0x01;

// HID++ feature IDs to try for LED control
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

// --- Helpers ---

static bool IsG923Xbox(USHORT pid)
{
	return pid == G923_PID_XBOX || pid == G923_PID_XBOX2;
}

static bool IsKnownPID(USHORT pid)
{
	return pid == G923_PID_XBOX || pid == G923_PID_XBOX2 ||
	       pid == G923_PID_PS || pid == G29_PID;
}

// --- LogitechLED implementation ---

LogitechLED::LogitechLED()
	: m_writeHandle(INVALID_HANDLE_VALUE)
	, m_readHandle(INVALID_HANDLE_VALUE)
	, m_available(false)
	, m_outputReportLength(0)
	, m_inputReportLength(0)
	, m_productId(0)
	, m_useHIDPP(false)
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

	if (!FindAndOpenDevice())
	{
		Log("Init FAILED - no device found");
		return false;
	}

	if (IsG923Xbox(m_productId))
	{
		Log("G923 Xbox detected (PID=0x%04X) -> HID++ protocol", m_productId);
		m_useHIDPP = true;

		if (InitHIDPP())
		{
			m_available = true;
			Log("Init SUCCESS - HID++ LED feature index=%u", m_ledFeatureIndex);
		}
		else
		{
			Log("HID++ LED discovery failed, will try legacy fallback");
			m_useHIDPP = false;
			m_available = true;
		}
	}
	else
	{
		Log("Legacy device (PID=0x%04X)", m_productId);
		m_useHIDPP = false;
		m_available = true;
		Log("Init SUCCESS - legacy protocol");
	}

	return m_available;
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
	m_useHIDPP = false;
	m_ledFeatureIndex = 0;
	m_outputReportLength = 0;
	m_inputReportLength = 0;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || m_writeHandle == INVALID_HANDLE_VALUE)
		return false;

	if (m_useHIDPP)
		return SetLEDsHIDPP(ledMask);
	else
		return SetLEDsLegacy(ledMask);
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
// Device enumeration
// -------------------------------------------------------

bool LogitechLED::FindAndOpenDevice()
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

	char bestPath[512] = {0};
	USHORT bestOutLen = 0, bestInLen = 0, bestPID = 0;
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
		{
			free(detail);
			continue;
		}

		// Open temporarily to check attributes
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

		bool isVendor = (caps.UsagePage >= 0xFF00);

		Log("Found PID=0x%04X UP=0x%04X U=0x%04X In=%u Out=%u Vendor=%s Path=%s",
			attrs.ProductID, caps.UsagePage, caps.Usage,
			caps.InputReportByteLength, caps.OutputReportByteLength,
			isVendor ? "Y" : "N", detail->DevicePath);

		HidD_FreePreparsedData(pp);
		CloseHandle(h);

		if (caps.OutputReportByteLength == 0) { free(detail); continue; }

		// Selection: G923 Xbox needs vendor collection (HID++), others need largest output
		bool isBetter = false;
		if (IsG923Xbox(attrs.ProductID))
		{
			if (isVendor && caps.OutputReportByteLength >= 20)
			{
				if (!found || !bestIsVendor)
					isBetter = true;
			}
		}
		else
		{
			if (caps.OutputReportByteLength >= 7)
			{
				if (!found || caps.OutputReportByteLength > bestOutLen)
					isBetter = true;
			}
		}

		if (isBetter)
		{
			strcpy_s(bestPath, detail->DevicePath);
			bestOutLen = caps.OutputReportByteLength;
			bestInLen = caps.InputReportByteLength;
			bestPID = attrs.ProductID;
			bestIsVendor = isVendor;
			found = true;
		}

		free(detail);
	}

	SetupDiDestroyDeviceInfoList(devInfo);

	if (!found)
	{
		Log("No suitable HID collection found");
		return false;
	}

	// Open sync handle for writes (HidD_SetOutputReport)
	m_writeHandle = CreateFileA(bestPath,
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (m_writeHandle == INVALID_HANDLE_VALUE)
	{
		Log("Failed to open write handle, err=%lu", GetLastError());
		return false;
	}

	// Open async handle for reads (ReadFile with overlapped)
	m_readHandle = CreateFileA(bestPath,
		GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	if (m_readHandle == INVALID_HANDLE_VALUE)
	{
		Log("Failed to open read handle, err=%lu", GetLastError());
		CloseHandle(m_writeHandle);
		m_writeHandle = INVALID_HANDLE_VALUE;
		return false;
	}

	m_outputReportLength = bestOutLen;
	m_inputReportLength = bestInLen;
	m_productId = bestPID;

	Log("Selected PID=0x%04X OutLen=%u InLen=%u Vendor=%s",
		bestPID, bestOutLen, bestInLen, bestIsVendor ? "Y" : "N");

	return true;
}

// -------------------------------------------------------
// Legacy protocol (G29, G923 PS)
// -------------------------------------------------------

bool LogitechLED::SetLEDsLegacy(BYTE ledMask)
{
	BYTE* rpt = (BYTE*)calloc(m_outputReportLength, 1);
	if (!rpt) return false;

	// Method 1: HidD_SetOutputReport, report ID = 0xF8
	rpt[0] = CMD_LED;
	rpt[1] = SUBCMD_LED;
	rpt[2] = ledMask & 0x1F;
	rpt[7] = 0x01;

	if (HidD_SetOutputReport(m_writeHandle, rpt, m_outputReportLength))
	{
		Log("Legacy OK (0xF8) mask=0x%02X", ledMask);
		free(rpt);
		return true;
	}
	Log("Legacy FAIL (0xF8) err=%lu", GetLastError());

	// Method 2: report ID = 0x00
	memset(rpt, 0, m_outputReportLength);
	rpt[0] = 0x00;
	rpt[1] = CMD_LED;
	rpt[2] = SUBCMD_LED;
	rpt[3] = ledMask & 0x1F;
	rpt[8] = 0x01;

	if (HidD_SetOutputReport(m_writeHandle, rpt, m_outputReportLength))
	{
		Log("Legacy OK (0x00) mask=0x%02X", ledMask);
		free(rpt);
		return true;
	}
	DWORD err = GetLastError();
	Log("Legacy FAIL (0x00) err=%lu", err);

	if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE)
		Close();

	free(rpt);
	return false;
}

// -------------------------------------------------------
// HID++ protocol (G923 Xbox)
// -------------------------------------------------------

bool LogitechLED::HIDPPSend(BYTE featureIdx, BYTE funcSwId,
                             const BYTE* params, int paramLen)
{
	BYTE rpt[20] = {0};
	rpt[0] = HIDPP_LONG;    // 0x11
	rpt[1] = HIDPP_DEV;     // 0x01
	rpt[2] = featureIdx;
	rpt[3] = funcSwId;

	if (params && paramLen > 0)
	{
		int n = (paramLen > 16) ? 16 : paramLen;
		memcpy(&rpt[4], params, n);
	}

	// Use HidD_SetOutputReport on synchronous handle
	BOOL ok = HidD_SetOutputReport(m_writeHandle, rpt, m_outputReportLength);

	Log("HID++ TX [%02X %02X %02X %02X %02X %02X %02X %02X] %s%s",
		rpt[0], rpt[1], rpt[2], rpt[3], rpt[4], rpt[5], rpt[6], rpt[7],
		ok ? "OK" : "FAIL",
		ok ? "" : "");

	if (!ok)
	{
		DWORD err = GetLastError();
		Log("HID++ TX err=%lu", err);
	}

	return ok == TRUE;
}

bool LogitechLED::HIDPPRecv(BYTE* response, DWORD timeoutMs)
{
	if (m_inputReportLength == 0 || m_readHandle == INVALID_HANDLE_VALUE)
		return false;

	BYTE* buf = (BYTE*)calloc(m_inputReportLength, 1);
	if (!buf) return false;

	OVERLAPPED ov = {0};
	ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	DWORD bytesRead = 0;

	BOOL ok = ReadFile(m_readHandle, buf, m_inputReportLength, &bytesRead, &ov);
	if (!ok)
	{
		if (GetLastError() == ERROR_IO_PENDING)
		{
			DWORD wait = WaitForSingleObject(ov.hEvent, timeoutMs);
			if (wait == WAIT_OBJECT_0)
			{
				GetOverlappedResult(m_readHandle, &ov, &bytesRead, FALSE);
			}
			else
			{
				CancelIo(m_readHandle);
				GetOverlappedResult(m_readHandle, &ov, &bytesRead, TRUE);
				bytesRead = 0;
			}
		}
		else
		{
			Log("HID++ RX ReadFile err=%lu", GetLastError());
		}
	}

	CloseHandle(ov.hEvent);

	if (bytesRead > 0)
	{
		memcpy(response, buf, (bytesRead < 20) ? bytesRead : 20);
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

BYTE LogitechLED::HIDPPGetFeatureIndex(USHORT featureId)
{
	BYTE params[16] = {0};
	params[0] = (BYTE)(featureId >> 8);
	params[1] = (BYTE)(featureId & 0xFF);

	// ROOT (index 0x00), function 0 (getFeatureIndex), SW ID = 0x01
	if (!HIDPPSend(0x00, 0x01, params, 2))
		return 0;

	BYTE resp[20] = {0};
	if (!HIDPPRecv(resp, 2000))
		return 0;

	// Error: byte[2] == 0xFF
	if (resp[2] == 0xFF)
	{
		Log("Feature 0x%04X -> error (code=0x%02X)", featureId, resp[5]);
		return 0;
	}

	BYTE idx = resp[4];
	Log("Feature 0x%04X -> index=%u (type=0x%02X)", featureId, idx, resp[5]);
	return idx;
}

void LogitechLED::HIDPPLogAllFeatures()
{
	BYTE fsIdx = HIDPPGetFeatureIndex(FEAT_FEATURE_SET);
	if (fsIdx == 0)
	{
		Log("Cannot enumerate features (IFeatureSet not found)");
		return;
	}

	// getCount: featureSetIndex, function 0, SW ID 1
	if (!HIDPPSend(fsIdx, 0x01, NULL, 0))
		return;

	BYTE resp[20] = {0};
	if (!HIDPPRecv(resp, 2000))
		return;

	int count = resp[4];
	Log("Device has %d features:", count);

	for (int i = 1; i <= count && i <= 64; i++)
	{
		BYTE p[16] = {0};
		p[0] = (BYTE)i;

		// getFeatureID: function 1, SW ID 1
		if (!HIDPPSend(fsIdx, 0x11, p, 1))
			continue;

		BYTE r[20] = {0};
		if (!HIDPPRecv(r, 2000))
			continue;

		USHORT fid = ((USHORT)r[4] << 8) | r[5];
		Log("  [%2d] Feature 0x%04X (type=0x%02X)", i, fid, r[6]);
	}
}

bool LogitechLED::InitHIDPP()
{
	Log("InitHIDPP: starting");

	// Ping: ROOT (index 0), function 1 (ping), SW ID 1
	BYTE pingParams[16] = {0, 0, 0xAA};
	if (HIDPPSend(0x00, 0x11, pingParams, 3))
	{
		BYTE resp[20] = {0};
		if (HIDPPRecv(resp, 2000))
		{
			if (resp[2] != 0xFF)
				Log("Ping OK, HID++ version %u.%u", resp[4], resp[5]);
			else
				Log("Ping error response (code=0x%02X)", resp[5]);
		}
		else
		{
			Log("Ping timeout - device may not support HID++");
		}
	}

	// Try known LED features
	struct { USHORT id; const char* name; } features[] = {
		{ FEAT_LED_CONTROL, "LED_CONTROL(0x1300)" },
		{ FEAT_LED_EFFECTS, "LED_EFFECTS(0x8070)" },
		{ FEAT_RGB_STATE,   "RGB_STATE(0x8071)"   },
	};

	for (int i = 0; i < 3; i++)
	{
		Log("Trying %s...", features[i].name);
		BYTE idx = HIDPPGetFeatureIndex(features[i].id);
		if (idx > 0)
		{
			m_ledFeatureIndex = idx;
			Log("Found %s at index %u", features[i].name, idx);
			return true;
		}
	}

	// Nothing found - dump all features for debugging
	Log("No known LED feature found, enumerating all features...");
	HIDPPLogAllFeatures();

	return false;
}

bool LogitechLED::SetLEDsHIDPP(BYTE ledMask)
{
	if (m_ledFeatureIndex == 0)
		return SetLEDsLegacy(ledMask);

	// Send to LED feature, function 3 (setState), SW ID 1
	// Param: LED bitmask
	BYTE params[16] = {0};
	params[0] = ledMask & 0x1F;

	bool ok = HIDPPSend(m_ledFeatureIndex, 0x31, params, 1);
	if (!ok)
	{
		// Try function 1 as alternative
		ok = HIDPPSend(m_ledFeatureIndex, 0x11, params, 1);
	}

	Log("SetLEDsHIDPP mask=0x%02X -> %s", ledMask, ok ? "OK" : "FAIL");
	return ok;
}
