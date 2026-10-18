[CmdletBinding()]
param(
    [string]$OutputDirectory = 'dist',
    [string]$FFmpegSourceArchive = 'dist\ffmpeg-source-c48230eb86.zip',
    [string]$BuildSourceArchive = 'dist\btbn-build-sources-590a661.zip'
)

$ErrorActionPreference = 'Stop'

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Resolve-ProjectPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $projectRoot $Path))
}

$outputPath = Resolve-ProjectPath $OutputDirectory
$ffmpegArchivePath = Resolve-ProjectPath $FFmpegSourceArchive
$buildArchivePath = Resolve-ProjectPath $BuildSourceArchive
$expectedHashes = @{
    $ffmpegArchivePath = '9521eaf5dd3dce9efd7938a560f5cab1c5f0b2b9fef97910f18dd59151bac175'
    $buildArchivePath = '0a6f3f2f4e9b8d4d4eb7040f1d53d3aa43963866b641fd15e7f87831abdde5e8'
}

foreach ($archivePath in @($ffmpegArchivePath, $buildArchivePath)) {
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        throw "Source archive not found: $archivePath"
    }
    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $expectedHashes[$archivePath]) {
        throw "Source archive SHA-256 mismatch: $archivePath"
    }
}

New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(MediaPlayerVST3\s+VERSION\s+(?<version>\d+\.\d+\.\d+)') {
    throw 'Could not determine the release version from CMakeLists.txt.'
}
$version = $Matches.version
$archivePath = Join-Path $outputPath "MediaPlayer-VST-v$version-ffmpeg-sources.zip"
$stagingPath = Join-Path $outputPath ('.ffmpeg-sources-' + [guid]::NewGuid().ToString('N'))

try {
    New-Item -ItemType Directory -Path $stagingPath | Out-Null
    Copy-Item -LiteralPath $ffmpegArchivePath -Destination $stagingPath
    Copy-Item -LiteralPath $buildArchivePath -Destination $stagingPath
    Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\ffmpeg\BUILD-INFO.txt') -Destination $stagingPath
    Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party\ffmpeg\LICENSE.txt') -Destination (Join-Path $stagingPath 'FFmpeg-LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $stagingPath
    $provenance = @'
MediaPlayer VST v0.1.0 FFmpeg source and build information

FFmpeg source revision: c48230eb86ff02246f6a14fa1475a0d9398363b4
FFmpeg source: https://github.com/FFmpeg/FFmpeg/tree/c48230eb86ff02246f6a14fa1475a0d9398363b4
FFmpeg source archive SHA-256: 9521EAF5DD3DCE9EFD7938A560F5CAB1C5F0B2B9FEF97910F18DD59151BAC175

BtbN build-recipe snapshot: 590a6612d7d961e9258429e501619e0b7d7cbedf
BtbN source: https://github.com/BtbN/FFmpeg-Builds/tree/590a6612d7d961e9258429e501619e0b7d7cbedf
BtbN source archive SHA-256: 0A6F3F2F4E9B8D4D4EB7040F1D53D3AA43963866B641FD15E7F87831ABDDE5E8

The pinned binary package identifies BtbN release autobuild-2026-08-14-13-16.
That daily release record is no longer available from the provider. The
included BtbN commit is the closest available build-recipe snapshot. BtbN
identified this same source commit for the adjacent 2026-08-15 build in
https://github.com/BtbN/FFmpeg-Builds/issues/661, but the missing 2026-08-14
release record prevents direct verification that it was the exact commit used
for this binary. BUILD-INFO.txt records the binary's own FFmpeg version and
`ffmpeg -buildconf` output.

The BtbN build scripts record the pinned sources for external libraries. Those
libraries retain their own licenses. The FFmpeg and BtbN repository source
archives are included here; the external library source archives are not.
'@
    Set-Content -LiteralPath (Join-Path $stagingPath 'SOURCE-PROVENANCE.txt') -Value $provenance -Encoding utf8
    Compress-Archive -Path (Join-Path $stagingPath '*') -DestinationPath $archivePath -CompressionLevel Optimal -Force
    Write-Host "Created $archivePath"
}
finally {
    if (Test-Path -LiteralPath $stagingPath -PathType Container) {
        Remove-Item -LiteralPath $stagingPath -Recurse -Force
    }
}
