Start-Transcript -Path "$PSScriptRoot\uninstall_redists.log" -Force | Out-Null
Write-Output "=== Uninstalling VCRedist 2010 x64 ==="
winget uninstall --id Microsoft.VCRedist.2010.x64 --silent --disable-interactivity
Write-Output "x64 exit code: $LASTEXITCODE"
Write-Output "=== Uninstalling VCRedist 2010 x86 ==="
winget uninstall --id Microsoft.VCRedist.2010.x86 --silent --disable-interactivity
Write-Output "x86 exit code: $LASTEXITCODE"
Stop-Transcript | Out-Null
