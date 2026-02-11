#include "LogitechLED.h"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

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
typedef void (__cdecl *LogiSteeringShutdown_t)();

static LogiSteeringInit_t            g_SteeringInit = NULL;
static LogiSteeringInitWithWindow_t  g_SteeringInitWithWindow = NULL;
static LogiUpdate_t                  g_SteeringUpdate = NULL;
static LogiIsConnected_t             g_IsConnected = NULL;
static LogiPlayLeds_t                g_PlayLeds = NULL;
static LogiSteeringShutdown_t        g_SteeringShutdown = NULL;
static bool                          g_SteeringNeedsLateInit = false;

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

static LogiLedInit_t                     g_LedInit = NULL;
static LogiLedInitWithName_t             g_LedInitWithName = NULL;
static LogiLedSetTargetDevice_t          g_LedSetTarget = NULL;
static LogiLedSetLighting_t              g_LedSetLighting = NULL;
static LogiLedSaveCurrentLighting_t      g_LedSave = NULL;
static LogiLedRestoreLighting_t          g_LedRestore = NULL;
static LogiLedShutdown_t                 g_LedShutdown = NULL;
static LogiLedGetSdkVersion_t            g_LedGetVersion = NULL;

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
	if (m_method == METHOD_STEERING_SDK && g_SteeringShutdown)
		g_SteeringShutdown();

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
	g_SteeringShutdown = NULL;
	g_SteeringNeedsLateInit = false;
	g_LedInit = NULL;
	g_LedSetLighting = NULL;
	g_LedShutdown = NULL;
}

bool LogitechLED::IsAvailable() const
{
	return m_available;
}

// --- Phase 0: Logitech Steering Wheel SDK ---

