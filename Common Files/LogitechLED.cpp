#include "LogitechLED.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

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

LogitechLED::LogitechLED()
	: m_deviceHandle(INVALID_HANDLE_VALUE)
	, m_available(false)
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
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available || m_deviceHandle == INVALID_HANDLE_VALUE)
		return false;

	// Logitech extended HID report for RPM LEDs
	// Format: [ReportID, CMD, SUBCMD, LED_MASK, 0, 0, 0, 1]
	BYTE report[8] = { 0 };
	report[0] = LOGITECH_CMD_SET_LED;  // 0xF8
	report[1] = LOGITECH_LED_SUBCMD;   // 0x12
	report[2] = ledMask & 0x1F;        // 5 LEDs, bits 0-4
	report[3] = 0x00;
	report[4] = 0x00;
	report[5] = 0x00;
	report[6] = 0x00;
	report[7] = 0x01;

	DWORD bytesWritten = 0;
	BOOL result = WriteFile(m_deviceHandle, report, sizeof(report), &bytesWritten, NULL);

	if (!result)
	{
		// Device may have been disconnected
		DWORD err = GetLastError();
		if (err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE)
		{
			Close();
		}
	}

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
		return false;

	SP_DEVICE_INTERFACE_DATA interfaceData;
	interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, i, &interfaceData); i++)
	{
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

		// Open the device to check VID/PID
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
				// Check this is the right HID interface for output reports
				// by verifying the output report length
				PHIDP_PREPARSED_DATA preparsedData = NULL;
				if (HidD_GetPreparsedData(handle, &preparsedData))
				{
					HIDP_CAPS caps;
					if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS)
					{
						// We need an output report length that can hold our 8-byte command
						if (caps.OutputReportByteLength >= 8)
						{
							HidD_FreePreparsedData(preparsedData);
							free(detailData);
							SetupDiDestroyDeviceInfoList(deviceInfoSet);
							m_deviceHandle = handle;
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

	SetupDiDestroyDeviceInfoList(deviceInfoSet);
	return false;
}
