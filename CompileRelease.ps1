# Build Release only with the locally installed Visual Studio 2022.
param([switch]$Rebuild)

$ErrorActionPreference = 'Stop'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
$buildRoot = 'Z:\AABBCC-build'
New-Item -ItemType Directory -Force "$buildRoot\logs", "$buildRoot\temp" | Out-Null
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$failed = $false
$buildTarget = if ($Rebuild) { '/t:Rebuild' } else { '/t:Build' }
try {
    $env:TEMP = "$buildRoot\temp"
    $env:TMP = $env:TEMP
    foreach ($entry in @(
        @{ Solution = 'vncviewer\vncviewer.sln'; Log = 'viewer-release-x64.log' },
        @{ Solution = 'winvnc\winvnc.sln'; Log = 'server-release-x64.log' }
    )) {
        & $msbuild (Join-Path $PSScriptRoot $entry.Solution) $buildTarget /m:4 /nologo /v:minimal `
            /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 `
            /p:PreBuildEventUseInBuild=false /p:PostBuildEventUseInBuild=false `
            /fl "/flp:logfile=$buildRoot\logs\$($entry.Log);verbosity=normal;encoding=UTF-8"
        if ($LASTEXITCODE -ne 0) { $failed = $true }
    }
} finally {
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
}
if ($failed) { exit 1 }
