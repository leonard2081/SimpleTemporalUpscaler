$sourceRepo = "C:\Work\Repo\GUSD\UE_553"
$targetRepo = "C:\Work\Repo\MyGithub\SimpleTemporalUpscaler"
$sourcePrefix = "Engine/Plugins/Runtime/SimpleTemporalUpscaler/"
$targetPrefix = "SimpleTemporalUpscaler"

$ErrorActionPreference = "Stop"

$commits = @(
    "6bf6290fb37d22415f129a4add4b9218cf359584",
    "4ca715daa57475cb42d7820f09b99226b6cb45f5",
    "b9585735b7e78b60e119623a25506366db270ec0",
    "04cc64c3a10e37fd3cbf3fa811c2d66b73e67747",
    "c83ba696e31e101c13b6a0eb36817a2f2563cb14",
    "2d04164208812cc271e18cb3da035b2a71cf14c4",
    "eafa3a9a5c6c7660ca14d8711523a954262f95d9",
    "ef85baad5ae986649f5a46b688196ca379eea690",
    "a3b28ad67679359f2bf7f015ae7c9de79e2dd1e1",
    "d0b27e5109fc968b4b19ca5f78ee9caac5e34c98",
    "f32ee5c77a42a6c5f834eb972f5592f7e515491f",
    "57e2098dac424deb4e0ba863446c72c3f59d9c31",
    "0f1bce99f91db78a8a6b27b87d12dd4975370fda"
)

$commitIndex = 0
foreach ($commit in $commits) {
    $commitIndex++
    Write-Host "========================================"
    Write-Host "[$commitIndex/$($commits.Count)] Processing commit $commit"
    Write-Host "========================================"

    $message = git -C $sourceRepo log --format="%B" -n 1 $commit
    $authorName = git -C $sourceRepo log --format="%an" -n 1 $commit
    $authorEmail = git -C $sourceRepo log --format="%ae" -n 1 $commit
    $authorDate = git -C $sourceRepo log --format="%aD" -n 1 $commit

    $firstLine = ($message -split "`r`n|`n")[0]
    Write-Host "Author: $authorName <$authorEmail>"
    Write-Host "Date: $authorDate"
    Write-Host "Message: $firstLine"

    $diffOutput = git -C $sourceRepo diff-tree --no-commit-id -r --name-status -z $commit

    $hasRelevantChanges = $false

    if ($diffOutput) {
        $parts = $diffOutput.Split("`0", [StringSplitOptions]::RemoveEmptyEntries)
        $i = 0
        while ($i -lt $parts.Count) {
            $status = $parts[$i]
            $i++
            $path = $parts[$i]
            $i++

            $oldPath = $null
            if ($status -match '^R') {
                $oldPath = $path
                $path = $parts[$i]
                $i++
            }

            if ($path -like "$sourcePrefix*") {
                $relativePath = $path.Substring($sourcePrefix.Length).Replace("/", "\")
                if ($relativePath -eq "") { continue }
                $targetPath = [System.IO.Path]::Combine($targetRepo, $targetPrefix, $relativePath)

                if ($oldPath -and $oldPath -like "$sourcePrefix*") {
                    $oldRelative = $oldPath.Substring($sourcePrefix.Length).Replace("/", "\")
                    $oldTarget = [System.IO.Path]::Combine($targetRepo, $targetPrefix, $oldRelative)
                    if (Test-Path $oldTarget -PathType Leaf) {
                        Write-Host "  [R(old)] $oldRelative"
                        Remove-Item $oldTarget -Force
                    }
                }

                if ($status -eq 'D' -or $status -match '^D') {
                    if (Test-Path $targetPath -PathType Leaf) {
                        Write-Host "  [DELETE] $relativePath"
                        Remove-Item $targetPath -Force
                    } else {
                        Write-Host "  [DELETE] $relativePath (not found)"
                    }
                } else {
                    $targetDir = Split-Path $targetPath -Parent
                    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
                    Write-Host "  [$status] $relativePath"
                    cmd /c "git -C `"$sourceRepo`" show `"$commit`:$path`" > `"$targetPath`" 2>nul"
                    if (-not (Test-Path $targetPath)) {
                        Write-Host "  ERROR: File not written: $targetPath"
                    }
                }
                $hasRelevantChanges = $true
            }
        }
    }

    if (-not $hasRelevantChanges) {
        Write-Host "No relevant changes, skipping commit"
        Write-Host ""
        continue
    }

    $env:GIT_AUTHOR_NAME = $authorName
    $env:GIT_AUTHOR_EMAIL = $authorEmail
    $env:GIT_AUTHOR_DATE = $authorDate

    git -C $targetRepo add -A -- ":!copy_commits.ps1"

    $msgFile = [System.IO.Path]::GetTempFileName()
    [System.IO.File]::WriteAllText($msgFile, $message, [System.Text.UTF8Encoding]::new($false))
    git -C $targetRepo commit -F $msgFile 2>&1 | ForEach-Object { "$_" }
    $exitCode = $LASTEXITCODE
    Remove-Item $msgFile -Force -ErrorAction SilentlyContinue

    if ($exitCode -eq 0) {
        Write-Host "COMMIT SUCCESSFUL!"
    } else {
        Write-Host "COMMIT FAILED (exit code: $exitCode)"
    }
    Write-Host ""
}

Write-Host "========================================"
Write-Host "All done!"
Write-Host "========================================"
