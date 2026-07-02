$ErrorActionPreference = "Continue"
Write-Host "SHIP IT -- hipx-csi-mesh" -ForegroundColor Cyan
Write-Host "================================"
Set-Location "C:\Users\joey\hipx-csi-mesh"

Write-Host "`n[1/4] Setting remote..." -ForegroundColor Yellow
git remote remove origin 2>$null
git remote add origin "https://github.com/Joeybarbush/hipx-csi-mesh.git"
git remote -v

Write-Host "`n[2/4] Fetching remote state..." -ForegroundColor Yellow
git fetch origin

Write-Host "`n[3/4] Merging remote README..." -ForegroundColor Yellow
git pull origin main --allow-unrelated-histories --no-edit --strategy-option=ours

Write-Host "`n[4/4] Pushing to GitHub..." -ForegroundColor Yellow
git push -u origin main

Write-Host "`nDONE. Verify at: https://github.com/Joeybarbush/hipx-csi-mesh" -ForegroundColor Green
Write-Host "`nPress any key to close..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
