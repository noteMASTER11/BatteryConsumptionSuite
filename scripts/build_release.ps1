$ErrorActionPreference = "Stop"

$repoWin = Split-Path -Parent $PSScriptRoot
$drive = $repoWin.Substring(0,1).ToLower()
$rest = $repoWin.Substring(2).Replace('\','/')
$repoWsl = "/mnt/$drive$rest"
$bashScript = "$repoWsl/scripts/build_release.sh"

wsl.exe -d Ubuntu-22.04 -- bash -lc "chmod +x '$bashScript' && '$bashScript'"
