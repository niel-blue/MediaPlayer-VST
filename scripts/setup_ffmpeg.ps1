[CmdletBinding()]
param(
    [string]$Destination = (Join-Path $PSScriptRoot '..\third_party\ffmpeg')
)

$ErrorActionPreference = 'Stop'

$archiveName = 'ffmpeg-N-126134-gc48230eb86-win64-lgpl-shared.zip'
$archiveUrl = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-08-14-13-16/' + $archiveName
$expectedSha256 = '0b7b0b900441dc75e4528edb4bf7224220253a9525f371e7edf1aca3cdd2edbe'
$expandedFolderName = [System.IO.Path]::GetFileNameWithoutExtension($archiveName)
$destinationPath = [System.IO.Path]::GetFullPath($Destination)

$requiredFiles = @(
    'include\libavformat\avformat.h'
    'lib\avformat.lib'
    'lib\avcodec.lib'
    'lib\avutil.lib'
    'lib\swresample.lib'
    'lib\swscale.lib'
    'bin\avformat-63.dll'
    'bin\avcodec-63.dll'
    'bin\avutil-61.dll'
    'bin\swresample-7.dll'
    'bin\swscale-10.dll'
    'bin\ffmpeg.exe'
    'LICENSE.txt'
)

$buildInfoPath = Join-Path $destinationPath 'BUILD-INFO.txt'
$sourceUrl = 'https://github.com/FFmpeg/FFmpeg/tree/c48230eb86'

function Update-FfmpegBuildInfo {
    $ffmpegExecutable = Join-Path $destinationPath 'bin\ffmpeg.exe'
    $versionOutput = (& $ffmpegExecutable -version 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $versionOutput -notmatch 'N-126134-gc48230eb86') {
        throw "FFmpeg executable does not match pinned revision c48230eb86: $ffmpegExecutable"
    }

    $buildConfiguration = (& $ffmpegExecutable -buildconf 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not read FFmpeg build configuration from $ffmpegExecutable"
    }

    $buildInfo = @"
FFmpeg development package used by DAW Media Player

Variant: win64-lgpl-shared
Release: autobuild-2026-08-14-13-16
Asset: $archiveName
Source: $archiveUrl
Source revision: c48230eb86 ($sourceUrl)
SHA256: $expectedSha256

Build configuration (ffmpeg -buildconf):
$buildConfiguration
The package's LICENSE.txt applies to the FFmpeg files. Review FFmpeg's LGPL
requirements before redistributing a plug-in package containing these DLLs.
"@
    Set-Content -LiteralPath $buildInfoPath -Value $buildInfo -Encoding utf8
}

$complete = $true
foreach ($relativePath in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $destinationPath $relativePath) -PathType Leaf)) {
        $complete = $false
        break
    }
}
if ($complete) {
    Update-FfmpegBuildInfo
    Write-Host "FFmpeg is already available at $destinationPath"
    exit 0
}

if (Test-Path -LiteralPath $destinationPath) {
    $existingItems = @(Get-ChildItem -LiteralPath $destinationPath -Force)
    if ($existingItems.Count -gt 0) {
        throw "FFmpeg destination exists but is incomplete: $destinationPath"
    }
}

$temporaryBase = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::GetTempPath())
$temporaryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $temporaryBase ('daw-media-player-ffmpeg-' + [guid]::NewGuid().ToString('N'))))
if (-not $temporaryRoot.StartsWith(
        $temporaryBase,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Temporary path escaped the system temporary directory: $temporaryRoot"
}

$archivePath = Join-Path $temporaryRoot $archiveName
$extractPath = Join-Path $temporaryRoot 'extracted'

try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    Write-Host "Downloading pinned FFmpeg package..."
    Invoke-WebRequest -Uri $archiveUrl -OutFile $archivePath -UseBasicParsing

    $actualSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "FFmpeg archive hash mismatch. Expected $expectedSha256, got $actualSha256"
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath
    $expandedPath = Join-Path $extractPath $expandedFolderName
    if (-not (Test-Path -LiteralPath $expandedPath -PathType Container)) {
        throw "Expected FFmpeg archive root was not found: $expandedFolderName"
    }

    New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
    Copy-Item -Path (Join-Path $expandedPath '*') -Destination $destinationPath -Recurse -Force

    Update-FfmpegBuildInfo

    foreach ($relativePath in $requiredFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $destinationPath $relativePath) -PathType Leaf)) {
            throw "FFmpeg setup did not produce required file: $relativePath"
        }
    }
    Write-Host "FFmpeg installed at $destinationPath"
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
