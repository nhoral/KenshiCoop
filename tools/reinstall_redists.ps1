Start-Transcript -Path "$PSScriptRoot\reinstall_redists.log" -Force | Out-Null
Write-Output "=== Installing VCRedist 2010 x64 ==="
winget install --id Microsoft.VCRedist.2010.x64 --silent --disable-interactivity --accept-source-agreements --accept-package-agreements
Write-Output "x64 exit code: $LASTEXITCODE"
Write-Output "=== Installing VCRedist 2010 x86 ==="
winget install --id Microsoft.VCRedist.2010.x86 --silent --disable-interactivity --accept-source-agreements --accept-package-agreements
Write-Output "x86 exit code: $LASTEXITCODE"
Stop-Transcript | Out-Null
