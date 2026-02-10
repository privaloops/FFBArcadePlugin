#include "LogitechLED.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// Logitech Vendor ID
static const USHORT LOGITECH_VID = 0x046D;

// G923 Product IDs
static const USHORT G923_PID_XBOX = 0xC26E;  // G923 Xbox/PC variant
static const USHORT G923_PID_PS   = 0xC267;  // G923 PlayStation/PC variant
// G29 for broader compatibility
static const USHORT G29_PID       = 0xC24F;

// Logitech extended command for RPM LEDs
static const BYTE LOGITECH_CMD_SET_LED = 0xF8;
static const BYTE LOGITECH_LED_SUBCMD  = 0x12;

// File-based logging for LED diagnostics
static FILE* g_ledLogFile = NULL;

static void LEDLogInit()
{
	if (g_ledLogFile) return;
	g_ledLogFile = fopen("FFBPlugin_LED.log", "w");
}

static void LEDLog(const char* msg)
{
	LEDLogInit();
	if (g_ledLogFile)
	{
		fprintf(g_ledLogFile, "%s\n", msg);
		fflush(g_ledLogFile);
	}
	OutputDebugStringA(msg);
	OutputDebugStringA("\n");
}

LogitechLED::LogitechLED()
	: m_deviceHandle(INVALID_HANDLE_VALUE)
	, m_available(false)
	, m_outputReportLength(0)
{
}

LogitechLED::~LogitechLED()
{
	Close();
}

bool LogitechLED::Init()
{
	LEDLog("LogitechLED::Init() called");
	if (m_available)
		return true;

	m_available = FindAndOpenDevice();
	if (m_available)
		LEDLog("LogitechLED::Init() SUCCESS - device opened");
	else
		LEDLog("LogitechLED::Init() FAILED - no compatible device found");
	return m_available;
}

void LogitechLED::Close()
{
	if (m_deviceHandle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_deviceHandle);
		m_deviceHandle = INVALID_HANDLE_VALUE;
	}
	m_available = false;
	m_outputReportLength = 0;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || m_deviceHandle == INVALID_HANDLE_VALUE || m_outputReportLength == 0)
		return false;

	// Allocate buffer matching the device's expected output report length
	BYTE* report = (BYTE*)calloc(m_outputReportLength, 1);
	if (!report)
		return false;

	// For Logitech wheels, the extended command IS the report.
	// The Report ID byte is implicit in the HID descriptor.
	// Use HidD_SetOutputReport which handles report ID routing properly.
	report[0] = LOGITECH_CMD_SET_LED;        // 0xF8 - this IS the report ID for extended commands
	report[1] = LOGITECH_LED_SUBCMD;         // 0x12
	report[2] = ledMask & 0x1F;              // 5 LEDs, bits 0-4
	report[3] = 0x00;
	report[4] = 0x00;
	report[5] = 0x00;
	report[6] = 0x00;
	report[7] = 0x01;

	char buf[256];
	sprintf_s(buf, "LogitechLED: SetLEDs mask=0x%02X reportLen=%u report[0]=0x%02X",
		ledMask, m_outputReportLength, report[0]);
	LEDLog(buf);

	// Try HidD_SetOutputReport first (more reliable for HID devices with specific report IDs)
	BOOL result = HidD_SetOutputReport(m_deviceHandle, report, m_outputReportLength);

	if (!result)
	{
		DWORD err = GetLastError();
		sprintf_s(buf, "LogitechLED: HidD_SetOutputReport FAILED (reportID=0xF8), error=%lu", err);
		LEDLog(buf);

		// Fallback: try with report ID 0x00 (some devices expect this)
		memset(report, 0, m_outputReportLength);
		report[0] = 0x00;                        // Report ID 0
		report[1] = LOGITECH_CMD_SET_LED;        // 0xF8
		report[2] = LOGITECH_LED_SUBCMD;         // 0x12
		report[3] = ledMask & 0x1F;
		report[4] = 0x00;
		report[5] = 0x00;
		report[6] = 0x00;
		report[7] = 0x01;

		result = HidD_SetOutputReport(m_deviceHandle, report, m_outputReportLength);
		if (!result)
		{
			err = GetLastError();
			sprintf_s(buf, "LogitechLED: HidD_SetOutputReport FAILED (reportID=0x00), error=%lu", err);
			LEDLog(buf);

			// Last resort: try WriteFile with report ID = 0xF8
			memset(report, 0, m_outputReportLength);
			report[0] = LOGITECH_CMD_SET_LED;
			report[1] = LOGITECH_LED_SUBCMD;
			report[2] = ledMask & 0x1F;
			report[3] = 0x00;
			report[4] = 0x00;
			report[5] = 0x00;
			report[6] = 0x00;
			report[7] = 0x01;

			DWORD bytesWritten = 0;
			result = WriteFile(m_deviceHandle, report, m_outputReportLength, &bytesWritten, NULL);
			if (!result)
			{
				err = GetLastError();
				sprintf_s(buf, "LogitechLED: WriteFile FAILED (reportID=0xF8), error=%lu", err);
				LEDLog(buf);

				if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE)
				{
					Close();
				}
			}
			else
			{
				sprintf_s(buf, "LogitechLED: WriteFile OK (reportID=0xF8), bytes=%lu", bytesWritten);
				LEDLog(buf);
			}
		}
		else
		{
			LEDLog("LogitechLED: HidD_SetOutputReport OK (reportID=0x00)");
		}
	}
	else
	{
		LEDLog("LogitechLED: HidD_SetOutputReport OK (reportID=0xF8)");
	}

	free(report);
	return result == TRUE;
}

