#include "LogitechLED.h"
#include "LogitechSDK/LogitechSteeringWheelLib.h"

#include <stdio.h>
#include <stdarg.h>

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

// --- LogitechLED ---

LogitechLED::LogitechLED()
	: m_available(false)
{
}

LogitechLED::~LogitechLED()
{
	Close();
}

void LogitechLED::Close()
{
	if (m_available)
	{
		LogiSteeringShutdown();
		m_available = false;
		Log("LogitechLED shutdown");
	}
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

bool LogitechLED::Init()
{
	Log("LogitechLED::Init() - Logitech SDK (static link)");

	if (m_available)
		return true;

	// Try init with foreground window first, then without
	HWND hwnd = GetForegroundWindow();
	bool initOk = false;

	if (hwnd)
	{
		Log("Trying LogiSteeringInitializeWithWindow (hwnd=%p)", hwnd);
		initOk = LogiSteeringInitializeWithWindow(false, hwnd);
	}

	if (!initOk)
	{
		Log("Trying LogiSteeringInitialize (no window)");
		initOk = LogiSteeringInitialize(false);
	}

	if (!initOk)
	{
		Log("SDK init failed - is G Hub running? Is LogitechSteeringWheelEnginesWrapper.dll present?");
		return false;
	}

	Log("SDK initialized");

	// Give SDK time to enumerate
	LogiUpdate();

	if (LogiIsConnected(0))
	{
		wchar_t name[256] = {};
		LogiGetFriendlyProductName(0, name, 256);
		Log("Wheel connected at index 0: %ls", name);
	}
	else
	{
		Log("No wheel detected yet (will retry on first LED update)");
	}

	m_available = true;
	Log("LogitechLED ready");
	return true;
}

bool LogitechLED::SetLEDsFromPercent(double percent)
{
	if (!m_available)
		return false;

	if (percent < 0.0) percent = 0.0;
	if (percent > 1.0) percent = 1.0;

	LogiUpdate();

	// Map FFB strength (0.0-1.0) to RPM values
	// rpmFirstLed=200: first LED at ~20% strength
	// rpmRedLine=1000: all LEDs at 100% strength
	float currentRPM = (float)(percent * 1000.0);
	return LogiPlayLeds(0, currentRPM, 200.0f, 1000.0f);
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

	LogiUpdate();
	return LogiPlayLeds(0, 0.0f, 200.0f, 1000.0f);
}
