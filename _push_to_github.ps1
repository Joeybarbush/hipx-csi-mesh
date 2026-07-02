# _push_to_github.ps1
# One-shot push for hipx-csi-mesh -- run from PowerShell at this folder
# Usage: powershell -ExecutionPolicy Bypass -File _push_to_github.ps1
# -----------------------------------------------------------------
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

Write-Host "== gh auth status ==" -ForegroundColor Cyan
gh auth status
if ($LASTEXITCODE -ne 0) { Write-Host "BLOCKED -- gh not authenticated. Run: gh auth login" -ForegroundColor Red; exit 1 }

Write-Host "`n== checking repo doesn't already exist ==" -ForegroundColor Cyan
$exists = gh repo view Joeybarbush/hipx-csi-mesh 2>&1
if ($LASTEXITCODE -eq 0) { Write-Host "BLOCKED -- Joeybarbush/hipx-csi-mesh already exists." -ForegroundColor Red; exit 1 }

Write-Host "`n== git status ==" -ForegroundColor Cyan
git status -s
git log --oneline | Select-Object -First 5

Write-Host "`n== creating + pushing ==" -ForegroundColor Cyan
gh repo create hipx-csi-mesh --public --source=. --remote=origin --push --description "Sovereign WiFi-sensing mesh from commodity ESP32s"
if ($LASTEXITCODE -ne 0) { Write-Host "BLOCKED -- gh repo create failed." -ForegroundColor Red; exit 1 }

Write-Host "`n== verifying ==" -ForegroundColor Green
gh repo view Joeybarbush/hipx-csi-mesh --json url,name,visibility,defaultBranchRef
$sha = git rev-parse --short HEAD
Write-Host "`nPUSHED LIVE -- commit $sha" -ForegroundColor Green
