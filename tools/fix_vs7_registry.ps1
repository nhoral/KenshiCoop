Start-Transcript -Path "$PSScriptRoot\fix_vs7_registry.log" -Force | Out-Null
$p = 'HKLM:\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\SxS\VS7'
if (!(Test-Path $p)) { New-Item -Path $p -Force | Out-Null }
New-ItemProperty -Path $p -Name '10.0' -Value 'C:\Program Files (x86)\Microsoft Visual Studio 10.0\' -PropertyType String -Force | Out-Null
Write-Output "=== VS7 key after write ==="
Get-ItemProperty $p | Format-List
Stop-Transcript | Out-Null
