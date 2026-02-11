#include "LogitechLED.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdarg.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dxguid.lib")

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

// --- Steering Wheel SDK function pointers ---

typedef bool (__cdecl *LogiSteeringInit_t)(bool);
typedef bool (__cdecl *LogiSteeringInitWithWindow_t)(bool, HWND);
typedef bool (__cdecl *LogiUpdate_t)();
typedef bool (__cdecl *LogiIsConnected_t)(int);
typedef bool (__cdecl *LogiPlayLeds_t)(int, float, float, float);
typedef bool (__cdecl *LogiPlayLedsDInput_t)(LPVOID, float, float, float);  // accepts any LPDIRECTINPUTDEVICE8 variant
typedef void (__cdecl *LogiSteeringShutdown_t)();

static LogiSteeringInit_t            g_SteeringInit = NULL;
static LogiSteeringInitWithWindow_t  g_SteeringInitWithWindow = NULL;
static LogiUpdate_t                  g_SteeringUpdate = NULL;
static LogiIsConnected_t             g_IsConnected = NULL;
static LogiPlayLeds_t                g_PlayLeds = NULL;
static LogiPlayLedsDInput_t          g_PlayLedsDInput = NULL;
static LogiSteeringShutdown_t        g_SteeringShutdown = NULL;
static bool                          g_SteeringNeedsLateInit = false;
static LPDIRECTINPUTDEVICE8A         g_realDIDevice = NULL;
static LPDIRECTINPUT8A               g_realDI = NULL;

// Bypass flag declared in DllMain.cpp
extern volatile bool g_bypassDIWrapper;

// --- G Hub LED SDK function pointers ---

typedef bool (*LogiLedInit_t)();
typedef bool (*LogiLedInitWithName_t)(const char*);
typedef bool (*LogiLedSetTargetDevice_t)(int);
typedef bool (*LogiLedSetLighting_t)(int, int, int);
typedef bool (*LogiLedSaveCurrentLighting_t)();
typedef bool (*LogiLedRestoreLighting_t)();
typedef void (*LogiLedShutdown_t)();
typedef bool (*LogiLedGetSdkVersion_t)(int*, int*, int*);
typedef bool (*LogiLedSetLightingForKeyWithKeyName_t)(int, int, int, int);
typedef bool (*LogiLedSetZone_t)(int, int, int, int, int);

static LogiLedInit_t                     g_LedInit = NULL;
static LogiLedInitWithName_t             g_LedInitWithName = NULL;
static LogiLedSetTargetDevice_t          g_LedSetTarget = NULL;
static LogiLedSetLighting_t              g_LedSetLighting = NULL;
static LogiLedSaveCurrentLighting_t      g_LedSave = NULL;
static LogiLedRestoreLighting_t          g_LedRestore = NULL;
static LogiLedShutdown_t                 g_LedShutdown = NULL;
static LogiLedGetSdkVersion_t            g_LedGetVersion = NULL;
static LogiLedSetZone_t                  g_LedSetZone = NULL;

// --- LogitechLED ---

LogitechLED::LogitechLED()
	: m_handle(INVALID_HANDLE_VALUE)
	, m_available(false)
	, m_reportLen(0)
	, m_method(METHOD_NONE)
	, m_ledFeatureIdx(0)
	, m_ledFunctionId(0)
	, m_deviceIdx(0xFF)
	, m_sdkDll(NULL)
	, m_steeringDll(NULL)
{
}

LogitechLED::~LogitechLED()
{
	Close();
}

void LogitechLED::Close()
{
	if (m_method == METHOD_STEERING_SDK)
	{
		if (g_SteeringShutdown) g_SteeringShutdown();
		if (g_realDIDevice) { g_realDIDevice->Release(); g_realDIDevice = NULL; }
		if (g_realDI) { g_realDI->Release(); g_realDI = NULL; }
	}

	if (m_method == METHOD_SDK && g_LedShutdown)
		g_LedShutdown();

	if (m_steeringDll)
	{
		FreeLibrary(m_steeringDll);
		m_steeringDll = NULL;
	}

	if (m_sdkDll)
	{
		FreeLibrary(m_sdkDll);
		m_sdkDll = NULL;
	}

	if (m_handle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_handle);
		m_handle = INVALID_HANDLE_VALUE;
	}

	m_available = false;
	m_method = METHOD_NONE;
	g_SteeringInit = NULL;
	g_SteeringInitWithWindow = NULL;
	g_SteeringUpdate = NULL;
	g_IsConnected = NULL;
	g_PlayLeds = NULL;
	g_PlayLedsDInput = NULL;
	g_SteeringShutdown = NULL;
	g_SteeringNeedsLateInit = false;
	g_LedInit = NULL;
	g_LedSetLighting = NULL;
	g_LedShutdown = NULL;
	g_LedSetZone = NULL;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

