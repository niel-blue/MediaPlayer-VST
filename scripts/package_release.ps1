[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = 'dist'
)

$ErrorActionPreference = 'Stop'

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildInput = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory
} else {
    Join-Path $projectRoot $BuildDirectory
}
$outputInput = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory
} else {
    Join-Path $projectRoot $OutputDirectory
}
$buildPath = [System.IO.Path]::GetFullPath($buildInput)
$outputPath = [System.IO.Path]::GetFullPath($outputInput)
$bundlePath = Join-Path $buildPath 'VST3\Release\MediaPlayer VST.vst3'
$requiredBundleFiles = @(
    'Contents\x86_64-win\MediaPlayer VST.vst3'
    'Contents\x86_64-win\avcodec-63.dll'
    'Contents\x86_64-win\avformat-63.dll'
    'Contents\x86_64-win\avutil-61.dll'
    'Contents\x86_64-win\swresample-7.dll'
    'Contents\x86_64-win\swscale-10.dll'
    'Contents\Resources\DAW-Media-Player-LICENSE.txt'
    'Contents\Resources\FFmpeg-LICENSE.txt'
    'Contents\Resources\FFmpeg-BUILD-INFO.txt'
    'Contents\Resources\THIRD_PARTY_NOTICES.md'
    'Contents\Resources\VST3-SDK-LICENSE.txt'
)

if (-not (Test-Path -LiteralPath $bundlePath -PathType Container)) {
    throw "VST3 bundle not found: $bundlePath"
}
foreach ($relativePath in $requiredBundleFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $bundlePath $relativePath) -PathType Leaf)) {
        throw "VST3 bundle is missing required release file: $relativePath"
    }
}

$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(MediaPlayerVST3\s+VERSION\s+(?<version>\d+\.\d+\.\d+)') {
    throw 'Could not determine the release version from CMakeLists.txt.'
}
$version = $Matches.version

New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$archivePath = Join-Path $outputPath "MediaPlayer-VST-v$version-win-x64.zip"
$stagingPath = Join-Path $outputPath ('.staging-' + [guid]::NewGuid().ToString('N'))

try {
    New-Item -ItemType Directory -Path $stagingPath | Out-Null
    Copy-Item -LiteralPath $bundlePath -Destination $stagingPath -Recurse
    Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $stagingPath
    Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $stagingPath
    Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $stagingPath
    Compress-Archive -Path (Join-Path $stagingPath '*') -DestinationPath $archivePath -CompressionLevel Optimal -Force
    Write-Host "Created $archivePath"
}
finally {
    if (Test-Path -LiteralPath $stagingPath -PathType Container) {
        Remove-Item -LiteralPath $stagingPath -Recurse -Force
    }
}
