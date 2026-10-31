param(
    [string]$Configuration = "Release",
    [string]$Version = "0.10.1",
    [switch]$IncludeDiagnostic
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Workspace = [System.IO.Path]::GetFullPath((Join-Path $Root ".."))
$Build16 = Join-Path $Workspace ".build\STRPluginMessagingAPI-1.6.1170"
$Build17 = Join-Path $Workspace ".build\STRPluginMessagingAPI-1.7.104"
$PackageSuffix = if ($IncludeDiagnostic) { "-test" } else { "" }
$Stage = Join-Path $Workspace ".package\STRPluginMessagingAPI-v$Version$PackageSuffix"
$Dist = Join-Path $Root "dist"
$Zip = Join-Path $Dist "STRPluginMessagingAPI-v$Version$PackageSuffix.zip"
$Package = Join-Path $Root "package"
$RelaySource = Join-Path $Root "extras\str-server-resources\strpm-chat-relay"
$Ninja = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$VsDevCmd = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"

$VcpkgRoot = if ($env:VCPKG_ROOT) {
    [System.IO.Path]::GetFullPath($env:VCPKG_ROOT)
} elseif (Test-Path -LiteralPath "C:\dev\vcpkg" -PathType Container) {
    "C:\dev\vcpkg"
} else {
    $null
}
$VcpkgToolchain = if ($VcpkgRoot) {
    Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
} else {
    $null
}

foreach ($Path in @($Build16, $Build17, $Stage, $Zip)) {
    $FullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (
            $FullPath.Equals($Workspace, [System.StringComparison]::OrdinalIgnoreCase) -or
            $FullPath.StartsWith("$Workspace\", [System.StringComparison]::OrdinalIgnoreCase))) {
        throw "Chemin de sortie hors workspace: $FullPath"
    }
}

if (-not (Test-Path -LiteralPath $VsDevCmd -PathType Leaf)) {
    throw "VsDevCmd introuvable: $VsDevCmd"
}
if (-not (Test-Path -LiteralPath $Ninja -PathType Leaf)) {
    throw "Ninja introuvable: $Ninja"
}
if (-not $VcpkgToolchain -or -not (Test-Path -LiteralPath $VcpkgToolchain -PathType Leaf)) {
    throw "vcpkg introuvable. Definis VCPKG_ROOT ou installe-le dans C:\dev\vcpkg."
}
if (-not (Test-Path -LiteralPath (Join-Path $RelaySource "main.lua") -PathType Leaf)) {
    throw "Relay STRPM introuvable: $RelaySource"
}
if (-not (Test-Path -LiteralPath (Join-Path $RelaySource "strpm-chat-relay.manifest") -PathType Leaf)) {
    throw "Manifest du relay STRPM introuvable: $RelaySource"
}

function Invoke-STRPMBuild {
    param(
        [Parameter(Mandatory=$true)][string]$Runtime,
        [Parameter(Mandatory=$true)][string]$BuildDir
    )

    Write-Host ""
    Write-Host "Building STRPM v$Version for Skyrim $Runtime..." -ForegroundColor Cyan

    $Command = "`"$VsDevCmd`" -arch=x64 && cmake -S `"$Root`" -B `"$BuildDir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$Ninja`" -DCMAKE_TOOLCHAIN_FILE=`"$VcpkgToolchain`" -DVCPKG_TARGET_TRIPLET=x64-windows -DCMAKE_BUILD_TYPE=$Configuration -DSTRPM_ENABLE_UDP_BACKEND=OFF -DSTRPM_BUILD_STR184_BRIDGE_PROBE=ON -DSTRPM_SKYRIM_RUNTIME=$Runtime && cmake --build `"$BuildDir`" --config $Configuration"
    cmd.exe /d /s /c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation CMake echouee pour Skyrim $Runtime avec le code $LASTEXITCODE."
    }

    foreach ($Name in @(
        "STRPluginMessagingAPI.dll",
        "STRPluginMessagingBridge.dll"
    )) {
        $Path = Join-Path $BuildDir $Name
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "DLL introuvable apres compilation Skyrim $Runtime : $Path"
        }
    }

    if ($IncludeDiagnostic) {
        $Diagnostic = Join-Path $BuildDir "STRPluginMessagingDiagnostic.dll"
        if (-not (Test-Path -LiteralPath $Diagnostic -PathType Leaf)) {
            throw "DLL diagnostic introuvable apres compilation Skyrim $Runtime : $Diagnostic"
        }
    }
}

Invoke-STRPMBuild -Runtime "1.6.1170" -BuildDir $Build16
Invoke-STRPMBuild -Runtime "1.7.104" -BuildDir $Build17