bool LogitechLED::SetLEDsFromPercent(double percent)
{
	if (percent < 0.0) percent = 0.0;
	if (percent > 1.0) percent = 1.0;

	BYTE mask = 0;

	// Progressive LED lighting: green -> yellow -> red
	if (percent >= 0.2) mask |= 0x01;  // LED 1 (green)
	if (percent >= 0.4) mask |= 0x02;  // LED 2 (green)
	if (percent >= 0.6) mask |= 0x04;  // LED 3 (yellow)
	if (percent >= 0.8) mask |= 0x08;  // LED 4 (yellow)
	if (percent >= 0.95) mask |= 0x10; // LED 5 (red)

	return SetLEDs(mask);
}

bool LogitechLED::ClearLEDs()
{
	return SetLEDs(0x00);
}

bool LogitechLED::FindAndOpenDevice()
{
	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);

	HDEVINFO deviceInfoSet = SetupDiGetClassDevsA(
		&hidGuid, NULL, NULL,
		DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

	if (deviceInfoSet == INVALID_HANDLE_VALUE)
	{
		LEDLog("LogitechLED: SetupDiGetClassDevs failed");
		return false;
	}

	SP_DEVICE_INTERFACE_DATA interfaceData;
	interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	int deviceCount = 0;
	int logitechCount = 0;
	HANDLE bestHandle = INVALID_HANDLE_VALUE;
	USHORT bestReportLen = 0;
	char bestPath[512] = { 0 };

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, i, &interfaceData); i++)
	{
		deviceCount++;

		DWORD requiredSize = 0;
		SetupDiGetDeviceInterfaceDetailA(deviceInfoSet, &interfaceData, NULL, 0, &requiredSize, NULL);

		PSP_DEVICE_INTERFACE_DETAIL_DATA_A detailData =
			(PSP_DEVICE_INTERFACE_DETAIL_DATA_A)malloc(requiredSize);
		if (!detailData)
			continue;

		detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		if (!SetupDiGetDeviceInterfaceDetailA(deviceInfoSet, &interfaceData, detailData, requiredSize, NULL, NULL))
		{
			free(detailData);
			continue;
		}

		HANDLE handle = CreateFileA(
			detailData->DevicePath,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);

		if (handle == INVALID_HANDLE_VALUE)
		{
			free(detailData);
			continue;
		}

		HIDD_ATTRIBUTES attrs;
		attrs.Size = sizeof(HIDD_ATTRIBUTES);

		if (HidD_GetAttributes(handle, &attrs))
		{
			if (attrs.VendorID == LOGITECH_VID &&
				(attrs.ProductID == G923_PID_XBOX ||
				 attrs.ProductID == G923_PID_PS ||
				 attrs.ProductID == G29_PID))
			{
				logitechCount++;

				PHIDP_PREPARSED_DATA preparsedData = NULL;
				if (HidD_GetPreparsedData(handle, &preparsedData))
				{
					HIDP_CAPS caps;
					if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS)
					{
						char buf[512];
						sprintf_s(buf,
							"LogitechLED: Found Logitech VID=0x%04X PID=0x%04X "
							"UsagePage=0x%04X Usage=0x%04X "
							"InLen=%u OutLen=%u FeatLen=%u "
							"NumOutputValueCaps=%u "
							"Path=%s",
							attrs.VendorID, attrs.ProductID,
							caps.UsagePage, caps.Usage,
							caps.InputReportByteLength,
							caps.OutputReportByteLength,
							caps.FeatureReportByteLength,
							caps.NumberOutputValueCaps,
							detailData->DevicePath);
						LEDLog(buf);

						// Log ALL collections, pick the best one for output
						if (caps.OutputReportByteLength >= 8)
						{
							// Prefer larger OutputReportByteLength (more likely the right interface)
							if (bestHandle == INVALID_HANDLE_VALUE || caps.OutputReportByteLength > bestReportLen)
							{
								if (bestHandle != INVALID_HANDLE_VALUE)
									CloseHandle(bestHandle);
								bestHandle = handle;
								bestReportLen = caps.OutputReportByteLength;
								strcpy_s(bestPath, detailData->DevicePath);
								handle = INVALID_HANDLE_VALUE; // don't close below
							}
						}
					}
					HidD_FreePreparsedData(preparsedData);
				}
			}
		}

		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		free(detailData);
	}

	SetupDiDestroyDeviceInfoList(deviceInfoSet);

	if (bestHandle != INVALID_HANDLE_VALUE)
	{
		m_deviceHandle = bestHandle;
		m_outputReportLength = bestReportLen;
		char buf[512];
		sprintf_s(buf, "LogitechLED: Selected device OutputReportLen=%u Path=%s",
			m_outputReportLength, bestPath);
		LEDLog(buf);
		return true;
	}

	char buf[128];
	sprintf_s(buf, "LogitechLED: Enumerated %d HID devices, found %d Logitech matches, none suitable",
		deviceCount, logitechCount);
	LEDLog(buf);
	return false;
}