bool LogitechLED::TrySteeringSDK()
{
	Log("=== Phase 0: Steering Wheel SDK ===");

	// Search paths for LogitechSteeringWheelEnginesWrapper.dll
	const char* searchPaths[] = {
		// 1. Same directory as game/DLL (LoadLibrary default search)
		"LogitechSteeringWheelEnginesWrapper.dll",
		// 2. Logitech Steering Wheel SDK install (x86)
		"C:\\Program Files\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x86\\LogitechSteeringWheelEnginesWrapper.dll",
		"C:\\Program Files (x86)\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x86\\LogitechSteeringWheelEnginesWrapper.dll",
#ifdef _WIN64
		// 3. x64 variants
		"C:\\Program Files\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x64\\LogitechSteeringWheelEnginesWrapper.dll",
		"C:\\Program Files (x86)\\Logitech\\Logitech Steering Wheel SDK\\Lib\\GameEnginesWrapper\\x64\\LogitechSteeringWheelEnginesWrapper.dll",
#endif
		// 4. G Hub directory (unlikely but check)
		"C:\\Program Files\\LGHUB\\LogitechSteeringWheelEnginesWrapper.dll",
		"C:\\Program Files\\LGHUB\\sdks\\LogitechSteeringWheelEnginesWrapper.dll",
		NULL
	};

	for (int i = 0; searchPaths[i]; i++)
	{
		m_steeringDll = LoadLibraryA(searchPaths[i]);
		if (m_steeringDll)
		{
			Log("  Loaded: %s", searchPaths[i]);
			break;
		}
	}

	if (!m_steeringDll)
	{
		Log("  DLL not found. Searched:");
		for (int i = 0; searchPaths[i]; i++)
			Log("    - %s", searchPaths[i]);
		Log("  >>> Place LogitechSteeringWheelEnginesWrapper.dll next to the game .exe <<<");
		return false;
	}

	// Get function pointers - try plain names first, then mangled
	const char* initNames[] = { "LogiSteeringInitialize", "_LogiSteeringInitialize", NULL };
	const char* updateNames[] = { "LogiUpdate", "_LogiUpdate", NULL };
	const char* connNames[] = { "LogiIsConnected", "_LogiIsConnected", NULL };
	const char* ledsNames[] = { "LogiPlayLeds", "_LogiPlayLeds", NULL };
	const char* shutNames[] = { "LogiSteeringShutdown", "_LogiSteeringShutdown", NULL };

	for (int i = 0; initNames[i] && !g_SteeringInit; i++)
		g_SteeringInit = (LogiSteeringInit_t)GetProcAddress(m_steeringDll, initNames[i]);
	for (int i = 0; updateNames[i] && !g_SteeringUpdate; i++)
		g_SteeringUpdate = (LogiUpdate_t)GetProcAddress(m_steeringDll, updateNames[i]);
	for (int i = 0; connNames[i] && !g_IsConnected; i++)
		g_IsConnected = (LogiIsConnected_t)GetProcAddress(m_steeringDll, connNames[i]);
	for (int i = 0; ledsNames[i] && !g_PlayLeds; i++)
		g_PlayLeds = (LogiPlayLeds_t)GetProcAddress(m_steeringDll, ledsNames[i]);
	for (int i = 0; shutNames[i] && !g_SteeringShutdown; i++)
		g_SteeringShutdown = (LogiSteeringShutdown_t)GetProcAddress(m_steeringDll, shutNames[i]);

	// Also try InitWithWindow variant
	g_SteeringInitWithWindow = (LogiSteeringInitWithWindow_t)
		GetProcAddress(m_steeringDll, "LogiSteeringInitializeWithWindow");

	Log("  LogiSteeringInitialize: %s", g_SteeringInit ? "FOUND" : "NOT FOUND");
	Log("  LogiSteeringInitializeWithWindow: %s", g_SteeringInitWithWindow ? "FOUND" : "NOT FOUND");
	Log("  LogiUpdate: %s", g_SteeringUpdate ? "FOUND" : "NOT FOUND");
	Log("  LogiIsConnected: %s", g_IsConnected ? "FOUND" : "NOT FOUND");
	Log("  LogiPlayLeds: %s", g_PlayLeds ? "FOUND" : "NOT FOUND");
	Log("  LogiSteeringShutdown: %s", g_SteeringShutdown ? "FOUND" : "NOT FOUND");

	if (!g_SteeringUpdate || !g_IsConnected || !g_PlayLeds)
	{
		Log("  Required functions missing");
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
		// Init succeeded now - enumerate and test
		Log("  Init succeeded, enumerating...");

		for (int retry = 0; retry < 10; retry++)
		{
			Sleep(200);
			g_SteeringUpdate();
			if (g_IsConnected(0))
			{
				Log("  Wheel connected at index 0 (after %d updates)", retry + 1);
				break;
			}
		}

		bool connected = g_IsConnected(0);
		Log("  LogiIsConnected(0) -> %s", connected ? "YES" : "NO");

		// Test LEDs
		g_SteeringUpdate();
		bool led = g_PlayLeds(0, 100.0f, 0.0f, 100.0f);
		Log("  LogiPlayLeds(0, 100, 0, 100) -> %s *** ALL LEDs ***", led ? "OK" : "FAIL");
		Sleep(1000);

		g_SteeringUpdate();
		g_PlayLeds(0, 0.0f, 0.0f, 100.0f);

		m_method = METHOD_STEERING_SDK;
		m_available = true;
		Log("=== LED CONTROL ACTIVE (Steering Wheel SDK) ===");
		return true;
	}

	// All immediate inits failed - defer to first SetLEDs call
	// (game window will exist by then)
	Log("  Immediate init failed - deferring to first SetLEDs call");
	g_SteeringNeedsLateInit = true;
	m_method = METHOD_STEERING_SDK;
	m_available = true;
	Log("=== LED CONTROL PENDING (Steering Wheel SDK - deferred init) ===");
	return true;
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

	// Enumerate ALL exports from the DLL using PE export table
	Log("  --- ALL DLL EXPORTS ---");
	BYTE* base = (BYTE*)m_sdkDll;
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
	if (dos->e_magic == IMAGE_DOS_SIGNATURE)
	{
		IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		if (nt->Signature == IMAGE_NT_SIGNATURE)
		{
			DWORD expRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
			if (expRVA)
			{
				IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + expRVA);
				DWORD* names = (DWORD*)(base + exp->AddressOfNames);
				for (DWORD i = 0; i < exp->NumberOfNames; i++)
					Log("    [%02lu] %s", i, (char*)(base + names[i]));
			}
		}
	}
	Log("  --- END EXPORTS ---");

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

	// Exhaustive scan: try many device type + zone combinations
	FARPROC pZone = GetProcAddress(m_sdkDll, "LogiLedSetLightingForTargetZone");
	if (pZone && g_LedSetTarget)
	{
		typedef bool (*SetZone_t)(int, int, int, int, int);
		SetZone_t setZone = (SetZone_t)pZone;

		// Try device types: known bitmasks + higher values + 0xFF
		int deviceTypes[] = { 0x01, 0x02, 0x04, 0x07, 0x08, 0x10, 0x20,
		                      0x40, 0x80, 0xFF, 0x0100, 0x0200, -1 };

		for (int di = 0; deviceTypes[di] != -1; di++)
		{
			g_LedSetTarget(deviceTypes[di]);
			for (int zone = 0; zone < 10; zone++)
			{
				bool zok = setZone(deviceTypes[di], zone, 100, 100, 100);
				if (zok)
				{
					Log("  *** HIT *** SetZone(devType=0x%X, zone=%d) -> OK", deviceTypes[di], zone);
					Sleep(500);
				}
			}
		}

		// Also try SetZone with ordinal device types (0, 1, 2, 3, 4, 5)
		for (int dt = 0; dt <= 5; dt++)
		{
			for (int zone = 0; zone < 10; zone++)
			{
				bool zok = setZone(dt, zone, 100, 100, 100);
				if (zok)
					Log("  *** HIT *** SetZone(ordinal=%d, zone=%d) -> OK", dt, zone);
			}
		}
	}

	// Don't activate LED SDK - it doesn't control wheel LEDs
	Log("  LED SDK scan complete (keyboard/mouse only)");
	if (g_LedShutdown) g_LedShutdown();
	FreeLibrary(m_sdkDll);
	m_sdkDll = NULL;
	g_LedInit = NULL;
	g_LedSetLighting = NULL;
	g_LedShutdown = NULL;
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
	Log("=== LogitechLED Init v6 ===");
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

	if (m_method == METHOD_SDK && g_LedSetLighting)
	{
		int numLeds = 0;
		for (int i = 0; i < 5; i++)
			if (ledMask & (1 << i)) numLeds++;

		int pct = numLeds * 20;
		bool ok = g_LedSetLighting(pct, pct, pct);

		g_setLedsCallCount++;
		if (g_setLedsCallCount <= 20 || !ok)
			Log("SetLEDs(0x%02X) [LedSDK pct=%d] -> %s (#%d)",
				ledMask, pct, ok ? "OK" : "FAIL", g_setLedsCallCount);

		return ok;
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
	if (m_method == METHOD_STEERING_SDK && g_PlayLeds && g_SteeringUpdate)
	{
		// Deferred init: game window exists now
		if (g_SteeringNeedsLateInit)
		{
			g_SteeringNeedsLateInit = false;
			bool ok = false;

			HWND fg = GetForegroundWindow();
			if (fg && g_SteeringInitWithWindow)
			{
				ok = g_SteeringInitWithWindow(false, fg);
				Log("  LATE InitWithWindow(false, hwnd=0x%p) -> %s", fg, ok ? "OK" : "FAIL");
			}
			if (!ok && g_SteeringInit)
			{
				ok = g_SteeringInit(false);
				Log("  LATE LogiSteeringInitialize(false) -> %s", ok ? "OK" : "FAIL");
			}

			if (ok)
			{
				for (int retry = 0; retry < 10; retry++)
				{
					Sleep(200);
					g_SteeringUpdate();
					if (g_IsConnected(0))
					{
						Log("  LATE: Wheel connected (after %d updates)", retry + 1);
						break;
					}
				}
				Log("  LATE: LogiIsConnected(0) -> %s", g_IsConnected(0) ? "YES" : "NO");
			}
			else
			{
				Log("  LATE init also failed - steering SDK unavailable");
				m_method = METHOD_NONE;
				m_available = false;
				return false;
			}
		}

		float rpm = (float)(percent * 100.0);
		g_SteeringUpdate();
		bool ok = g_PlayLeds(0, rpm, 0.0f, 100.0f);

		g_setLedsCallCount++;
		if (g_setLedsCallCount <= 20 || !ok)
			Log("SetLEDsFromPercent(%.2f) [SteeringSDK rpm=%.0f] -> %s (#%d)",
				percent, rpm, ok ? "OK" : "FAIL", g_setLedsCallCount);

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
