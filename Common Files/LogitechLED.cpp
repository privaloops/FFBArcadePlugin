#include "LogitechLED.h"

#include <stdio.h>
#include <stdarg.h>

// --- Logitech LED Escape protocol (from SDK "Independent" sample) ---

static const DWORD ESCAPE_COMMAND_LEDS = 0;
static const DWORD LEDS_VERSION_NUMBER = 0x00000001;

struct LedsRpmData
{
	FLOAT currentRPM;
	FLOAT rpmFirstLedTurnsOn;
	FLOAT rpmRedLine;
};

struct WheelData
{
	DWORD size;
	DWORD versionNbr;
	LedsRpmData rpmData;
};

// --- Known Logitech wheel VID/PIDs ---

static const DWORD LOGITECH_VID = 0x046D;
static const DWORD KNOWN_PIDS[] = {
	0xC24F,  // G29
	0xC260,  // G920 (no RPM LEDs but detected for completeness)
	0xC262,  // G920 Xbox
	0xC266,  // G923 PS
	0xC267,  // G923 PS (alt)
	0xC26D,  // G923 Xbox (alt PID)
	0xC26E,  // G923 Xbox
};
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

static bool IsKnownPID(DWORD pid)
{
	for (int i = 0; i < NUM_PIDS; i++)
		if (KNOWN_PIDS[i] == pid) return true;
	return false;
}

// --- LogitechLED ---

LogitechLED::LogitechLED()
	: m_dinputDll(NULL)
	, m_pDI(NULL)
	, m_pDevice(NULL)
	, m_available(false)
{
}

LogitechLED::~LogitechLED()
{
	Close();
}

void LogitechLED::Close()
{
	if (m_pDevice)
	{
		m_pDevice->Unacquire();
		m_pDevice->Release();
		m_pDevice = NULL;
	}

	if (m_pDI)
	{
		m_pDI->Release();
		m_pDI = NULL;
	}

	if (m_dinputDll)
	{
		FreeLibrary(m_dinputDll);
		m_dinputDll = NULL;
	}

	m_available = false;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

// --- Device enumeration callback ---

BOOL CALLBACK LogitechLED::EnumDevicesCallback(LPCDIDEVICEINSTANCEA lpddi, LPVOID pvRef)
{
	EnumContext* ctx = (EnumContext*)pvRef;

	DWORD vid = LOWORD(lpddi->guidProduct.Data1);
	DWORD pid = HIWORD(lpddi->guidProduct.Data1);

	Log("  Enum: VID=0x%04X PID=0x%04X Name='%s'", vid, pid, lpddi->tszProductName);

	if (vid == LOGITECH_VID && IsKnownPID(pid))
	{
		Log("  -> Logitech wheel found!");
		ctx->deviceGuid = lpddi->guidInstance;
		ctx->found = true;
		return DIENUM_STOP;
	}

	return DIENUM_CONTINUE;
}

// --- Init ---

bool LogitechLED::Init()
{
	Log("LogitechLED::Init() - DirectInput Escape mode");

	if (m_available)
		return true;

	// Load the REAL dinput8.dll from System32 (not our wrapper)
	char sysDir[MAX_PATH];
	GetSystemDirectoryA(sysDir, MAX_PATH);
	strcat_s(sysDir, "\\dinput8.dll");

	m_dinputDll = LoadLibraryA(sysDir);
	if (!m_dinputDll)
	{
		Log("Failed to load real dinput8.dll from %s (err=%lu)", sysDir, GetLastError());
		return false;
	}

	Log("Real dinput8.dll loaded from %s", sysDir);

	// Get the real DirectInput8Create
	typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
	PFN_DirectInput8Create pfnCreate = (PFN_DirectInput8Create)GetProcAddress(m_dinputDll, "DirectInput8Create");
	if (!pfnCreate)
	{
		Log("DirectInput8Create not found in real dinput8.dll");
		Close();
		return false;
	}

	// Create DirectInput interface
	HRESULT hr = pfnCreate(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8A, (LPVOID*)&m_pDI, NULL);
	if (FAILED(hr))
	{
		Log("DirectInput8Create failed (hr=0x%08X)", hr);
		Close();
		return false;
	}

	Log("DirectInput8 interface created");

	// Enumerate game controllers to find Logitech wheel
	EnumContext ctx = {};
	ctx.found = false;

	Log("Enumerating game controllers...");
	m_pDI->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevicesCallback, &ctx, DIEDFL_ATTACHEDONLY);

	if (!ctx.found)
	{
		Log("No Logitech wheel found");
		Close();
		return false;
	}

	// Create device
	hr = m_pDI->CreateDevice(ctx.deviceGuid, &m_pDevice, NULL);
	if (FAILED(hr))
	{
		Log("CreateDevice failed (hr=0x%08X)", hr);
		Close();
		return false;
	}

	// Set data format
	hr = m_pDevice->SetDataFormat(&c_dfDIJoystick2);
	if (FAILED(hr))
	{
		Log("SetDataFormat failed (hr=0x%08X)", hr);
		Close();
		return false;
	}

	// Set cooperative level: background + non-exclusive (don't steal from game)
	HWND hwnd = GetForegroundWindow();
	if (!hwnd) hwnd = GetDesktopWindow();

	hr = m_pDevice->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
	if (FAILED(hr))
	{
		Log("SetCooperativeLevel failed (hr=0x%08X)", hr);
		Close();
		return false;
	}

	// Acquire the device
	hr = m_pDevice->Acquire();
	if (FAILED(hr))
	{
		Log("Acquire failed (hr=0x%08X)", hr);
		Close();
		return false;
	}

	Log("Device acquired, testing Escape LED command...");

	// Test: try to clear LEDs to verify Escape() works
	if (!PlayLedsEscape(0.0f, 200.0f, 1000.0f))
	{
		Log("Escape LED command not supported on this device");
		Close();
		return false;
	}

	m_available = true;
	Log("LogitechLED ready (DirectInput Escape mode)");
	return true;
}

