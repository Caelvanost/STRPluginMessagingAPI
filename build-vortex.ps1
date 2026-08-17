param(
    [string]$Configuration = "Release",
    [string]$Version = "0.4.3"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Build = [System.IO.Path]::GetFullPath((Join-Path $Root "..\.build\STRPluginMessagingAPI"))
$Stage = [System.IO.Path]::GetFullPath((Join-Path $Root "..\.package\STRPluginMessagingAPI-v$Version-Vortex"))
$Dist = [System.IO.Path]::GetFullPath((Join-Path $Root "dist"))
$Zip = [System.IO.Path]::GetFullPath((Join-Path $Dist "STRPluginMessagingAPI-v$Version-Vortex.zip"))
$Package = Join-Path $Root "package"
$Ninja = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$VsDevCmd = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"

$AllowedRoot = [System.IO.Path]::GetFullPath((Join-Path $Root "..")).TrimEnd('\')
foreach ($Path in @($Build, $Stage, $Zip)) {
    $FullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (
            $FullPath.Equals($AllowedRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
            $FullPath.StartsWith("$AllowedRoot\", [System.StringComparison]::OrdinalIgnoreCase))) {
        throw "Chemin de sortie hors workspace: $FullPath"
    }
}

if (-not (Test-Path -LiteralPath $VsDevCmd -PathType Leaf)) {
    throw "VsDevCmd introuvable: $VsDevCmd"
}
if (-not (Test-Path -LiteralPath $Ninja -PathType Leaf)) {
    throw "Ninja introuvable: $Ninja"
}

$Command = "`"$VsDevCmd`" -arch=x64 && cmake -S `"$Root`" -B `"$Build`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$Ninja`" -DCMAKE_BUILD_TYPE=$Configuration -DSTRPM_ENABLE_UDP_BACKEND=OFF -DSTRPM_BUILD_STR180_BRIDGE_PROBE=ON && cmake --build `"$Build`" --config $Configuration"
cmd.exe /d /s /c $Command
if ($LASTEXITCODE -ne 0) {
    throw "Compilation CMake echouee avec le code $LASTEXITCODE."
}

$Dll = Join-Path $Build "STRPluginMessagingAPI.dll"
$BridgeDll = Join-Path $Build "STRPluginMessagingBridge.dll"
foreach ($RequiredDll in @($Dll, $BridgeDll)) {
    if (-not (Test-Path -LiteralPath $RequiredDll -PathType Leaf)) {
        throw "DLL introuvable apres compilation: $RequiredDll"
    }
}

foreach ($Path in @($Stage, $Zip)) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

New-Item -ItemType Directory -Force -Path $Dist | Out-Null

Copy-Item -LiteralPath $Package -Destination $Stage -Recurse -Force
$PluginDir = Join-Path $Stage "Data\SKSE\Plugins"
New-Item -ItemType Directory -Force -Path $PluginDir | Out-Null
Copy-Item -LiteralPath $Dll -Destination (Join-Path $PluginDir "STRPluginMessagingAPI.dll") -Force
Copy-Item -LiteralPath $BridgeDll -Destination (Join-Path $PluginDir "STRPluginMessagingBridge.dll") -Force

Compress-Archive `
    -Path (Join-Path $Stage "*") `
    -DestinationPath $Zip `
    -CompressionLevel Optimal `
    -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$Archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
try {
    $Entries = @($Archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    foreach ($RequiredEntry in @(
        "Data/SKSE/Plugins/STRPluginMessagingAPI.dll",
        "Data/SKSE/Plugins/STRPluginMessagingAPI.ini",
        "Data/SKSE/Plugins/STRPluginMessagingBridge.dll"
    )) {
        if ($Entries -notcontains $RequiredEntry) {
            throw "Entree absente de l'archive: $RequiredEntry"
        }
    }
} finally {
    $Archive.Dispose()
}

Write-Host ""
Write-Host "OK - package Vortex cree :" -ForegroundColor Green
Write-Host $Zip