// --- Phase 0: Logitech Steering Wheel SDK ---

// Helper: dump all PE exports from a loaded DLL
static void DumpExports(HMODULE dll, const char* label)
{
	BYTE* base = (BYTE*)dll;
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return;

	DWORD expRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	if (!expRVA) { Log("  %s: no export table", label); return; }

	IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + expRVA);
	DWORD* names = (DWORD*)(base + exp->AddressOfNames);
	Log("  --- %s EXPORTS (%lu) ---", label, exp->NumberOfNames);
	for (DWORD i = 0; i < exp->NumberOfNames; i++)
		Log("    [%02lu] %s", i, (char*)(base + names[i]));
	Log("  --- END ---");
}

bool LogitechLED::TrySteeringSDK()
{
	Log("=== Phase 0: Steering Wheel SDK ===");

	// Search order: G Hub internal SDK first (newer, knows G923), then old SDK
	struct SDKCandidate {
		const char* path;
		const char* label;
	};

	SDKCandidate sdkCandidates[] = {
		// Priority 1: G Hub's own internal steering wheel SDK (2019+, knows G923)
#ifdef _WIN64
		{ "C:\\Program Files\\LGHUB\\sdk_legacy_steering_wheel_x64.dll", "G Hub internal x64" },
		{ "C:\\Program Files\\LGHUB\\sdks\\sdk_legacy_steering_wheel_x64.dll", "G Hub sdks x64" },
#else
		{ "C:\\Program Files\\LGHUB\\sdk_legacy_steering_wheel_x86.dll", "G Hub internal x86" },
		{ "C:\\Program Files\\LGHUB\\sdks\\sdk_legacy_steering_wheel_x86.dll", "G Hub sdks x86" },
#endif
		// Priority 2: Old SDK in game directory or SDK install path
		{ "LogitechSteeringWheelEnginesWrapper.dll", "game dir" },
		{ "C:\\Program Files\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x86\\LogitechSteeringWheelEnginesWrapper.dll", "SDK x86" },
		{ "C:\\Program Files (x86)\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x86\\LogitechSteeringWheelEnginesWrapper.dll", "SDK x86 (wow64)" },
#ifdef _WIN64
		{ "C:\\Program Files\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x64\\LogitechSteeringWheelEnginesWrapper.dll", "SDK x64" },
		{ "C:\\Program Files (x86)\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x64\\LogitechSteeringWheelEnginesWrapper.dll", "SDK x64 (wow64)" },
#endif
		{ NULL, NULL }
	};

	const char* loadedLabel = NULL;
	for (int i = 0; sdkCandidates[i].path; i++)
	{
		m_steeringDll = LoadLibraryA(sdkCandidates[i].path);
		if (m_steeringDll)
		{
			Log("  Loaded: %s [%s]", sdkCandidates[i].path, sdkCandidates[i].label);
			loadedLabel = sdkCandidates[i].label;
			break;
		}
	}

	if (!m_steeringDll)
	{
		Log("  No steering wheel DLL found. Searched:");
		for (int i = 0; sdkCandidates[i].path; i++)
			Log("    - %s", sdkCandidates[i].path);
		return false;
	}

	// Dump ALL exports so we can see what functions exist
	DumpExports(m_steeringDll, loadedLabel ? loadedLabel : "SteeringSDK");

	// Try many possible function name patterns (G Hub internal DLL may use different names)
	const char* initNames[] = {
		"LogiSteeringInitialize", "_LogiSteeringInitialize",
		"LogiInit", "LogiInitialize",
		NULL
	};
	const char* initWndNames[] = {
		"LogiSteeringInitializeWithWindow", "_LogiSteeringInitializeWithWindow",
		"LogiInitWithWindow",
		NULL
	};
	const char* updateNames[] = {
		"LogiUpdate", "_LogiUpdate",
		"LogiSteeringUpdate",
		NULL
	};
	const char* connNames[] = {
		"LogiIsConnected", "_LogiIsConnected",
		"LogiIsDeviceConnected",
		NULL
	};
	const char* ledsNames[] = {
		"LogiPlayLeds", "_LogiPlayLeds",
		"LogiSetLeds", "LogiSetLEDs", "LogiPlayLEDs",
		"LogiSetRpmLeds", "LogiPlayRpmLeds",
		NULL
	};
	const char* shutNames[] = {
		"LogiSteeringShutdown", "_LogiSteeringShutdown",
		"LogiShutdown",
		NULL
	};

	for (int i = 0; initNames[i] && !g_SteeringInit; i++)
		g_SteeringInit = (LogiSteeringInit_t)GetProcAddress(m_steeringDll, initNames[i]);
	for (int i = 0; initWndNames[i] && !g_SteeringInitWithWindow; i++)
		g_SteeringInitWithWindow = (LogiSteeringInitWithWindow_t)GetProcAddress(m_steeringDll, initWndNames[i]);
	for (int i = 0; updateNames[i] && !g_SteeringUpdate; i++)
		g_SteeringUpdate = (LogiUpdate_t)GetProcAddress(m_steeringDll, updateNames[i]);
	for (int i = 0; connNames[i] && !g_IsConnected; i++)
		g_IsConnected = (LogiIsConnected_t)GetProcAddress(m_steeringDll, connNames[i]);
	for (int i = 0; ledsNames[i] && !g_PlayLeds; i++)
		g_PlayLeds = (LogiPlayLeds_t)GetProcAddress(m_steeringDll, ledsNames[i]);
	g_PlayLedsDInput = (LogiPlayLedsDInput_t)GetProcAddress(m_steeringDll, "LogiPlayLedsDInput");
	for (int i = 0; shutNames[i] && !g_SteeringShutdown; i++)
		g_SteeringShutdown = (LogiSteeringShutdown_t)GetProcAddress(m_steeringDll, shutNames[i]);

	Log("  Function resolution:");
	Log("    Init: %s", g_SteeringInit ? "FOUND" : "not found");
	Log("    InitWithWindow: %s", g_SteeringInitWithWindow ? "FOUND" : "not found");
	Log("    Update: %s", g_SteeringUpdate ? "FOUND" : "not found");
	Log("    IsConnected: %s", g_IsConnected ? "FOUND" : "not found");
	Log("    PlayLeds: %s", g_PlayLeds ? "FOUND" : "not found");
	Log("    PlayLedsDInput: %s", g_PlayLedsDInput ? "FOUND" : "not found");
	Log("    Shutdown: %s", g_SteeringShutdown ? "FOUND" : "not found");

	if (!g_SteeringUpdate || !g_IsConnected || !g_PlayLeds)
	{
		Log("  Required functions missing - this DLL may use different names");
		Log("  Check export list above for LED-related functions");
		FreeLibrary(m_steeringDll);
		m_steeringDll = NULL;
		return false;
	}

	if (!g_SteeringInit && !g_SteeringInitWithWindow)
	{
		Log("  No init function found");
		FreeLibrary(m_steeringDll);
		m_steeringDll = NULL;
		return false;
	}

	// Bypass our dinput8 wrapper so the SDK can enumerate real DirectInput devices
	Log("  Enabling DirectInput bypass for SDK enumeration...");
	g_bypassDIWrapper = true;

	// Try immediate init with multiple approaches
	bool ok = false;

	// Attempt 1: InitWithWindow + GetDesktopWindow (always available)
	if (!ok && g_SteeringInitWithWindow)
	{
		HWND desktop = GetDesktopWindow();
		ok = g_SteeringInitWithWindow(false, desktop);
		Log("  InitWithWindow(false, desktop=0x%p) -> %s", desktop, ok ? "OK" : "FAIL");
	}

	// Attempt 2: InitWithWindow + GetForegroundWindow
	if (!ok && g_SteeringInitWithWindow)
	{
		HWND fg = GetForegroundWindow();
		if (fg)
		{
			ok = g_SteeringInitWithWindow(false, fg);
			Log("  InitWithWindow(false, foreground=0x%p) -> %s", fg, ok ? "OK" : "FAIL");
		}
	}

	// Attempt 3: Simple init (no window)
	if (!ok && g_SteeringInit)
	{
		ok = g_SteeringInit(false);
		Log("  LogiSteeringInitialize(false) -> %s", ok ? "OK" : "FAIL");
	}

	if (ok)
	{
		// Init succeeded - enumerate and test (keep bypass active)
		Log("  Init succeeded, enumerating...");

		for (int retry = 0; retry < 20; retry++)
		{
			Sleep(300);
			g_SteeringUpdate();
			if (g_IsConnected(0))
			{
				Log("  Wheel connected at index 0 (after %d updates)", retry + 1);
				break;
			}
		}

		g_bypassDIWrapper = false;  // SDK done enumerating

		bool connected = g_IsConnected(0);
		Log("  LogiIsConnected(0) -> %s", connected ? "YES" : "NO");

		if (connected)
		{
			// Standard path: SDK found the wheel by index
			g_SteeringUpdate();
			bool led = g_PlayLeds(0, 100.0f, 0.0f, 100.0f);
			Log("  LogiPlayLeds(0, 100, 0, 100) -> %s *** ALL LEDs ***", led ? "OK" : "FAIL");

			if (led)
			{
				Sleep(1000);
				g_SteeringUpdate();
				g_PlayLeds(0, 0.0f, 0.0f, 100.0f);
				m_method = METHOD_STEERING_SDK;
				m_available = true;
				Log("=== LED CONTROL ACTIVE (Steering Wheel SDK) ===");
				return true;
			}
			Log("  PlayLeds failed despite connection");
		}

		// Fallback: SDK doesn't know G923 PID, try LogiPlayLedsDInput
		// Create a REAL DirectInput device bypassing our wrapper
		if (g_PlayLedsDInput)
		{
			Log("  Trying LogiPlayLedsDInput fallback...");

			// Load the real system dinput8.dll
			char sysDir[MAX_PATH];
			GetSystemDirectoryA(sysDir, MAX_PATH);
			strcat_s(sysDir, MAX_PATH, "\\dinput8.dll");

			typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
			HMODULE realDIDll = LoadLibraryA(sysDir);
			if (!realDIDll)
			{
				Log("  Failed to load real dinput8.dll from %s", sysDir);
			}
			else
			{
				DI8Create_t realCreate = (DI8Create_t)GetProcAddress(realDIDll, "DirectInput8Create");
				if (!realCreate)
				{
					Log("  DirectInput8Create not found in real dll");
					FreeLibrary(realDIDll);
				}
				else
				{
					HRESULT hr = realCreate(GetModuleHandle(NULL), DIRECTINPUT_VERSION,
						IID_IDirectInput8A, (LPVOID*)&g_realDI, NULL);
					Log("  Real DirectInput8Create -> 0x%08lX", hr);

					if (SUCCEEDED(hr) && g_realDI)
					{
						// Enumerate to find a Logitech wheel
						struct EnumCtx { GUID guid; bool found; };
						EnumCtx ctx = {{0}, false};

						g_realDI->EnumDevices(DI8DEVCLASS_GAMECTRL,
							[](const DIDEVICEINSTANCEA* inst, VOID* pCtx) -> BOOL {
								EnumCtx* c = (EnumCtx*)pCtx;
								USHORT vid = LOWORD(inst->guidProduct.Data1);
								USHORT pid = HIWORD(inst->guidProduct.Data1);
								Log("    DI device: VID=0x%04X PID=0x%04X '%s'",
									vid, pid, inst->tszInstanceName);
								if (vid == 0x046D && IsKnownPID(pid))
								{
									c->guid = inst->guidInstance;
									c->found = true;
									return DIENUM_STOP;
								}
								return DIENUM_CONTINUE;
							}, &ctx, DIEDFL_ATTACHEDONLY);

						if (!ctx.found)
						{
							Log("  No Logitech wheel found via real DirectInput");
							g_realDI->Release();
							g_realDI = NULL;
						}
						else
						{
							hr = g_realDI->CreateDevice(ctx.guid, &g_realDIDevice, NULL);
							Log("  CreateDevice -> 0x%08lX", hr);

							if (SUCCEEDED(hr) && g_realDIDevice)
							{
								// Test LogiPlayLedsDInput
								g_SteeringUpdate();
								bool led = g_PlayLedsDInput(g_realDIDevice, 100.0f, 0.0f, 100.0f);
								Log("  LogiPlayLedsDInput(dev, 100, 0, 100) -> %s *** ALL LEDs ***",
									led ? "OK" : "FAIL");

								if (led)
								{
									Sleep(1000);
									g_SteeringUpdate();
									g_PlayLedsDInput(g_realDIDevice, 0.0f, 0.0f, 100.0f);
									m_method = METHOD_STEERING_SDK;
									m_available = true;
									Log("=== LED CONTROL ACTIVE (Steering SDK + DInput) ===");
									return true;
								}

								// Cleanup on failure
								g_realDIDevice->Release();
								g_realDIDevice = NULL;
							}
							g_realDI->Release();
							g_realDI = NULL;
						}
					}
				}
			}
		}

		Log("  Steering SDK exhausted - falling through");
		if (g_SteeringShutdown) g_SteeringShutdown();
		FreeLibrary(m_steeringDll);
		m_steeringDll = NULL;
		return false;
	}

	g_bypassDIWrapper = false;  // Restore wrapper

	// Init failed
	Log("  Steering SDK init failed - falling through");
	FreeLibrary(m_steeringDll);
	m_steeringDll = NULL;
	return false;
}