// --- LED control via Escape ---

bool LogitechLED::PlayLedsEscape(float currentRPM, float rpmFirstLed, float rpmRedLine)
{
	if (!m_pDevice)
		return false;

	WheelData wheelData;
	ZeroMemory(&wheelData, sizeof(wheelData));
	wheelData.size = sizeof(WheelData);
	wheelData.versionNbr = LEDS_VERSION_NUMBER;
	wheelData.rpmData.currentRPM = currentRPM;
	wheelData.rpmData.rpmFirstLedTurnsOn = rpmFirstLed;
	wheelData.rpmData.rpmRedLine = rpmRedLine;

	DIEFFESCAPE escape;
	ZeroMemory(&escape, sizeof(escape));
	escape.dwSize = sizeof(DIEFFESCAPE);
	escape.dwCommand = ESCAPE_COMMAND_LEDS;
	escape.lpvInBuffer = &wheelData;
	escape.cbInBuffer = sizeof(wheelData);

	HRESULT hr = m_pDevice->Escape(&escape);
	if (FAILED(hr))
	{
		Log("Escape failed (hr=0x%08X)", hr);
		return false;
	}

	return true;
}

bool LogitechLED::SetLEDsFromPercent(double percent)
{
	if (!m_available)
		return false;

	if (percent < 0.0) percent = 0.0;
	if (percent > 1.0) percent = 1.0;

	// Map FFB strength (0.0-1.0) to RPM values
	float currentRPM = (float)(percent * 1000.0);
	return PlayLedsEscape(currentRPM, 200.0f, 1000.0f);
}

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available)
		return false;

	int count = 0;
	for (int i = 0; i < 5; i++)
		if (ledMask & (1 << i)) count++;

	return SetLEDsFromPercent(count / 5.0);
}

bool LogitechLED::ClearLEDs()
{
	if (!m_available)
		return false;

	return PlayLedsEscape(0.0f, 200.0f, 1000.0f);
}
