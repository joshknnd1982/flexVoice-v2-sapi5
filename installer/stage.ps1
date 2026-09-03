# stage.ps1 -- assemble output\ for the Inno Setup script.
#
# Keeping the layout here rather than in the .iss keeps the installer script
# short and makes the shipped tree inspectable before packaging.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root "output"

# A host holding the engine files open would block the copy.
$hostExe = Join-Path $root "build_x86\bin\Release\flexvoice_host.exe"
if (Test-Path $hostExe) { & $hostExe --shutdown 2>&1 | Out-Null }
Get-Process flexvoice_host -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null
New-Item -ItemType Directory -Path (Join-Path $out "x64") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $out "engine") | Out-Null

function Need($path) {
    if (-not (Test-Path $path)) { throw "missing build output: $path" }
    return $path
}

$x86 = Join-Path $root "build_x86\bin\Release"
$x64 = Join-Path $root "build_x64\bin\Release"

Copy-Item (Need "$x86\FlexVoiceSAPI.dll")    $out
Copy-Item (Need "$x86\flexvoice_host.exe")   $out
Copy-Item (Need "$x86\FlexVoiceConfig.exe")  $out
Copy-Item (Need "$x64\FlexVoiceSAPI.dll")    (Join-Path $out "x64")

# The MindMaker engine and its English language data.
Copy-Item (Need (Join-Path $root "bin\mmfvapi\dll\FlexVoice_3_01_001.dll")) $out
Copy-Item (Join-Path $root "bin\fv\English") (Join-Path $out "engine") -Recurse

# The demo text the engine shipped with is not needed at runtime.
$demo = Join-Path $out "engine\English\DemoText"
if (Test-Path $demo) { Remove-Item $demo -Recurse -Force }

# Diagnostics the user can run if speech ever stops working.
Copy-Item (Need "$x86\client_test.exe") (Join-Path $out "flexvoice_diag32.exe")
Copy-Item (Need "$x64\client_test.exe") (Join-Path $out "flexvoice_diag64.exe")

Copy-Item (Join-Path $root "README.md")  $out
Copy-Item (Join-Path $root "LICENSE")    $out
Copy-Item (Join-Path $root "CREDITS.md") $out

$files = Get-ChildItem $out -Recurse -File
$mb = [math]::Round(($files | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Output "staged $($files.Count) files, $mb MB, in $out"