// --- Phase 1: G Hub LED SDK ---

bool LogitechLED::TrySDK()
{
	Log("=== Phase 1: G Hub LED SDK ===");

#ifdef _WIN64
	const char* dllPath = "C:\\Program Files\\LGHUB\\sdks\\sdk_legacy_led_x64.dll";
#else
	const char* dllPath = "C:\\Program Files\\LGHUB\\sdks\\sdk_legacy_led_x86.dll";
#endif

	m_sdkDll = LoadLibraryA(dllPath);
	if (!m_sdkDll)
	{
		Log("  DLL not found: %s (err=%lu)", dllPath, GetLastError());
		return false;
	}

	Log("  Loaded: %s", dllPath);
	DumpExports(m_sdkDll, "LED SDK");

	g_LedInit       = (LogiLedInit_t)GetProcAddress(m_sdkDll, "LogiLedInit");
	g_LedSetTarget  = (LogiLedSetTargetDevice_t)GetProcAddress(m_sdkDll, "LogiLedSetTargetDevice");
	g_LedSetLighting = (LogiLedSetLighting_t)GetProcAddress(m_sdkDll, "LogiLedSetLighting");
	g_LedShutdown   = (LogiLedShutdown_t)GetProcAddress(m_sdkDll, "LogiLedShutdown");
	g_LedGetVersion = (LogiLedGetSdkVersion_t)GetProcAddress(m_sdkDll, "LogiLedGetSdkVersion");

	if (g_LedGetVersion)
	{
		int major = 0, minor = 0, build = 0;
		if (g_LedGetVersion(&major, &minor, &build))
			Log("  SDK version: %d.%d.%d", major, minor, build);
	}

	if (!g_LedInit)
	{
		FreeLibrary(m_sdkDll);
		m_sdkDll = NULL;
		return false;
	}

	bool ok = g_LedInit();
	Log("  LogiLedInit() -> %s", ok ? "OK" : "FAIL");
	if (!ok)
	{
		FreeLibrary(m_sdkDll);
		m_sdkDll = NULL;
		return false;
	}

	Sleep(500);

	g_LedSetZone = (LogiLedSetZone_t)GetProcAddress(m_sdkDll, "LogiLedSetLightingForTargetZone");

	if (g_LedSetZone && g_LedSetTarget)
	{
		// Target device type 0x8 (responded OK in previous tests)
		g_LedSetTarget(0x8);

		// Test: light up zone 0 and 1 with bright green (RPM LED color)
		bool z0 = g_LedSetZone(0x8, 0, 0, 100, 0);
		bool z1 = g_LedSetZone(0x8, 1, 0, 100, 0);
		Log("  devType=0x8 zone0(green) -> %s, zone1(green) -> %s", z0 ? "OK" : "FAIL", z1 ? "OK" : "FAIL");

		if (z0 || z1)
		{
			Log("  *** Activating LED SDK with devType=0x8 ***");
			Log("  Waiting 2s to check if LEDs are visible...");
			Sleep(2000);

			// Clear
			g_LedSetZone(0x8, 0, 0, 0, 0);
			g_LedSetZone(0x8, 1, 0, 0, 0);

			m_method = METHOD_SDK;
			m_available = true;
			Log("=== LED CONTROL ACTIVE (LED SDK devType=0x8) ===");
			return true;
		}
	}

	// devType=0x8 didn't work, clean up
	Log("  LED SDK: no usable zones found");
	if (g_LedShutdown) g_LedShutdown();
	FreeLibrary(m_sdkDll);
	m_sdkDll = NULL;
	g_LedInit = NULL;
	g_LedSetLighting = NULL;
	g_LedShutdown = NULL;
	g_LedSetZone = NULL;
	return false;
}