foreach ($Path in @($Stage, $Zip)) {
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
New-Item -ItemType Directory -Force -Path $Stage | Out-Null

Copy-Item -LiteralPath (Join-Path $Package "fomod") -Destination (Join-Path $Stage "fomod") -Recurse -Force

$CommonPluginDir = Join-Path $Stage "Common\SKSE\Plugins"
New-Item -ItemType Directory -Force -Path $CommonPluginDir | Out-Null
Copy-Item -LiteralPath (Join-Path $Package "Data\SKSE\Plugins\STRPluginMessagingAPI.ini") -Destination $CommonPluginDir -Force

function Stage-Runtime {
    param(
        [Parameter(Mandatory=$true)][string]$Runtime,
        [Parameter(Mandatory=$true)][string]$BuildDir
    )

    $PluginDir = Join-Path $Stage "Runtime\Skyrim-$Runtime\SKSE\Plugins"
    New-Item -ItemType Directory -Force -Path $PluginDir | Out-Null

    Copy-Item -LiteralPath (Join-Path $BuildDir "STRPluginMessagingAPI.dll") -Destination $PluginDir -Force
    Copy-Item -LiteralPath (Join-Path $BuildDir "STRPluginMessagingBridge.dll") -Destination $PluginDir -Force

    if ($IncludeDiagnostic) {
        Copy-Item -LiteralPath (Join-Path $BuildDir "STRPluginMessagingDiagnostic.dll") -Destination $PluginDir -Force
    }
}

Stage-Runtime -Runtime "1.6.1170" -BuildDir $Build16
Stage-Runtime -Runtime "1.7.104" -BuildDir $Build17

$ServerResourcesRoot = Join-Path $Stage "Server\SkyrimTogetherReborn\resources"
New-Item -ItemType Directory -Force -Path $ServerResourcesRoot | Out-Null
Copy-Item -LiteralPath $RelaySource -Destination $ServerResourcesRoot -Recurse -Force

$FomodInfo = Join-Path $Stage "fomod\info.xml"
$FomodConfig = Join-Path $Stage "fomod\ModuleConfig.xml"
foreach ($XmlFile in @($FomodInfo, $FomodConfig)) {
    if (-not (Test-Path -LiteralPath $XmlFile -PathType Leaf)) {
        throw "Fichier FOMOD absent: $XmlFile"
    }
    try {
        [xml](Get-Content -LiteralPath $XmlFile -Raw) | Out-Null
    } catch {
        throw "XML FOMOD invalide: $XmlFile`n$($_.Exception.Message)"
    }
}

Compress-Archive `
    -Path (Join-Path $Stage "*") `
    -DestinationPath $Zip `
    -CompressionLevel Optimal `
    -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$Archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
try {
    $Entries = @($Archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })

    $RequiredEntries = @(
        "fomod/info.xml",
        "fomod/ModuleConfig.xml",
        "Common/SKSE/Plugins/STRPluginMessagingAPI.ini",
        "Runtime/Skyrim-1.6.1170/SKSE/Plugins/STRPluginMessagingAPI.dll",
        "Runtime/Skyrim-1.6.1170/SKSE/Plugins/STRPluginMessagingBridge.dll",
        "Runtime/Skyrim-1.7.104/SKSE/Plugins/STRPluginMessagingAPI.dll",
        "Runtime/Skyrim-1.7.104/SKSE/Plugins/STRPluginMessagingBridge.dll",
        "Server/SkyrimTogetherReborn/resources/strpm-chat-relay/main.lua",
        "Server/SkyrimTogetherReborn/resources/strpm-chat-relay/strpm-chat-relay.manifest"
    )

    foreach ($RequiredEntry in $RequiredEntries) {
        if ($Entries -notcontains $RequiredEntry) {
            throw "Entree absente de l'archive: $RequiredEntry"
        }
    }

    foreach ($Runtime in @("1.6.1170", "1.7.104")) {
        $DiagnosticEntry = "Runtime/Skyrim-$Runtime/SKSE/Plugins/STRPluginMessagingDiagnostic.dll"
        if ($IncludeDiagnostic -and $Entries -notcontains $DiagnosticEntry) {
            throw "Le client diagnostic Skyrim $Runtime est absent du package de test v$Version."
        }
        if (-not $IncludeDiagnostic -and $Entries -contains $DiagnosticEntry) {
            throw "Le client diagnostic Skyrim $Runtime ne doit pas etre inclus dans le package normal v$Version."
        }
    }
} finally {
    $Archive.Dispose()
}

Write-Host ""
Write-Host "OK - package FOMOD multi-runtime cree :" -ForegroundColor Green
Write-Host $Zip
Write-Host "Runtimes: Skyrim 1.6.1170 + Skyrim 1.7.104"
Write-Host "FOMOD: Installation Type -> Skyrim Runtime"
Write-Host "Server destination: Data\SkyrimTogetherReborn\resources\strpm-chat-relay"
