# stage.ps1 -- collect everything the installer packs into output\.
#
# The engine's own files are copied from engine\, minus the two the installer
# builds for itself: RHL2.dat and Julie.bin are generated from CHL2.dat and
# Julie.cod by FVZip.exe at install time, exactly as Mindmaker's setup did.
# Shipping the sources instead of the results saves about four megabytes.

$ErrorActionPreference = 'Stop'

$root   = Split-Path -Parent $PSScriptRoot
$out    = Join-Path $root 'output'
$x86    = Join-Path $root 'build_x86\Release'
$x64    = Join-Path $root 'build_x64\Release'
$engine = Join-Path $root 'engine'

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null
New-Item -ItemType Directory -Path (Join-Path $out 'x64') | Out-Null

function Need($path) {
    if (-not (Test-Path $path)) { throw "missing build output: $path" }
    $path
}

Copy-Item (Need (Join-Path $x86 'FlexVoice2SAPI.dll'))   $out
Copy-Item (Need (Join-Path $x64 'FlexVoice2SAPI.dll'))   (Join-Path $out 'x64')
Copy-Item (Need (Join-Path $x86 'fv2_host.exe'))         $out
Copy-Item (Need (Join-Path $x86 'FlexVoice2Config.exe')) $out

Copy-Item (Need (Join-Path $engine 'FlexVoice_2_00_010.dll')) $out

# The engine data, minus what the installer generates.
$dataSrc = Join-Path $engine 'Data'
$dataDst = Join-Path $out 'engine\Data'
New-Item -ItemType Directory -Path $dataDst -Force | Out-Null
Copy-Item (Join-Path $dataSrc '*') $dataDst -Recurse -Force
foreach ($generated in 'RHL2.dat', 'Julie.bin') {
    $p = Join-Path $dataDst $generated
    if (Test-Path $p) { Remove-Item $p -Force }
}

# FVZip.exe has to travel with the data: it is what builds those two files.
if (-not (Test-Path (Join-Path $dataDst 'FVZip.exe'))) {
    throw "FVZip.exe is missing from $dataDst; the installer cannot build RHL2.dat or Julie.bin"
}

foreach ($doc in 'README.md', 'LICENSE', 'CREDITS.md') {
    $p = Join-Path $root $doc
    if (Test-Path $p) { Copy-Item $p $out }
}

$size = (Get-ChildItem $out -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("staged {0:N1} MB into {1}" -f ($size / 1MB), $out)
