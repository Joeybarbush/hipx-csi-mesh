$ErrorActionPreference = "Continue"
Write-Host "SHIP IT FIX -- hipx-csi-mesh (recovery + push)" -ForegroundColor Cyan
Write-Host "==============================================="
Set-Location "C:\Users\joey\hipx-csi-mesh"

Write-Host "`n[1/7] Removing stale lock files..." -ForegroundColor Yellow
Remove-Item ".git\config.lock" -Force -ErrorAction SilentlyContinue
Remove-Item ".git\HEAD.lock" -Force -ErrorAction SilentlyContinue
Remove-Item ".git\index.lock" -Force -ErrorAction SilentlyContinue
Write-Host "Locks cleared."

Write-Host "`n[2/7] Rewriting corrupted .git/config..." -ForegroundColor Yellow
@'
[core]
	repositoryformatversion = 0
	filemode = false
	bare = false
	logallrefupdates = true
	symlinks = false
	ignorecase = true
[remote "origin"]
	url = https://github.com/Joeybarbush/hipx-csi-mesh.git
	fetch = +refs/heads/*:refs/remotes/origin/*
[branch "main"]
	remote = origin
	merge = refs/heads/main
'@ | Set-Content -Path ".git\config" -Encoding ASCII -NoNewline
Write-Host "Config rewritten."

Write-Host "`n[3/7] Verifying git can read repo..." -ForegroundColor Yellow
git status
git log --oneline -3

Write-Host "`n[4/7] Confirming remote..." -ForegroundColor Yellow
git remote -v

Write-Host "`n[5/7] Fetching remote state..." -ForegroundColor Yellow
git fetch origin

Write-Host "`n[6/7] Merging remote README (prefer local)..." -ForegroundColor Yellow
git pull origin main --allow-unrelated-histories --no-edit --strategy-option=ours

Write-Host "`n[7/7] Pushing to GitHub..." -ForegroundColor Yellow
git push -u origin main

Write-Host "`nDONE. Verify at: https://github.com/Joeybarbush/hipx-csi-mesh" -ForegroundColor Green
Write-Host "`nPress any key to close..."
$null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
