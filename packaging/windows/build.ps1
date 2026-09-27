<#
.SYNOPSIS
  Build the self-contained Windows bundle.

.DESCRIPTION
  Produces dist\esp32-media-remote-windows\, a folder that runs on a machine
  with no Python installed: the embeddable CPython from python.org, the host
  dependencies pip-installed beside it, and the .bat files that drive them.

  Why build it instead of committing it: the result is around 70 MB of
  binaries, and pip brings each dependency's own .dist-info -- licence text
  included -- which a hand-copied lib/ folder silently loses. Those licences
  have to ship with the bundle, so letting pip place them is both less work
  and more correct.

  pip is bootstrapped into a throwaway copy of the interpreter under build\,
  never into the one that ships, so the bundle stays free of pip itself.

.PARAMETER PythonVersion
  Which CPython to embed. Must be a version python.org still publishes an
  embeddable amd64 zip for.

.PARAMETER OutDir
  Where to write the bundle. Defaults to dist\esp32-media-remote-windows.

.PARAMETER Zip
  Also pack the result into dist\<name>.zip, ready for a GitHub release.

.EXAMPLE
  .\packaging\windows\build.ps1 -Zip
#>
[CmdletBinding()]
param(
    [string]$PythonVersion = "3.12.8",
    [string]$OutDir,
    [switch]$Zip
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is much faster without it

$here = Split-Path -Parent $PSCommandPath
$repo = (Resolve-Path (Join-Path $here "..\..")).Path
$work = Join-Path $repo "build"
if (-not $OutDir) { $OutDir = Join-Path $repo "dist\esp32-media-remote-windows" }

# python312._pth for 3.12.x, python313._pth for 3.13.x, and so on.
$parts = $PythonVersion.Split(".")
if ($parts.Count -lt 2) { throw "PythonVersion must look like 3.12.8, got '$PythonVersion'" }
$tag = "$($parts[0])$($parts[1])"
$pthName = "python$tag._pth"

Write-Host ""
Write-Host "  Building the Windows bundle" -ForegroundColor Cyan
Write-Host "  CPython $PythonVersion  ->  $OutDir"
Write-Host ""

New-Item -ItemType Directory -Force -Path $work | Out-Null
if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# --- 1. the embeddable interpreter ------------------------------------------
$zipName = "python-$PythonVersion-embed-amd64.zip"
$zipPath = Join-Path $work $zipName
if (-not (Test-Path $zipPath)) {
    $url = "https://www.python.org/ftp/python/$PythonVersion/$zipName"
    Write-Host "==> Downloading $zipName"
    try {
        Invoke-WebRequest -Uri $url -OutFile $zipPath
    } catch {
        throw "Could not download $url -- check that python.org publishes an embeddable build for $PythonVersion. ($_)"
    }
} else {
    Write-Host "==> Using the cached $zipName"
}

$pyDir = Join-Path $OutDir "python"
Write-Host "==> Unpacking the interpreter"
Expand-Archive -Path $zipPath -DestinationPath $pyDir -Force

# The embeddable build disables site and ships a ._pth listing only itself.
# Ours re-enables site and adds the two folders the daemon imports from.
$pthSource = Join-Path $here "python._pth.template"
$pthTarget = Join-Path $pyDir $pthName
(Get-Content $pthSource -Raw).Replace("python312.zip", "python$tag.zip") |
    Set-Content -Path $pthTarget -Encoding ascii -NoNewline
Add-Content -Path $pthTarget -Value "" -Encoding ascii

# --- 2. pip, in a throwaway copy of the same interpreter --------------------
# This copy lives in the local temp folder rather than under the repo, because
# it is the one thing here that has to *execute*. A repo on a network share
# often grants read but not execute -- a Samba mount is the usual case -- and
# Windows then refuses to start python.exe from it with a bare "Access denied".
$pipDir = Join-Path ([System.IO.Path]::GetTempPath()) "esp32-media-remote-pip-host"
if (Test-Path $pipDir) { Remove-Item -Recurse -Force $pipDir }
Expand-Archive -Path $zipPath -DestinationPath $pipDir -Force
# Let this copy see site-packages, so get-pip has somewhere to install to.
Set-Content -Path (Join-Path $pipDir $pthName) -Encoding ascii -Value @(
    "python$tag.zip", ".", "Lib\site-packages", "import site"
)

$getPip = Join-Path $work "get-pip.py"
if (-not (Test-Path $getPip)) {
    Write-Host "==> Downloading get-pip.py"
    Invoke-WebRequest -Uri "https://bootstrap.pypa.io/get-pip.py" -OutFile $getPip
}

$pipPython = Join-Path $pipDir "python.exe"
Write-Host "==> Bootstrapping pip"
& $pipPython $getPip --no-warn-script-location --quiet
if ($LASTEXITCODE -ne 0) { throw "get-pip.py failed with exit code $LASTEXITCODE" }

# --- 3. the dependencies, into the bundle's lib\ ---------------------------
$libDir = Join-Path $OutDir "lib"
Write-Host "==> Installing dependencies into lib\"
& $pipPython -m pip install --quiet --no-warn-script-location `
    --target $libDir -r (Join-Path $here "requirements.txt")
if ($LASTEXITCODE -ne 0) { throw "pip install failed with exit code $LASTEXITCODE" }

# --- 4. the parts we wrote ourselves ---------------------------------------
Write-Host "==> Copying the host code and the .bat files"
$hostDir = Join-Path $OutDir "host"
New-Item -ItemType Directory -Force -Path $hostDir | Out-Null
Copy-Item (Join-Path $repo "host\media_remote_win.py") $hostDir
Copy-Item (Join-Path $repo "host\remote_common.py") $hostDir
Copy-Item (Join-Path $here "*.bat") $OutDir
Copy-Item (Join-Path $here "README.txt") $OutDir
Copy-Item (Join-Path $repo "LICENSE") $OutDir
Copy-Item (Join-Path $repo "THIRD-PARTY-NOTICES.md") $OutDir

# --- 5. what went in -------------------------------------------------------
Write-Host ""
Write-Host "  Bundled versions" -ForegroundColor Cyan
Get-ChildItem $libDir -Directory -Filter "*.dist-info" |
    ForEach-Object { "    " + ($_.Name -replace "\.dist-info$", "") }

$size = (Get-ChildItem $OutDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ""
Write-Host ("  Done: {0}  ({1:N0} MB)" -f $OutDir, ($size / 1MB)) -ForegroundColor Green

# Can the bundle actually start where it was written? On a network share the
# answer is often no, which is worth saying now rather than leaving the user to
# read "Access denied" out of Troubleshoot.bat later. An interpreter that cannot
# start at all raises rather than returning an exit code, hence the try.
$canRun = $true
try {
    & (Join-Path $pyDir "python.exe") --version *> $null
    $canRun = ($LASTEXITCODE -eq 0)
} catch {
    $canRun = $false
}
if (-not $canRun) {
    Write-Host ""
    Write-Host "  Note: the bundled interpreter will not start from here." -ForegroundColor Yellow
    Write-Host "  That is normal on a network share, which grants read but not execute."
    Write-Host "  Copy the folder to a local disk, or run its Install.bat, which does that."
}

if ($Zip) {
    $zipOut = "$OutDir.zip"
    if (Test-Path $zipOut) { Remove-Item -Force $zipOut }
    Write-Host "==> Packing $([System.IO.Path]::GetFileName($zipOut))"
    Compress-Archive -Path "$OutDir\*" -DestinationPath $zipOut
    Write-Host "  Wrote $zipOut" -ForegroundColor Green
}
Write-Host ""