// --- I/O helpers ---

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

// --- Enumerate HID collections ---

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

// --- HID++ 2.0 Discovery (Phase 1) ---

bool LogitechLED::TryHIDPPDiscovery(HANDLE h, USHORT outLen, USHORT inLen)
{
	BYTE reportId = (outLen <= 20) ? 0x11 : 0x12;
	BYTE devIdx = 0xFF;
	BYTE req[64] = {0};
	BYTE resp[64] = {0};

	// Query IRoot for IFeatureSet
	req[0] = reportId; req[1] = devIdx; req[2] = 0x00;
	req[3] = 0x01; req[4] = 0x00; req[5] = 0x01;

	if (!SendReport(h, req, outLen)) return false;
	if (!ReadReport(h, resp, inLen, 2000)) return false;
	if (resp[2] == 0xFF || resp[4] == 0) return false;

	BYTE ifsIdx = resp[4];
	Log("  HID++ 2.0: IFeatureSet at index %d", ifsIdx);

	// Get count
	memset(req, 0, sizeof(req));
	req[0] = reportId; req[1] = devIdx; req[2] = ifsIdx;
	req[3] = 0x01;
	if (!SendReport(h, req, outLen)) return false;
	memset(resp, 0, sizeof(resp));
	if (!ReadReport(h, resp, inLen, 2000)) return false;

	int count = resp[4];
	Log("  %d features (enumeration only)", count);

	for (int fi = 1; fi <= count && fi < 128; fi++)
	{
		memset(req, 0, sizeof(req));
		req[0] = reportId; req[1] = devIdx; req[2] = ifsIdx;
		req[3] = (1 << 4) | 0x01; req[4] = (BYTE)fi;
		if (!SendReport(h, req, outLen)) continue;
		memset(resp, 0, sizeof(resp));
		if (!ReadReport(h, resp, inLen, 500)) continue;
		USHORT fid = ((USHORT)resp[4] << 8) | resp[5];
		Log("    [%02d] 0x%04X", fi, fid);
	}

	return false;  // Don't activate - kernel drivers block HID commands
}

