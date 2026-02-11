#include "LogitechLED.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdarg.h>
#include <objbase.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "advapi32.lib")

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

// DirectInput device enumeration callback (must be __stdcall, not lambda)
struct EnumWheelCtx { GUID guid; bool found; };

static BOOL CALLBACK EnumWheelCB(LPCDIDEVICEINSTANCEA lpddi, LPVOID pvRef)
{
	EnumWheelCtx* ctx = (EnumWheelCtx*)pvRef;

	// Extract VID/PID from the device GUID
	DWORD vidpid = lpddi->guidProduct.Data1;
	USHORT vid = (USHORT)(vidpid & 0xFFFF);
	USHORT pid = (USHORT)((vidpid >> 16) & 0xFFFF);

	Log("  DI device: \"%s\" type=0x%08lX VID=0x%04X PID=0x%04X",
		lpddi->tszInstanceName, lpddi->dwDevType, vid, pid);

	if (vid == LOGITECH_VID && IsKnownPID(pid))
	{
		Log("  >>> Logitech wheel match! PID=0x%04X", pid);
		ctx->guid = lpddi->guidInstance;
		ctx->found = true;
		return DIENUM_STOP;
	}

	return DIENUM_CONTINUE;
}

bool LogitechLED::TrySteeringSDK()
{
	Log("=== Phase 0: Steering Wheel SDK (v12 - direct engine) ===");

	// --- Step 1: Load the steering wheel engine DLL ---
	// Priority 1: Load the REAL engine directly via registry (G Hub v9.1.0+)
	// G Hub registers: HKLM\SOFTWARE\[WOW6432Node\]Classes\CLSID\{63BD165D-...}\ServerBinary
	// The engine v9.1.0 contains G923 PID 0xC26E (confirmed by binary scan)
	// The old wrapper v8.75.30 is too outdated and fails to forward properly

	const char* regPaths[] = {
#ifndef _WIN64
		"SOFTWARE\\WOW6432Node\\Classes\\CLSID\\{63BD165D-1584-4E75-AB56-08330350545F}\\ServerBinary",
#endif
		"SOFTWARE\\Classes\\CLSID\\{63BD165D-1584-4E75-AB56-08330350545F}\\ServerBinary",
		NULL
	};

	for (int i = 0; regPaths[i] && !m_steeringDll; i++)
	{
		HKEY hKey = NULL;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regPaths[i], 0, KEY_READ, &hKey) == ERROR_SUCCESS)
		{
			char dllPath[MAX_PATH] = {0};
			DWORD pathSize = MAX_PATH;
			DWORD type = 0;
			if (RegQueryValueExA(hKey, NULL, NULL, &type, (LPBYTE)dllPath, &pathSize) == ERROR_SUCCESS
				&& type == REG_SZ && dllPath[0])
			{
				Log("  Registry: %s", regPaths[i]);
				Log("  Engine: %s", dllPath);
				m_steeringDll = LoadLibraryA(dllPath);
				if (m_steeringDll)
					Log("  *** DIRECT ENGINE LOADED (bypassing old wrapper) ***");
				else
					Log("  LoadLibrary failed (err=%lu)", GetLastError());
			}
			RegCloseKey(hKey);
		}
	}

	// Priority 2: Fall back to old wrapper in game directory
	if (!m_steeringDll)
	{
		Log("  Registry load failed, trying wrapper fallback...");
		const char* fallbackPaths[] = {
			"LogitechSteeringWheelEnginesWrapper.dll",
#ifdef _WIN64
			"C:\\Program Files\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x64\\LogitechSteeringWheelEnginesWrapper.dll",
#else
			"C:\\Program Files\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x86\\LogitechSteeringWheelEnginesWrapper.dll",
			"C:\\Program Files (x86)\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x86\\LogitechSteeringWheelEnginesWrapper.dll",
#endif
			NULL
		};

		for (int i = 0; fallbackPaths[i] && !m_steeringDll; i++)
		{
			m_steeringDll = LoadLibraryA(fallbackPaths[i]);
			if (m_steeringDll)
				Log("  Loaded wrapper: %s", fallbackPaths[i]);
		}
	}

	if (!m_steeringDll)
	{
		Log("  No steering wheel DLL found");
		return false;
	}

	DumpExports(m_steeringDll, "SteeringEngine");

	// --- Step 2: Resolve function pointers ---
	g_SteeringInitWithWindow = (LogiSteeringInitWithWindow_t)GetProcAddress(m_steeringDll, "LogiSteeringInitializeWithWindow");
	g_SteeringInit           = (LogiSteeringInit_t)GetProcAddress(m_steeringDll, "LogiSteeringInitialize");
	g_SteeringUpdate         = (LogiUpdate_t)GetProcAddress(m_steeringDll, "LogiUpdate");
	g_IsConnected            = (LogiIsConnected_t)GetProcAddress(m_steeringDll, "LogiIsConnected");
	g_PlayLeds               = (LogiPlayLeds_t)GetProcAddress(m_steeringDll, "LogiPlayLeds");
	g_PlayLedsDInput         = (LogiPlayLedsDInput_t)GetProcAddress(m_steeringDll, "LogiPlayLedsDInput");
	g_SteeringShutdown       = (LogiSteeringShutdown_t)GetProcAddress(m_steeringDll, "LogiSteeringShutdown");

	Log("  Functions: Init=%s InitWnd=%s Update=%s Connected=%s Leds=%s LedsDI=%s Shut=%s",
		g_SteeringInit ? "OK" : "-",
		g_SteeringInitWithWindow ? "OK" : "-",
		g_SteeringUpdate ? "OK" : "-",
		g_IsConnected ? "OK" : "-",
		g_PlayLeds ? "OK" : "-",
		g_PlayLedsDInput ? "OK" : "-",
		g_SteeringShutdown ? "OK" : "-");

	if (!g_SteeringUpdate || !g_PlayLeds)
	{
		Log("  Required functions missing (Update/PlayLeds)");
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

	// --- Step 3: Initialize the engine ---
	// Enable DInput bypass: when the engine calls DirectInput8Create internally,
	// our dinput8.dll wrapper must return the real interface (not our hook)
	Log("  Enabling DInput bypass for engine enumeration...");
	g_bypassDIWrapper = true;

	bool ok = false;

	if (!ok && g_SteeringInitWithWindow)
	{
		HWND desktop = GetDesktopWindow();
		ok = g_SteeringInitWithWindow(false, desktop);
		Log("  InitWithWindow(false, desktop=0x%p) -> %s", desktop, ok ? "OK" : "FAIL");
	}

	if (!ok && g_SteeringInit)
	{
		ok = g_SteeringInit(false);
		Log("  SteeringInitialize(false) -> %s", ok ? "OK" : "FAIL");
	}

	if (!ok)
	{
		g_bypassDIWrapper = false;
		Log("  Init failed");
		FreeLibrary(m_steeringDll);
		m_steeringDll = NULL;
		return false;
	}

	// --- Step 4: Wait for wheel detection ---
	Log("  Init OK, polling for wheel connection...");
	for (int retry = 0; retry < 20; retry++)
	{
		Sleep(300);
		g_SteeringUpdate();
		if (g_IsConnected && g_IsConnected(0))
		{
			Log("  Wheel connected at index 0 (after %d polls)", retry + 1);
			break;
		}
	}

	g_bypassDIWrapper = false;

	bool connected = g_IsConnected ? g_IsConnected(0) : false;
	Log("  IsConnected(0) -> %s", connected ? "YES" : "NO");

	// --- Step 5: Try LED control ---
	if (connected)
	{
		g_SteeringUpdate();
		bool led = g_PlayLeds(0, 100.0f, 0.0f, 100.0f);
		Log("  PlayLeds(0, 100, 0, 100) -> %s", led ? "OK" : "FAIL");

		if (led)
		{
			Sleep(1000);
			g_SteeringUpdate();
			g_PlayLeds(0, 0.0f, 0.0f, 100.0f);
			m_method = METHOD_STEERING_SDK;
			m_available = true;
			Log("=== LED CONTROL ACTIVE (direct engine, connected) ===");
			return true;
		}
		Log("  PlayLeds failed despite connection");
	}

	// --- Fallback A: blind PlayLeds (G Hub IPC may still route it) ---
	Log("  Fallback A: blind PlayLeds(0)...");
	g_SteeringUpdate();
	{
		bool ledA = g_PlayLeds(0, 100.0f, 0.0f, 100.0f);
		Log("  PlayLeds(0, 100, 0, 100) -> %s", ledA ? "OK" : "FAIL");
		if (ledA)
		{
			Sleep(2000);
			g_SteeringUpdate();
			g_PlayLeds(0, 0.0f, 0.0f, 100.0f);
			m_method = METHOD_STEERING_SDK;
			m_available = true;
			Log("=== LED CONTROL ACTIVE (direct engine, blind) ===");
			return true;
		}
	}

	// --- Fallback B: PlayLedsDInput(NULL) ---
	if (g_PlayLedsDInput)
	{
		Log("  Fallback B: PlayLedsDInput(NULL)...");
		g_SteeringUpdate();
		bool ledB = g_PlayLedsDInput(NULL, 100.0f, 0.0f, 100.0f);
		Log("  PlayLedsDInput(NULL, 100, 0, 100) -> %s", ledB ? "OK" : "FAIL");
		if (ledB)
		{
			Sleep(2000);
			g_SteeringUpdate();
			g_PlayLedsDInput(NULL, 0.0f, 0.0f, 100.0f);
			m_method = METHOD_STEERING_SDK;
			m_available = true;
			Log("=== LED CONTROL ACTIVE (direct engine, DInput NULL) ===");
			return true;
		}
	}

	// --- Fallback C: Create real DInput device + PlayLedsDInput ---
	if (g_PlayLedsDInput)
	{
		Log("  Fallback C: Real DirectInput device...");

		HRESULT comHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		Log("  CoInitializeEx -> 0x%08lX", comHr);

		char sysDir[MAX_PATH];
		GetSystemDirectoryA(sysDir, MAX_PATH);
		strcat_s(sysDir, MAX_PATH, "\\dinput8.dll");

		typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		HMODULE realDIDll = LoadLibraryA(sysDir);
		HMODULE ourDll = GetModuleHandleA("dinput8.dll");
		Log("  Real dinput8: %s -> 0x%p (ours=0x%p)", sysDir, realDIDll, ourDll);

		if (realDIDll && realDIDll == ourDll)
		{
			Log("  Conflict: resolved to our wrapper, trying SysWOW64...");
			FreeLibrary(realDIDll);
			realDIDll = LoadLibraryA("C:\\Windows\\SysWOW64\\dinput8.dll");
			Log("  SysWOW64 -> 0x%p", realDIDll);
		}

		DI8Create_t realCreate = realDIDll ?
			(DI8Create_t)GetProcAddress(realDIDll, "DirectInput8Create") : NULL;

		if (realCreate)
		{
			HRESULT hr = realCreate(GetModuleHandle(NULL), DIRECTINPUT_VERSION,
				IID_IDirectInput8A, (LPVOID*)&g_realDI, NULL);
			Log("  DirectInput8Create -> 0x%08lX", hr);

			if (SUCCEEDED(hr) && g_realDI)
			{
				EnumWheelCtx enumCtx;
				memset(&enumCtx, 0, sizeof(enumCtx));

				g_realDI->EnumDevices(DI8DEVCLASS_GAMECTRL,
					EnumWheelCB, &enumCtx, DIEDFL_ATTACHEDONLY);

				if (!enumCtx.found)
				{
					memset(&enumCtx, 0, sizeof(enumCtx));
					g_realDI->EnumDevices(0, EnumWheelCB, &enumCtx, DIEDFL_ALLDEVICES);
				}

				if (enumCtx.found)
				{
					hr = g_realDI->CreateDevice(enumCtx.guid, &g_realDIDevice, NULL);
					Log("  CreateDevice -> 0x%08lX", hr);

					if (SUCCEEDED(hr) && g_realDIDevice)
					{
						g_SteeringUpdate();
						bool led = g_PlayLedsDInput(g_realDIDevice, 100.0f, 0.0f, 100.0f);
						Log("  PlayLedsDInput(dev, 100, 0, 100) -> %s", led ? "OK" : "FAIL");

						if (led)
						{
							Sleep(1000);
							g_SteeringUpdate();
							g_PlayLedsDInput(g_realDIDevice, 0.0f, 0.0f, 100.0f);
							m_method = METHOD_STEERING_SDK;
							m_available = true;
							Log("=== LED CONTROL ACTIVE (direct engine + DInput device) ===");
							if (SUCCEEDED(comHr)) CoUninitialize();
							return true;
						}

						g_realDIDevice->Release();
						g_realDIDevice = NULL;
					}
				}
				else
				{
					Log("  No Logitech wheel found via DirectInput");
				}

				g_realDI->Release();
				g_realDI = NULL;
			}
		}

		if (SUCCEEDED(comHr)) CoUninitialize();
	}

	Log("  All methods exhausted");
	if (g_SteeringShutdown) g_SteeringShutdown();
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
	Log("=== LogitechLED Init v12 (direct engine) ===");
	Log("");

	if (m_available) return true;

	// Phase 0: Steering Wheel SDK (LogiPlayLeds)
	if (TrySteeringSDK())
		return true;

	// Phase 1: G Hub LED SDK
	Log("");
	if (TrySDK())
		return true;

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
