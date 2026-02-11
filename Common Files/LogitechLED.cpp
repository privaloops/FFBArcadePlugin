#include "LogitechLED.h"

#include <windows.h>
#include <winhttp.h>
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
#pragma comment(lib, "winhttp.lib")

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
typedef bool (__cdecl *LogiIsDeviceConnected_t)(int, int);
typedef bool (__cdecl *LogiIsModelConnected_t)(int, int);
typedef bool (__cdecl *LogiIsManufacturerConnected_t)(int, int);
typedef bool (__cdecl *LogiGetFriendlyProductName_t)(int, wchar_t*, int);
typedef bool (__cdecl *LogiPlayLeds_t)(int, float, float, float);
typedef bool (__cdecl *LogiPlayLedsDInput_t)(LPVOID, float, float, float);
typedef void (__cdecl *LogiSteeringShutdown_t)();
typedef int  (__cdecl *LogiGetSdkVersion_t)();

static LogiSteeringInit_t            g_SteeringInit = NULL;
static LogiSteeringInitWithWindow_t  g_SteeringInitWithWindow = NULL;
static LogiUpdate_t                  g_SteeringUpdate = NULL;
static LogiIsConnected_t             g_IsConnected = NULL;
static LogiIsDeviceConnected_t       g_IsDeviceConnected = NULL;
static LogiIsModelConnected_t        g_IsModelConnected = NULL;
static LogiIsManufacturerConnected_t g_IsManufacturerConnected = NULL;
static LogiGetFriendlyProductName_t  g_GetFriendlyName = NULL;
static LogiPlayLeds_t                g_PlayLeds = NULL;
static LogiPlayLedsDInput_t          g_PlayLedsDInput = NULL;
static LogiSteeringShutdown_t        g_SteeringShutdown = NULL;
static LogiGetSdkVersion_t           g_GetSdkVersion = NULL;
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

// Discovered LED SDK configuration (set during TrySDK scan)
static int  g_sdkDevType = 0x8;   // Device type for zone API
static int  g_sdkMaxZone = 1;     // Highest zone that returned OK
static bool g_sdkUseGlobal = false; // Use SetLighting instead of zones
static int  g_sdkGlobalTarget = 0x1; // Target for global SetLighting

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
	g_IsDeviceConnected = NULL;
	g_IsModelConnected = NULL;
	g_IsManufacturerConnected = NULL;
	g_GetFriendlyName = NULL;
	g_PlayLeds = NULL;
	g_PlayLedsDInput = NULL;
	g_SteeringShutdown = NULL;
	g_GetSdkVersion = NULL;
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

// --- Phase -1: G Hub WebSocket Registration ---

// Simple JSON value extractor (no library needed)
static bool JsonGetString(const char* json, const char* key, char* out, int outSize)
{
	char search[128];
	snprintf(search, sizeof(search), "\"%s\"", key);
	const char* p = strstr(json, search);
	if (!p) return false;
	p += strlen(search);
	// Skip whitespace and colon
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	if (*p != '"') return false;
	p++; // skip opening quote
	int i = 0;
	while (*p && *p != '"' && i < outSize - 1)
		out[i++] = *p++;
	out[i] = 0;
	return i > 0;
}

static bool WS_Send(HINTERNET hWS, const char* json)
{
	DWORD err = WinHttpWebSocketSend(hWS,
		WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
		(PVOID)json, (DWORD)strlen(json));
	return err == ERROR_SUCCESS;
}

static bool WS_Recv(HINTERNET hWS, char* buf, int bufSize, DWORD* bytesRead)
{
	*bytesRead = 0;
	DWORD totalRead = 0;
	WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;

	while (totalRead < (DWORD)(bufSize - 1))
	{
		DWORD read = 0;
		DWORD err = WinHttpWebSocketReceive(hWS,
			buf + totalRead, bufSize - 1 - totalRead, &read, &bufType);
		if (err != ERROR_SUCCESS) return false;
		totalRead += read;

		if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
			bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
			break;
	}

	buf[totalRead] = 0;
	*bytesRead = totalRead;
	return totalRead > 0;
}

// G Hub WebSocket registration state
static char g_ghubDeviceId[64] = {0};
static char g_ghubInstanceGuid[64] = {0};
static char g_ghubIntegrationGuid[64] = {0};
static bool g_ghubRegistered = false;

