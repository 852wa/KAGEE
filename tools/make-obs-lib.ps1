# Creates import libraries (obs.lib, w32-pthreads.lib, obs-frontend-api.lib) so the plugin can be
# built without compiling OBS. Uses the export lists in sdk/lib/*.def; with -FromInstalledObs the
# lists are regenerated from an installed OBS first (use when moving to a new OBS version).
param([string]$ObsDir = "C:\Program Files\obs-studio", [switch]$FromInstalledObs)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$out = Join-Path $root "sdk\lib"
New-Item -ItemType Directory -Force $out | Out-Null

foreach ($name in @("obs", "w32-pthreads", "obs-frontend-api")) {
    $def = Join-Path $out "$name.def"
    if ($FromInstalledObs -or -not (Test-Path $def)) {
        $dll = Join-Path $ObsDir "bin\64bit\$name.dll"
        if (-not (Test-Path $dll)) { throw "$def is missing and $dll was not found" }
        $exports = cmd /c "`"$vcvars`" >nul && dumpbin /nologo /exports `"$dll`""
        $names = foreach ($l in $exports) { if ($l -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { $Matches[1] } }
        @("LIBRARY $name", "EXPORTS") + $names | Set-Content -Encoding ascii $def
    }
    cmd /c "`"$vcvars`" >nul && lib /nologo /machine:x64 /def:`"$def`" /out:`"$out\$name.lib`"" | Out-Null
    Write-Host "$name.lib ready"
}
