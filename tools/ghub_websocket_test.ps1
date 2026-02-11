# G Hub WebSocket Explorer - FFBArcadePlugin
# Connects to lghub_agent.exe WebSocket and explores available commands
# Requires: G Hub running, PowerShell 5.1+

Add-Type -AssemblyName System.Net.WebSockets

$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ws.Options.AddSubProtocol("json")

$uri = [Uri]"ws://localhost:9010"
$cts = New-Object System.Threading.CancellationTokenSource

Write-Host ""
Write-Host "=== G Hub WebSocket Explorer ==="
Write-Host "Connecting to $uri..."
Write-Host ""

try {
    $task = $ws.ConnectAsync($uri, $cts.Token)
    if (-not $task.Wait(5000)) {
        Write-Host "ERROR: Connection timeout (5s)"
        Read-Host "Appuie sur Entree"
        exit 1
    }
    if ($task.IsFaulted) { throw $task.Exception }
    Write-Host "Connected! State: $($ws.State)"
} catch {
    Write-Host "Connection failed: $_"
    Write-Host ""
    Write-Host "G Hub is running? Check if lghub_agent.exe is in Task Manager."
    Read-Host "Appuie sur Entree"
    exit 1
}

function Send-WS($json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $segment = New-Object System.ArraySegment[byte](,$bytes)
    $task = $ws.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $cts.Token)
    $task.Wait(3000) | Out-Null
}

function Recv-WS {
    $buffer = New-Object byte[] 131072
    $result = ""
    $timeoutCts = New-Object System.Threading.CancellationTokenSource
    $timeoutCts.CancelAfter(3000)  # 3s timeout per receive

    try {
        do {
            $segment = New-Object System.ArraySegment[byte](,$buffer)
            $task = $ws.ReceiveAsync($segment, $timeoutCts.Token)
            $task.Wait() | Out-Null
            $recv = $task.Result
            $result += [System.Text.Encoding]::UTF8.GetString($buffer, 0, $recv.Count)
        } while (-not $recv.EndOfMessage)
        return $result
    } catch {
        if ($result.Length -gt 0) { return $result }
        return $null
    }
}

function Test-Endpoint($id, $verb, $path, $payload) {
    Write-Host "--- [$id] $verb $path ---"

    $msg = @{ msgId = "$id"; verb = $verb; path = $path }
    if ($payload) { $msg.payload = $payload }
    $json = $msg | ConvertTo-Json -Compress -Depth 5

    Send-WS $json
    $resp = Recv-WS

    if ($resp) {
        Write-Host $resp
    } else {
        Write-Host "  (no response / timeout)"
    }
    Write-Host ""
}

$id = 1

# ============================================================
# PHASE 1: Device discovery
# ============================================================
Write-Host ""
Write-Host "========================================"
Write-Host "PHASE 1: Device Discovery"
Write-Host "========================================"
Write-Host ""

Test-Endpoint ($id++) "GET" "/devices/list" $null
Test-Endpoint ($id++) "GET" "/devices/state" $null
Test-Endpoint ($id++) "GET" "/api/v1/devices/list" $null

# ============================================================
# PHASE 2: Integration registration
# ============================================================
Write-Host "========================================"
Write-Host "PHASE 2: Integration Registration"
Write-Host "========================================"
Write-Host ""

$regPayload = @{
    integrationIdentifier = "ffb_arcade_plugin"
    name = "FFB Arcade Plugin"
    author = "FFBArcade"
    description = "LED control for racing wheels"
    manualRegistration = $true
}
Test-Endpoint ($id++) "SET" "/api/v1/integration/register" $regPayload

# Try activation with different SDK types
$sdkTypes = @("ACTION", "LED", "LIGHTSYNC", "STEERING", "WHEEL", "LIGHTING", "SCREEN_SAMPLER")
foreach ($st in $sdkTypes) {
    $actPayload = @{
        integrationIdentifier = "ffb_arcade_plugin"
        sdkType = $st
    }
    Test-Endpoint ($id++) "SET" "/api/v1/integration/activate" $actPayload
}

# ============================================================
# PHASE 3: Explore lighting/LED endpoints
# ============================================================
Write-Host "========================================"
Write-Host "PHASE 3: Lighting Endpoints"
Write-Host "========================================"
Write-Host ""

$lightingPaths = @(
    @("GET", "/lighting/state"),
    @("GET", "/lighting/devices"),
    @("GET", "/lighting/effects"),
    @("GET", "/api/v1/lighting/state"),
    @("GET", "/api/v1/lighting/devices"),
    @("GET", "/lightsync/state"),
    @("GET", "/lightsync/devices"),
    @("GET", "/profiles/current"),
    @("GET", "/profiles/list"),
    @("GET", "/api/v1/profiles/current"),
    @("GET", "/api/v1/profiles/list"),
    @("GET", "/features/list"),
    @("GET", "/api/v1/features/list"),
    @("GET", "/steering/state"),
    @("GET", "/wheel/state"),
    @("GET", "/sdk/state"),
    @("GET", "/api/v1/sdk/state")
)

foreach ($ep in $lightingPaths) {
    Test-Endpoint ($id++) $ep[0] $ep[1] $null
}

# ============================================================
# PHASE 4: Try SET commands for LED control
# ============================================================
Write-Host "========================================"
Write-Host "PHASE 4: LED Control Attempts"
Write-Host "========================================"
Write-Host ""

# Try setting lighting on various paths
$ledPayload = @{ red = 0; green = 100; blue = 0 }
Test-Endpoint ($id++) "SET" "/lighting/color" $ledPayload
Test-Endpoint ($id++) "SET" "/api/v1/lighting/color" $ledPayload

$zonePayload = @{ deviceType = "steering_wheel"; zone = 0; red = 0; green = 100; blue = 0 }
Test-Endpoint ($id++) "SET" "/lighting/zone" $zonePayload

$ledPayload2 = @{ leds = 31; device = "steering_wheel" }
Test-Endpoint ($id++) "SET" "/steering/leds" $ledPayload2
Test-Endpoint ($id++) "SET" "/wheel/leds" $ledPayload2

# ============================================================
# Cleanup
# ============================================================
Write-Host "========================================"
Write-Host "Done! Copy-paste ALL output above."
Write-Host "========================================"

try {
    $ws.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done", $cts.Token).Wait(3000) | Out-Null
} catch { }

Read-Host "Appuie sur Entree pour fermer"