bool LogitechLED::TryGHubWebSocket()
{
	Log("=== Phase -1: G Hub WebSocket Registration ===");

	Log("  Opening WinHTTP session...");
	HINTERNET hSession = WinHttpOpen(L"FFBArcadePlugin/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
	if (!hSession) { Log("  WinHttpOpen failed (err=%lu)", GetLastError()); return false; }

	// Set timeouts: 5s connect, 5s send, 5s receive
	DWORD timeout = 5000;
	WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
	WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
	WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
	DWORD resolveTimeout = 3000;
	WinHttpSetOption(hSession, WINHTTP_OPTION_RESOLVE_TIMEOUT, &resolveTimeout, sizeof(resolveTimeout));

	Log("  Connecting to localhost:9010...");
	HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", 9010, 0);
	if (!hConnect) { Log("  WinHttpConnect failed (err=%lu)", GetLastError()); WinHttpCloseHandle(hSession); return false; }

	Log("  Opening request...");
	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/",
		NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
	if (!hRequest)
	{
		Log("  WinHttpOpenRequest failed (err=%lu)", GetLastError());
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	// Enable WebSocket upgrade
	Log("  Setting WebSocket option...");
	BOOL optResult = WinHttpSetOption(hRequest,
		WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0);
	if (!optResult) { Log("  WebSocket option failed (err=%lu)", GetLastError()); }

	// Add required headers
	WinHttpAddRequestHeaders(hRequest,
		L"Sec-WebSocket-Protocol: json\r\nOrigin: file://",
		(DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

	// Send the upgrade request
	Log("  Sending upgrade request...");
	BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0);
	if (!sent)
	{
		Log("  WinHttpSendRequest failed (err=%lu)", GetLastError());
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	Log("  Waiting for response...");
	BOOL received = WinHttpReceiveResponse(hRequest, NULL);
	if (!received)
	{
		Log("  WinHttpReceiveResponse failed (err=%lu)", GetLastError());
		WinHttpCloseHandle(hRequest);
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	Log("  Completing WebSocket upgrade...");
	HINTERNET hWS = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
	WinHttpCloseHandle(hRequest); // No longer needed after upgrade
	if (!hWS)
	{
		Log("  WebSocket upgrade failed (err=%lu)", GetLastError());
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	Log("  WebSocket connected to G Hub!");

	char buf[65536];
	DWORD bytesRead = 0;
	char sendBuf[1024];
	int msgId = 1;

	// Drain initial OPTIONS message
	WS_Recv(hWS, buf, sizeof(buf), &bytesRead);
	if (bytesRead > 0) Log("  Initial: %.200s...", buf);

	// --- Step 1: GET /devices/list ---
	snprintf(sendBuf, sizeof(sendBuf),
		"{\"msgId\":\"%d\",\"verb\":\"GET\",\"path\":\"/devices/list\"}", msgId++);
	WS_Send(hWS, sendBuf);
	Sleep(500);
	if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
	{
		Log("  /devices/list -> %lu bytes", bytesRead);

		// Debug: dump JSON start to see actual formatting
		Log("  JSON[0..300]: %.300s", buf);

		// Fix: search for plain "STEERING_WHEEL" (G Hub JSON has spaces after colons)
		const char* swPos = strstr(buf, "STEERING_WHEEL");
		if (swPos)
		{
			Log("  STEERING_WHEEL at offset %d", (int)(swPos - buf));

			// Search backwards for device ID pattern "dev0..."
			const char* p = buf;
			const char* lastDev = NULL;
			while (p < swPos)
			{
				const char* found = strstr(p, "\"dev0");
				if (found && found < swPos)
				{
					lastDev = found + 1; // skip opening quote
					p = found + 5;
				}
				else break;
			}

			if (lastDev)
			{
				int i = 0;
				while (lastDev[i] && lastDev[i] != '"' && i < 63)
				{
					g_ghubDeviceId[i] = lastDev[i];
					i++;
				}
				g_ghubDeviceId[i] = 0;
				Log("  Found steering wheel: %s", g_ghubDeviceId);
			}
			else
				Log("  STEERING_WHEEL found but no device ID nearby");

			// Dump context around STEERING_WHEEL
			int ctxStart = (int)(swPos - buf);
			if (ctxStart > 200) ctxStart -= 200; else ctxStart = 0;
			Log("  Context: %.400s", buf + ctxStart);
		}
		else
			Log("  No STEERING_WHEEL in %lu bytes", bytesRead);
	}
	else
		Log("  No response to /devices/list");

	// --- Step 2: Register integration ---
	snprintf(sendBuf, sizeof(sendBuf),
		"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/api/v1/integration/register\","
		"\"payload\":{\"integrationIdentifier\":\"ffb_arcade\","
		"\"name\":\"FFB Arcade Plugin\",\"author\":\"FFB\","
		"\"description\":\"RPM LED control\",\"manualRegistration\":true}}", msgId++);
	WS_Send(hWS, sendBuf);
	Sleep(500);
	if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
		Log("  Register: %.300s", buf);

	// --- Step 3: Activate WHEEL first (ACTION before WHEEL corrupts GUID) ---
	snprintf(sendBuf, sizeof(sendBuf),
		"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/api/v1/integration/activate\","
		"\"payload\":{\"integrationIdentifier\":\"ffb_arcade\",\"sdkType\":\"WHEEL\"}}", msgId++);
	WS_Send(hWS, sendBuf);
	Sleep(500);
	if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
	{
		Log("  Activate WHEEL: %.300s", buf);
		JsonGetString(buf, "instanceGuid", g_ghubInstanceGuid, sizeof(g_ghubInstanceGuid));
		JsonGetString(buf, "integrationGuid", g_ghubIntegrationGuid, sizeof(g_ghubIntegrationGuid));
		if (strstr(buf, "SUCCESS"))
		{
			g_ghubRegistered = true;
			Log("  WHEEL OK! instance=%s integration=%s",
				g_ghubInstanceGuid, g_ghubIntegrationGuid);
		}
		else
			Log("  WHEEL failed, trying ACTION fallback...");
	}

	// --- Step 3b: ACTION fallback ---
	if (!g_ghubRegistered)
	{
		snprintf(sendBuf, sizeof(sendBuf),
			"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/api/v1/integration/activate\","
			"\"payload\":{\"integrationIdentifier\":\"ffb_arcade\",\"sdkType\":\"ACTION\"}}", msgId++);
		WS_Send(hWS, sendBuf);
		Sleep(500);
		if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
		{
			Log("  Activate ACTION: %.200s", buf);
			if (!g_ghubIntegrationGuid[0])
				JsonGetString(buf, "integrationGuid", g_ghubIntegrationGuid, sizeof(g_ghubIntegrationGuid));
			if (!g_ghubInstanceGuid[0])
				JsonGetString(buf, "instanceGuid", g_ghubInstanceGuid, sizeof(g_ghubInstanceGuid));
		}
	}

	// --- Step 4: HID++ LED commands via WebSocket ---
	// If G Hub can relay HID++ to the wheel, this bypasses the kernel driver
	if (g_ghubDeviceId[0])
	{
		Log("  --- HID++ LED tests via WebSocket ---");

		// A) HID++ setup: [0x11,0xFF,0x12,0x31,0x00] = feat 0x12, func 3 (enable)
		snprintf(sendBuf, sizeof(sendBuf),
			"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/api/v1/devices/%s/hid\","
			"\"payload\":{\"data\":[17,255,18,49,0]}}", msgId++, g_ghubDeviceId);
		WS_Send(hWS, sendBuf);
		Sleep(500);
		if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
			Log("  HID++ setup -> %.200s", buf);

		// B) HID++ LEDs ALL ON: [0x11,0xFF,0x12,0x51,0x00,0x05,0x1F]
		snprintf(sendBuf, sizeof(sendBuf),
			"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/api/v1/devices/%s/hid\","
			"\"payload\":{\"data\":[17,255,18,81,0,5,31]}}", msgId++, g_ghubDeviceId);
		WS_Send(hWS, sendBuf);
		Sleep(500);
		if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
		{
			Log("  HID++ LEDs -> %.200s", buf);
			if (strstr(buf, "SUCCESS"))
			{
				Log("  >>> HID++ ACCEPTED! CHECK WHEEL LEDs! (3s)");
				Sleep(3000);
			}
		}

		// C) Alternate HID path
		snprintf(sendBuf, sizeof(sendBuf),
			"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/devices/%s/hid\","
			"\"payload\":{\"data\":[17,255,18,81,0,5,31]}}", msgId++, g_ghubDeviceId);
		WS_Send(hWS, sendBuf);
		Sleep(300);
		if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
			Log("  HID++ (alt path) -> %.200s", buf);

		// D) WHEEL SDK RPM endpoint (if WHEEL activation succeeded)
		if (g_ghubInstanceGuid[0])
		{
			snprintf(sendBuf, sizeof(sendBuf),
				"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/api/v1/sdk/wheel/rpm\","
				"\"payload\":{\"instanceGuid\":\"%s\",\"deviceId\":\"%s\","
				"\"currentRpm\":8000,\"rpmMax\":9000,\"rpmRedLine\":8500}}",
				msgId++, g_ghubInstanceGuid, g_ghubDeviceId);
			WS_Send(hWS, sendBuf);
			Sleep(500);
			if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
			{
				Log("  WHEEL RPM -> %.200s", buf);
				if (strstr(buf, "SUCCESS"))
				{
					Log("  >>> RPM ACCEPTED! CHECK WHEEL LEDs! (3s)");
					Sleep(3000);
				}
			}
		}

		// E) Direct LED endpoints
		snprintf(sendBuf, sizeof(sendBuf),
			"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/api/v1/wheel/leds\","
			"\"payload\":{\"deviceId\":\"%s\",\"leds\":31,"
			"\"instanceGuid\":\"%s\",\"integrationGuid\":\"%s\"}}",
			msgId++, g_ghubDeviceId, g_ghubInstanceGuid, g_ghubIntegrationGuid);
		WS_Send(hWS, sendBuf);
		Sleep(300);
		if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
			Log("  /wheel/leds -> %.200s", buf);

		snprintf(sendBuf, sizeof(sendBuf),
			"{\"msgId\":\"%d\",\"verb\":\"SET\",\"path\":\"/api/v1/devices/%s/leds\","
			"\"payload\":{\"leds\":31}}", msgId++, g_ghubDeviceId);
		WS_Send(hWS, sendBuf);
		Sleep(300);
		if (WS_Recv(hWS, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
			Log("  /devices/leds -> %.200s", buf);
	}
	else
		Log("  No deviceId - skipping LED tests");

	// Keep WebSocket open briefly then close
	WinHttpWebSocketClose(hWS, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
	WinHttpCloseHandle(hWS);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	Log("  WebSocket done: device=%s registered=%s",
		g_ghubDeviceId[0] ? g_ghubDeviceId : "none",
		g_ghubRegistered ? "yes" : "no");
	return false; // Diagnostic only - other phases handle LED control
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
	Log("=== Phase 0: Steering Wheel SDK (v12b - direct engine + diagnostics) ===");

	// --- Step 0: Ensure COM is initialized (engine needs it for G Hub IPC) ---
	HRESULT comHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (comHr == RPC_E_CHANGED_MODE)
		comHr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	Log("  COM init -> 0x%08lX", comHr);

	// --- Step 1: Load the steering wheel engine DLL ---
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
					Log("  *** DIRECT ENGINE LOADED ***");
				else
					Log("  LoadLibrary failed (err=%lu)", GetLastError());
			}
			RegCloseKey(hKey);
		}
	}

	// Fallback: old wrapper
	if (!m_steeringDll)
	{
		Log("  Registry failed, trying wrapper...");
		const char* fallbackPaths[] = {
			"LogitechSteeringWheelEnginesWrapper.dll",
#ifndef _WIN64
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
		if (SUCCEEDED(comHr)) CoUninitialize();
		return false;
	}

	// --- Step 2: Resolve ALL function pointers ---
	g_SteeringInitWithWindow = (LogiSteeringInitWithWindow_t)GetProcAddress(m_steeringDll, "LogiSteeringInitializeWithWindow");
	g_SteeringInit           = (LogiSteeringInit_t)GetProcAddress(m_steeringDll, "LogiSteeringInitialize");
	g_SteeringUpdate         = (LogiUpdate_t)GetProcAddress(m_steeringDll, "LogiUpdate");
	g_IsConnected            = (LogiIsConnected_t)GetProcAddress(m_steeringDll, "LogiIsConnected");
	g_IsDeviceConnected      = (LogiIsDeviceConnected_t)GetProcAddress(m_steeringDll, "LogiIsDeviceConnected");
	g_IsModelConnected       = (LogiIsModelConnected_t)GetProcAddress(m_steeringDll, "LogiIsModelConnected");
	g_IsManufacturerConnected = (LogiIsManufacturerConnected_t)GetProcAddress(m_steeringDll, "LogiIsManufacturerConnected");
	g_GetFriendlyName        = (LogiGetFriendlyProductName_t)GetProcAddress(m_steeringDll, "LogiGetFriendlyProductName");
	g_PlayLeds               = (LogiPlayLeds_t)GetProcAddress(m_steeringDll, "LogiPlayLeds");
	g_PlayLedsDInput         = (LogiPlayLedsDInput_t)GetProcAddress(m_steeringDll, "LogiPlayLedsDInput");
	g_SteeringShutdown       = (LogiSteeringShutdown_t)GetProcAddress(m_steeringDll, "LogiSteeringShutdown");
	g_GetSdkVersion          = (LogiGetSdkVersion_t)GetProcAddress(m_steeringDll, "LogiGetSdkVersion");

	Log("  Core: Init=%s InitWnd=%s Update=%s Shut=%s",
		g_SteeringInit ? "OK" : "-", g_SteeringInitWithWindow ? "OK" : "-",
		g_SteeringUpdate ? "OK" : "-", g_SteeringShutdown ? "OK" : "-");
	Log("  Detection: Connected=%s DevConn=%s ModelConn=%s MfgConn=%s Name=%s",
		g_IsConnected ? "OK" : "-", g_IsDeviceConnected ? "OK" : "-",
		g_IsModelConnected ? "OK" : "-", g_IsManufacturerConnected ? "OK" : "-",
		g_GetFriendlyName ? "OK" : "-");
	Log("  LEDs: PlayLeds=%s PlayLedsDI=%s",
		g_PlayLeds ? "OK" : "-", g_PlayLedsDInput ? "OK" : "-");

	if (g_GetSdkVersion)
	{
		int ver = g_GetSdkVersion();
		Log("  SDK version: %d (0x%X)", ver, ver);
	}

	if (!g_SteeringUpdate || !g_PlayLeds)
	{
		Log("  Required functions missing");
		FreeLibrary(m_steeringDll);
		m_steeringDll = NULL;
		if (SUCCEEDED(comHr)) CoUninitialize();
		return false;
	}

	if (!g_SteeringInit && !g_SteeringInitWithWindow)
	{
		Log("  No init function found");
		FreeLibrary(m_steeringDll);
		m_steeringDll = NULL;
		if (SUCCEEDED(comHr)) CoUninitialize();
		return false;
	}

	// --- Step 3: Initialize ---
	Log("  Enabling DInput bypass...");
	g_bypassDIWrapper = true;

	bool ok = false;

	// Try 1: InitWithWindow(false, desktop) - ignoreXInput=false
	if (!ok && g_SteeringInitWithWindow)
	{
		HWND desktop = GetDesktopWindow();
		ok = g_SteeringInitWithWindow(false, desktop);
		Log("  InitWithWindow(ignoreXI=false, desktop=0x%p) -> %s", desktop, ok ? "OK" : "FAIL");
	}

	// Try 2: InitWithWindow(true, desktop) - ignoreXInput=true (G923 Xbox is XInput!)
	if (!ok && g_SteeringInitWithWindow)
	{
		HWND desktop = GetDesktopWindow();
		ok = g_SteeringInitWithWindow(true, desktop);
		Log("  InitWithWindow(ignoreXI=true, desktop=0x%p) -> %s", desktop, ok ? "OK" : "FAIL");
	}

	// Try 3: Simple init
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
		if (SUCCEEDED(comHr)) CoUninitialize();
		return false;
	}

	// --- Step 4: Extended polling + full diagnostics ---
	Log("  Init OK, polling (40 iterations, 15s total)...");
	bool anyConnected = false;
	for (int retry = 0; retry < 40; retry++)
	{
		Sleep(400);
		g_SteeringUpdate();

		// Check indices 0-3
		for (int idx = 0; idx < 4; idx++)
		{
			if (g_IsConnected && g_IsConnected(idx))
			{
				Log("  *** IsConnected(%d) -> YES (poll %d) ***", idx, retry + 1);
				anyConnected = true;
			}
		}

		if (anyConnected) break;

		// Log progress every 10 polls
		if (retry == 9 || retry == 19 || retry == 29)
			Log("  Poll %d/40: still no connection...", retry + 1);
	}

	g_bypassDIWrapper = false;

	// --- Detailed diagnostics ---
	Log("  --- DETECTION DIAGNOSTICS ---");

	// IsConnected for indices 0-3
	for (int idx = 0; idx < 4; idx++)
	{
		bool c = g_IsConnected ? g_IsConnected(idx) : false;
		if (c) Log("  IsConnected(%d) -> YES", idx);
	}
	if (!anyConnected)
		Log("  IsConnected(0..3) -> all NO");

	// IsDeviceConnected: try device types 0-6
	// 0=wheel, 1=joystick, 2=gamepad, 3=other, ...
	if (g_IsDeviceConnected)
	{
		for (int devType = 0; devType <= 6; devType++)
		{
			bool c = g_IsDeviceConnected(0, devType);
			if (c) Log("  IsDeviceConnected(0, type=%d) -> YES", devType);
		}
	}

	// IsModelConnected: G29=28, G920=27, try 25-35 for G923
	if (g_IsModelConnected)
	{
		for (int model = 25; model <= 35; model++)
		{
			bool c = g_IsModelConnected(0, model);
			if (c) Log("  IsModelConnected(0, model=%d) -> YES", model);
		}
	}

	// IsManufacturerConnected: Logitech=3 (guessing)
	if (g_IsManufacturerConnected)
	{
		for (int mfg = 0; mfg <= 5; mfg++)
		{
			bool c = g_IsManufacturerConnected(0, mfg);
			if (c) Log("  IsManufacturerConnected(0, mfg=%d) -> YES", mfg);
		}
	}

	// GetFriendlyProductName
	if (g_GetFriendlyName)
	{
		wchar_t name[256] = {0};
		bool got = g_GetFriendlyName(0, name, 256);
		if (got && name[0])
		{
			char narrow[256];
			WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, 256, NULL, NULL);
			Log("  FriendlyName(0) -> \"%s\"", narrow);
		}
		else
			Log("  FriendlyName(0) -> empty/fail");
	}

	Log("  --- END DIAGNOSTICS ---");

	// --- Step 5: Try LED control ---
	bool connected = anyConnected;

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
	}

	// --- Fallback A: blind PlayLeds on all indices ---
	Log("  Fallback A: blind PlayLeds on indices 0-3...");
	for (int idx = 0; idx < 4; idx++)
	{
		g_SteeringUpdate();
		bool ledA = g_PlayLeds(idx, 100.0f, 0.0f, 100.0f);
		Log("  PlayLeds(%d, 100, 0, 100) -> %s", idx, ledA ? "OK" : "FAIL");
		if (ledA)
		{
			Sleep(2000);
			g_SteeringUpdate();
			g_PlayLeds(idx, 0.0f, 0.0f, 100.0f);
			m_method = METHOD_STEERING_SDK;
			m_available = true;
			Log("=== LED CONTROL ACTIVE (direct engine, blind idx=%d) ===", idx);
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

	// --- Fallback C: DInput device via bypass ---
	if (g_PlayLedsDInput)
	{
		Log("  Fallback C: Real DInput device (bypass mode)...");
		g_bypassDIWrapper = true;

		typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		DI8Create_t realCreate = (DI8Create_t)GetProcAddress(
			GetModuleHandleA("dinput8.dll"), "DirectInput8Create");

		if (realCreate)
		{
			HRESULT hr = realCreate(GetModuleHandle(NULL), DIRECTINPUT_VERSION,
				IID_IDirectInput8A, (LPVOID*)&g_realDI, NULL);
			Log("  DirectInput8Create (bypass) -> 0x%08lX, DI=0x%p", hr, g_realDI);

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
							g_bypassDIWrapper = false;
							Log("=== LED CONTROL ACTIVE (direct engine + DInput) ===");
							return true;
						}

						g_realDIDevice->Release();
						g_realDIDevice = NULL;
					}
				}
				else
				{
					Log("  No Logitech wheel via DInput");
				}

				g_realDI->Release();
				g_realDI = NULL;
			}
		}
		g_bypassDIWrapper = false;
	}

	Log("  All methods exhausted");
	if (g_SteeringShutdown) g_SteeringShutdown();
	FreeLibrary(m_steeringDll);
	m_steeringDll = NULL;
	if (SUCCEEDED(comHr)) CoUninitialize();
	return false;
}

// --- Phase 1: G Hub LED SDK ---

bool LogitechLED::TrySDK()
{
	Log("=== Phase 1: G Hub LED SDK (v13 - exhaustive scan) ===");

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

	g_LedInit        = (LogiLedInit_t)GetProcAddress(m_sdkDll, "LogiLedInit");
	g_LedInitWithName = (LogiLedInitWithName_t)GetProcAddress(m_sdkDll, "LogiLedInitWithName");
	g_LedSetTarget   = (LogiLedSetTargetDevice_t)GetProcAddress(m_sdkDll, "LogiLedSetTargetDevice");
	g_LedSetLighting = (LogiLedSetLighting_t)GetProcAddress(m_sdkDll, "LogiLedSetLighting");
	g_LedShutdown    = (LogiLedShutdown_t)GetProcAddress(m_sdkDll, "LogiLedShutdown");
	g_LedGetVersion  = (LogiLedGetSdkVersion_t)GetProcAddress(m_sdkDll, "LogiLedGetSdkVersion");
	g_LedSetZone     = (LogiLedSetZone_t)GetProcAddress(m_sdkDll, "LogiLedSetLightingForTargetZone");

	Log("  Init=%s InitName=%s Target=%s Lighting=%s Zone=%s Shut=%s",
		g_LedInit ? "OK" : "-", g_LedInitWithName ? "OK" : "-",
		g_LedSetTarget ? "OK" : "-", g_LedSetLighting ? "OK" : "-",
		g_LedSetZone ? "OK" : "-", g_LedShutdown ? "OK" : "-");

	if (g_LedGetVersion)
	{
		int major = 0, minor = 0, build = 0;
		if (g_LedGetVersion(&major, &minor, &build))
			Log("  SDK version: %d.%d.%d", major, minor, build);
	}

	if (!g_LedInit && !g_LedInitWithName)
	{
		FreeLibrary(m_sdkDll);
		m_sdkDll = NULL;
		return false;
	}

	bool initOk = false;
	if (g_LedInit)
	{
		initOk = g_LedInit();
		Log("  LogiLedInit() -> %s", initOk ? "OK" : "FAIL");
	}
	if (!initOk && g_LedInitWithName)
	{
		initOk = g_LedInitWithName("FFBArcadePlugin");
		Log("  LogiLedInitWithName('FFBArcadePlugin') -> %s", initOk ? "OK" : "FAIL");
	}

	if (!initOk)
	{
		FreeLibrary(m_sdkDll);
		m_sdkDll = NULL;
		return false;
	}

	Log("  Init OK, waiting 1s for G Hub registration...");
	Sleep(1000);

	// ================================================================
	// SCAN A: Global LogiLedSetLighting with various target devices
	// ================================================================
	Log("  --- SCAN A: SetTargetDevice + SetLighting (green) ---");
	if (g_LedSetTarget && g_LedSetLighting)
	{
		// Known device types: 0x1=mono, 0x2=RGB, 0x4=perkey, 0x7=all
		// Also try: 0x8=headset, 0x10, 0x20, 0x40, 0x80, 0xFF
		int targets[] = { 0x1, 0x2, 0x3, 0x4, 0x7, 0x8, 0xE, 0x10, 0x20, 0x40, 0x80, 0xFF };
		int numTargets = sizeof(targets) / sizeof(targets[0]);

		for (int t = 0; t < numTargets; t++)
		{
			g_LedSetTarget(targets[t]);
			bool ok = g_LedSetLighting(0, 100, 0);  // Bright green
			Log("  target=0x%02X SetLighting(0,100,0) -> %s", targets[t], ok ? "OK" : "FAIL");
			if (ok)
			{
				Log("  >>> VISUAL A: target=0x%02X - CHECK WHEEL LEDs! (500ms)", targets[t]);
				Sleep(500);
				g_LedSetLighting(0, 0, 0);  // Turn off
				Sleep(200);
			}
		}
	}

	// ================================================================
	// SCAN B: LogiLedSetLightingForTargetZone - exhaustive
	// ================================================================
	if (g_LedSetZone)
	{
		Log("  --- SCAN B: SetLightingForTargetZone (exhaustive) ---");
		// Device types to test (covers known + potential undocumented)
		// 0x0=keyboard, 0x3=mouse, 0x4=mousemat, 0x8=headset, 0xE=speaker
		int zoneTypes[] = {
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
			0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
			0x10, 0x11, 0x12, 0x13, 0x14
		};
		int numZoneTypes = sizeof(zoneTypes) / sizeof(zoneTypes[0]);

		// Pass 1: silent scan to collect OK results
		Log("  Pass 1: collecting OK results...");
		struct ZoneResult { int devType; int maxZone; int okCount; };
		ZoneResult okResults[21];
		int numOkResults = 0;

		for (int dt = 0; dt < numZoneTypes; dt++)
		{
			int okCount = 0;
			int maxZone = -1;

			for (int zone = 0; zone <= 5; zone++)
			{
				bool ok = g_LedSetZone(zoneTypes[dt], zone, 0, 100, 0);
				if (ok)
				{
					okCount++;
					maxZone = zone;
				}
				// Immediately turn off
				g_LedSetZone(zoneTypes[dt], zone, 0, 0, 0);
			}

			if (okCount > 0)
			{
				Log("  devType=0x%02X: %d zones OK (max zone=%d)", zoneTypes[dt], okCount, maxZone);
				if (numOkResults < 21)
				{
					okResults[numOkResults].devType = zoneTypes[dt];
					okResults[numOkResults].maxZone = maxZone;
					okResults[numOkResults].okCount = okCount;
					numOkResults++;
				}
			}
		}

		Log("  %d device types returned OK", numOkResults);

		// Pass 2: visual test on each OK device type (light ALL zones, 800ms per type)
		if (numOkResults > 0)
		{
			Log("  Pass 2: visual test (%d types, ~%ds)...", numOkResults, numOkResults);

			for (int r = 0; r < numOkResults; r++)
			{
				int dt = okResults[r].devType;
				int mz = okResults[r].maxZone;

				// Light all zones bright green
				for (int z = 0; z <= mz; z++)
					g_LedSetZone(dt, z, 0, 100, 0);

				Log("  >>> VISUAL B-%d: devType=0x%02X zones 0-%d GREEN - CHECK WHEEL! (800ms)",
					r + 1, dt, mz);
				Sleep(800);

				// Turn off
				for (int z = 0; z <= mz; z++)
					g_LedSetZone(dt, z, 0, 0, 0);
				Sleep(200);
			}
		}

		// Use first OK result as default
		if (numOkResults > 0)
		{
			g_sdkDevType = okResults[0].devType;
			g_sdkMaxZone = okResults[0].maxZone;
			g_sdkUseGlobal = false;
			Log("  Selected: devType=0x%02X maxZone=%d", g_sdkDevType, g_sdkMaxZone);
		}
	}

	// Accept METHOD_SDK if init was OK (user will check visual tests and log)
	m_method = METHOD_SDK;
	m_available = true;
	Log("=== LED CONTROL ACTIVE (LED SDK v13 scan) ===");
	Log("=== Check log above for VISUAL tests that lit the wheel ===");
	return true;
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
	Log("=== LogitechLED Init v15 (fix parsing + HID++ via WS) ===");
	Log("");

	if (m_available) return true;

	// Phase -1: Register with G Hub via WebSocket
	// This tells G Hub about our process, which may enable device detection
	TryGHubWebSocket();

	// Phase 0: Steering Wheel SDK (LogiPlayLeds)
	// After G Hub registration, the engine might now detect the wheel
	Log("");
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

	if (m_method == METHOD_SDK)
	{
		int numLeds = 0;
		for (int i = 0; i < 5; i++)
			if (ledMask & (1 << i)) numLeds++;

		bool anyOk = false;

		if (g_sdkUseGlobal && g_LedSetTarget && g_LedSetLighting)
		{
			// Global mode: single color for all LEDs
			int pct = (numLeds * 100) / 5;
			g_LedSetTarget(g_sdkGlobalTarget);
			anyOk = g_LedSetLighting(0, pct, 0);
		}
		else if (g_LedSetZone)
		{
			// Zone mode: map LED mask to zones
			if (g_sdkMaxZone >= 4)
			{
				// 5+ zones: one zone per LED
				for (int z = 0; z <= g_sdkMaxZone && z < 5; z++)
				{
					int green = (ledMask & (1 << z)) ? 100 : 0;
					bool ok = g_LedSetZone(g_sdkDevType, z, 0, green, 0);
					if (ok) anyOk = true;
				}
			}
			else
			{
				// 2 zones: zone 0 = green intensity, zone 1 = red intensity
				int greenPct = 0, redPct = 0;
				if (numLeds >= 1) greenPct = 33;
				if (numLeds >= 2) greenPct = 66;
				if (numLeds >= 3) greenPct = 100;
				if (numLeds >= 4) redPct = 50;
				if (numLeds >= 5) redPct = 100;

				bool z0 = g_LedSetZone(g_sdkDevType, 0, 0, greenPct, 0);
				bool z1 = g_LedSetZone(g_sdkDevType, 1, redPct, 0, 0);
				anyOk = z0 || z1;
			}
		}

		g_setLedsCallCount++;
		if (g_setLedsCallCount <= 20 || !anyOk)
			Log("SetLEDs(0x%02X) [SDK dt=0x%02X mz=%d] -> %s (#%d)",
				ledMask, g_sdkDevType, g_sdkMaxZone,
				anyOk ? "OK" : "FAIL", g_setLedsCallCount);

		return anyOk;
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