// --- Legacy (Phase 2) ---

bool LogitechLED::TryLegacy(HANDLE h, USHORT outLen)
{
	BYTE reportId = (outLen <= 20) ? 0x11 : 0x12;
	BYTE rpt[64] = {0};
	rpt[0] = reportId; rpt[1] = 0xF8; rpt[2] = 0x12; rpt[3] = 0x1F;

	DWORD written = 0;
	BOOL ok = WriteFile(h, rpt, outLen, &written, NULL);
	bool success = ok && written > 0;

	Log("  Legacy [%02X F8 12 1F] -> %s", reportId, success ? "OK" : "FAIL");
	if (!success) return false;

	Sleep(500);
	rpt[3] = 0x00;
	WriteFile(h, rpt, outLen, &written, NULL);

	m_method = METHOD_LEGACY;
	return true;
}

// --- Init ---

bool LogitechLED::Init()
{
	Log("=== LogitechLED Init v8 ===");
	Log("");

	if (m_available) return true;

	// Phase 0: Steering Wheel SDK (LogiPlayLeds)
	if (TrySteeringSDK())
		return true;

	// Phase 1: G Hub LED SDK (diagnostic - enumerate exports, scan zones)
	Log("");
	TrySDK();  // always returns false now, just diagnostic

	// Phase 2: HID enumeration + Legacy attempt
	Log("");
	Log("=== Phase 2: HID collections ===");

	HIDCandidate candidates[MAX_CANDIDATES];
	int numCandidates = 0;
	EnumerateCandidates(candidates, &numCandidates);
	Log("  %d writable collections", numCandidates);

	// Try legacy on ALL writable collections (not just UP=0xFF43)
	for (int ci = 0; ci < numCandidates; ci++)
	{
		HANDLE h = CreateFileA(candidates[ci].path,
			GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_EXISTING, 0, NULL);
		if (h == INVALID_HANDLE_VALUE) continue;

		// Try legacy [F8 12] on every collection
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
	Log("=== NO WORKING METHOD ===");
	Log("G Hub kernel drivers block HID LED commands.");
	Log("Try: taskkill /f /im lghub_agent.exe  (then relaunch game)");
	return false;
}

// --- Runtime LED control ---

static int g_setLedsCallCount = 0;

bool LogitechLED::SetLEDs(BYTE ledMask)
{
	if (!m_available) return false;

	if (m_method == METHOD_STEERING_SDK && g_PlayLeds && g_SteeringUpdate)
	{
		// Delegate to SetLEDsFromPercent which handles deferred init
		int numLeds = 0;
		for (int i = 0; i < 5; i++)
			if (ledMask & (1 << i)) numLeds++;
		return SetLEDsFromPercent(numLeds / 5.0);
	}

	if (m_method == METHOD_SDK && g_LedSetZone)
	{
		// Map 5-bit LED mask to devType=0x8 zones 0-1
		// Zone 0 = lower LEDs (green), Zone 1 = upper LEDs (red)
		int numLeds = 0;
		for (int i = 0; i < 5; i++)
			if (ledMask & (1 << i)) numLeds++;

		// Zone 0: green intensity based on LED count (first 3 LEDs)
		// Zone 1: red intensity for high RPM (last 2 LEDs)
		int greenPct = 0, redPct = 0;
		if (numLeds >= 1) greenPct = 33;
		if (numLeds >= 2) greenPct = 66;
		if (numLeds >= 3) greenPct = 100;
		if (numLeds >= 4) redPct = 50;
		if (numLeds >= 5) redPct = 100;

		g_LedSetTarget(0x8);
		bool z0 = g_LedSetZone(0x8, 0, 0, greenPct, 0);
		bool z1 = g_LedSetZone(0x8, 1, redPct, 0, 0);

		g_setLedsCallCount++;
		if (g_setLedsCallCount <= 20 || (!z0 && !z1))
			Log("SetLEDs(0x%02X) [SDK 0x8 g=%d r=%d] -> z0=%s z1=%s (#%d)",
				ledMask, greenPct, redPct,
				z0 ? "OK" : "FAIL", z1 ? "OK" : "FAIL", g_setLedsCallCount);

		return z0 || z1;
	}

	if (m_handle == INVALID_HANDLE_VALUE) return false;

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
	}

	return success;
}

