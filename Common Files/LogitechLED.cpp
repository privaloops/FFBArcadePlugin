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

// Forward declaration for logging (defined in DllMain.cpp)
extern void LogMessage(const char* fmt, ...);

// Simple log helper - uses OutputDebugString if LogMessage is not available
static void LEDLog(const char* msg)
{
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
	if (m_available)
		return true;

	m_available = FindAndOpenDevice();
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

	// First byte = Report ID (0x00 for default)
	// Then the Logitech extended command
	report[0] = 0x00;                        // Report ID
	report[1] = LOGITECH_CMD_SET_LED;        // 0xF8
	report[2] = LOGITECH_LED_SUBCMD;         // 0x12
	report[3] = ledMask & 0x1F;              // 5 LEDs, bits 0-4
	report[4] = 0x00;
	report[5] = 0x00;
	report[6] = 0x00;
	report[7] = 0x01;

	DWORD bytesWritten = 0;
	BOOL result = WriteFile(m_deviceHandle, report, m_outputReportLength, &bytesWritten, NULL);

	if (!result)
	{
		DWORD err = GetLastError();
		char buf[256];
		sprintf_s(buf, "LogitechLED: WriteFile failed, error=%lu, reportLen=%u, mask=0x%02X",
			err, m_outputReportLength, ledMask);
		LEDLog(buf);

		if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE)
		{
			Close();
		}
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

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, i, &interfaceData); i++)
	{
		deviceCount++;

		// Get required buffer size
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

		// Open the device
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
							"InputReportLen=%u OutputReportLen=%u FeatureReportLen=%u "
							"Path=%s",
							attrs.VendorID, attrs.ProductID,
							caps.UsagePage, caps.Usage,
							caps.InputReportByteLength,
							caps.OutputReportByteLength,
							caps.FeatureReportByteLength,
							detailData->DevicePath);
						LEDLog(buf);

						// We need an output report length that can hold our command
						if (caps.OutputReportByteLength >= 8)
						{
							m_outputReportLength = caps.OutputReportByteLength;
							HidD_FreePreparsedData(preparsedData);
							free(detailData);
							SetupDiDestroyDeviceInfoList(deviceInfoSet);
							m_deviceHandle = handle;

							sprintf_s(buf, "LogitechLED: Using device with OutputReportLen=%u", m_outputReportLength);
							LEDLog(buf);
							return true;
						}
					}
					HidD_FreePreparsedData(preparsedData);
				}
			}
		}

		CloseHandle(handle);
		free(detailData);
	}

	char buf[128];
	sprintf_s(buf, "LogitechLED: Enumerated %d HID devices, found %d Logitech matches, none suitable",
		deviceCount, logitechCount);
	LEDLog(buf);

	SetupDiDestroyDeviceInfoList(deviceInfoSet);
	return false;
}
