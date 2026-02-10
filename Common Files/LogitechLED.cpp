#include "LogitechLED.h"

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
	: m_sdkModule(NULL)
	, m_available(false)
	, m_pfnInit(NULL)
	, m_pfnUpdate(NULL)
	, m_pfnIsConnected(NULL)
	, m_pfnPlayLeds(NULL)
	, m_pfnShutdown(NULL)
{
}

LogitechLED::~LogitechLED()
{
	Close();
}

void LogitechLED::Close()
{
	if (m_pfnShutdown && m_available)
		m_pfnShutdown();

	if (m_sdkModule)
	{
		FreeLibrary(m_sdkModule);
		m_sdkModule = NULL;
	}

	m_available = false;
	m_pfnInit = NULL;
	m_pfnUpdate = NULL;
	m_pfnIsConnected = NULL;
	m_pfnPlayLeds = NULL;
	m_pfnShutdown = NULL;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

bool LogitechLED::Init()
{
	Log("LogitechLED::Init() - Logitech SDK mode");

	if (m_available)
		return true;

	// Load the SDK DLL
	m_sdkModule = LoadLibraryA("LogitechSteeringWheel.dll");
	if (!m_sdkModule)
	{
		Log("LogitechSteeringWheel.dll not found (err=%lu)", GetLastError());
		Log("Ensure Logitech G Hub is installed and LogitechSteeringWheel.dll is alongside the game.");
		return false;
	}

	Log("LogitechSteeringWheel.dll loaded");

	// Resolve function pointers
	m_pfnInit = (PFN_LogiSteeringInitialize)GetProcAddress(m_sdkModule, "LogiSteeringInitialize");
	m_pfnUpdate = (PFN_LogiUpdate)GetProcAddress(m_sdkModule, "LogiUpdate");
	m_pfnIsConnected = (PFN_LogiIsConnected)GetProcAddress(m_sdkModule, "LogiIsConnected");
	m_pfnPlayLeds = (PFN_LogiPlayLeds)GetProcAddress(m_sdkModule, "LogiPlayLeds");
	m_pfnShutdown = (PFN_LogiSteeringShutdown)GetProcAddress(m_sdkModule, "LogiSteeringShutdown");

	if (!m_pfnInit || !m_pfnUpdate || !m_pfnPlayLeds || !m_pfnShutdown)
	{
		Log("Failed to resolve SDK functions (Init=%p Update=%p PlayLeds=%p Shutdown=%p)",
			m_pfnInit, m_pfnUpdate, m_pfnPlayLeds, m_pfnShutdown);
		FreeLibrary(m_sdkModule);
		m_sdkModule = NULL;
		return false;
	}

	Log("SDK functions resolved");

	// Initialize the steering wheel SDK
	if (!m_pfnInit(false))
	{
		Log("LogiSteeringInitialize() failed - is G Hub running?");
		FreeLibrary(m_sdkModule);
		m_sdkModule = NULL;
		return false;
	}

	Log("SDK initialized");

	// Give the SDK a moment to enumerate devices
	m_pfnUpdate();

	if (m_pfnIsConnected && m_pfnIsConnected(0))
		Log("Steering wheel detected at index 0");
	else
		Log("No wheel detected yet (will retry on first LED update)");

	m_available = true;
	Log("LogitechLED ready");
	return true;
}

bool LogitechLED::SetLEDsFromPercent(double percent)
{
	if (!m_available || !m_pfnPlayLeds || !m_pfnUpdate)
		return false;

	if (percent < 0.0) percent = 0.0;
	if (percent > 1.0) percent = 1.0;

	m_pfnUpdate();

	// Map FFB strength (0.0-1.0) to RPM values.
	// rpmFirstLed=200: first LED lights at 20% strength
	// rpmRedLine=1000: all LEDs at 100% strength
	float currentRPM = (float)(percent * 1000.0);
	return m_pfnPlayLeds(0, currentRPM, 200.0f, 1000.0f);
}

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available)
		return false;

	// Map 5-bit mask to approximate percentage
	int count = 0;
	for (int i = 0; i < 5; i++)
		if (ledMask & (1 << i)) count++;

	return SetLEDsFromPercent(count / 5.0);
}

bool LogitechLED::ClearLEDs()
{
	if (!m_available || !m_pfnPlayLeds || !m_pfnUpdate)
		return false;

	m_pfnUpdate();
	return m_pfnPlayLeds(0, 0.0f, 200.0f, 1000.0f);
}