bool LogitechLED::SetLEDsFromPercent(double percent)
{
	if (percent < 0.0) percent = 0.0;
	if (percent > 1.0) percent = 1.0;

	// For steering SDK, pass percentage directly as RPM for smooth LED progression
	if (m_method == METHOD_STEERING_SDK && g_SteeringUpdate)
	{
		float rpm = (float)(percent * 100.0);
		g_SteeringUpdate();

		bool ok = false;

		// Prefer LogiPlayLedsDInput (works without SDK device detection)
		if (g_PlayLedsDInput && g_realDIDevice)
		{
			ok = g_PlayLedsDInput(g_realDIDevice, rpm, 0.0f, 100.0f);
			g_setLedsCallCount++;
			if (g_setLedsCallCount <= 20 || !ok)
				Log("SetLEDsFromPercent(%.2f) [DInput rpm=%.0f] -> %s (#%d)",
					percent, rpm, ok ? "OK" : "FAIL", g_setLedsCallCount);
		}
		else if (g_PlayLeds)
		{
			ok = g_PlayLeds(0, rpm, 0.0f, 100.0f);
			g_setLedsCallCount++;
			if (g_setLedsCallCount <= 20 || !ok)
				Log("SetLEDsFromPercent(%.2f) [SteeringSDK rpm=%.0f] -> %s (#%d)",
					percent, rpm, ok ? "OK" : "FAIL", g_setLedsCallCount);
		}

		return ok;
	}

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
