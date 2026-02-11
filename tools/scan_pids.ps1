# Scan PIDs Logitech dans le DLL steering wheel engine
$dll = "C:\Program Files\Logi\wheel_sdk\9_1_0\logi_steering_wheel_x86.dll"

if (-not (Test-Path $dll)) {
    Write-Host "DLL non trouve: $dll"
    Read-Host "Appuie sur Entree"
    exit 1
}

$bytes = [IO.File]::ReadAllBytes($dll)
$hex = [BitConverter]::ToString($bytes).Replace('-','')

Write-Host ""
Write-Host "DLL: $dll"
Write-Host "Taille: $($bytes.Length) bytes"
Write-Host ""
Write-Host "=== SCAN PIDs ==="

$pids = @(
    ,@("C298", "DFP")
    ,@("C299", "G25")
    ,@("C29A", "DFGT")
    ,@("C29B", "G27")
    ,@("C24F", "G29")
    ,@("C260", "G920")
    ,@("C266", "G923 PS")
    ,@("C26E", "G923 Xbox")
)

foreach ($entry in $pids) {
    $pidHex = $entry[0]
    $name = $entry[1]
    # Little-endian: swap bytes
    $le = $pidHex.Substring(2,2) + $pidHex.Substring(0,2)
    $found = $hex.Contains($le)
    if ($found) { $status = "PRESENT" } else { $status = "ABSENT" }
    if ($pidHex -eq "C26E") { $mark = " <<<" } else { $mark = "" }
    Write-Host ("  0x{0} ({1,-12}): {2}{3}" -f $pidHex, $name, $status, $mark)
}

Write-Host ""
Read-Host "Appuie sur Entree pour fermer"
