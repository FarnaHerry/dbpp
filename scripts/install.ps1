<#
.SYNOPSIS
  Install, upgrade, or uninstall db++ on Windows (x86_64).
.DESCRIPTION
  Supports both direct execution (install.ps1 -Version 0.4.7) and
  piped execution (irm .../install.ps1 | iex) configured via DBPP_* env vars.
#>
[CmdletBinding()]
param(
  [string]$Version = '',
  [string]$InstallDir = '',
  [string]$DownloadBase = '',
  [string]$Repo = '',
  [switch]$UseInstaller,
  [switch]$Force,
  [switch]$NoModifyPath,
  [switch]$NoShortcut,
  [switch]$NoUninstallEntry,
  [switch]$RequireChecksum,
  [switch]$SkipChecksum,
  [switch]$Uninstall
)

function Test-EnvFlag([string]$Name) {
  $val = [Environment]::GetEnvironmentVariable($Name)
  if ([string]::IsNullOrWhiteSpace($val)) { return $false }
  return $val.Trim() -match '^(1|true|yes|on)$'
}

function Write-DbppLog([string]$Message) { Write-Host "[dbpp] $Message" }
function Write-DbppWarn([string]$Message) { Write-Warning "[dbpp] $Message" }

function Get-WindowsX64Check {
  $effective = if (-not [string]::IsNullOrWhiteSpace($env:PROCESSOR_ARCHITEW6432)) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
  if ($effective -ne 'AMD64') {
    throw "Unsupported Windows architecture '$effective'. Prebuilt Windows binaries currently target x86_64 (AMD64)."
  }
}

function Resolve-LatestDbppVersion([string]$RepoName) {
  $latestUrl = "https://github.com/$RepoName/releases/latest"
  try {
    $req = [System.Net.HttpWebRequest]::Create($latestUrl)
    $req.Method = 'HEAD'
    $req.AllowAutoRedirect = $false
    $req.UserAgent = 'dbpp-installer'
    $resp = $req.GetResponse()
    try { $loc = $resp.Headers['Location'] } finally { $resp.Close() }
    if ($loc -match '/releases/tag/v?([^/?#]+)') { return $Matches[1] }
  } catch [System.Net.WebException] {
    if ($_.Exception.Response) {
      try {
        $loc = $_.Exception.Response.Headers['Location']
        if ($loc -match '/releases/tag/v?([^/?#]+)') { return $Matches[1] }
      } finally { $_.Exception.Response.Close() }
    }
  } catch { }

  try {
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$RepoName/releases/latest" -Headers @{ 'User-Agent' = 'dbpp-installer' }
    if ($release -and $release.tag_name) { return ([string]$release.tag_name).TrimStart('v') }
  } catch { }
  throw "Failed to resolve latest release version for $RepoName. Set -Version or `$env:DBPP_VERSION."
}

function Copy-OrDownloadFile([string]$SourceUrl, [string]$DestPath) {
  if ($SourceUrl -match '^file://') {
    $localPath = ([System.Uri]$SourceUrl).LocalPath
    if (-not (Test-Path -LiteralPath $localPath -PathType Leaf)) { throw "Local file not found: $localPath" }
    Copy-Item -LiteralPath $localPath -Destination $DestPath -Force
    return
  }
  if (Test-Path -LiteralPath $SourceUrl -PathType Leaf) {
    Copy-Item -LiteralPath $SourceUrl -Destination $DestPath -Force
    return
  }
  Invoke-WebRequest -Uri $SourceUrl -OutFile $DestPath -UseBasicParsing
}

function Verify-DbppChecksum([string]$AssetPath, [string]$AssetName, [string]$SumsUrl, [string]$TempDir, [bool]$RequireSum, [bool]$SkipSum) {
  if ($SkipSum) { Write-DbppWarn 'Skipping SHA-256 verification (-SkipChecksum).'; return }
  $sumsFile = Join-Path $TempDir 'SHA256SUMS.txt'
  try {
    Copy-OrDownloadFile $SumsUrl $sumsFile
  } catch {
    if ($RequireSum) { throw "SHA256SUMS.txt could not be downloaded from $SumsUrl" }
    Write-DbppWarn 'SHA256SUMS.txt not found for this release; skipping checksum verification.'
    return
  }
  $expected = $null
  foreach ($line in (Get-Content -LiteralPath $sumsFile)) {
    if ($line -match '^\s*([A-Fa-f0-9]{64})\s+\*?(\./)?(.+?)\s*$' -and $Matches[3] -eq $AssetName) {
      $expected = $Matches[1].ToLowerInvariant()
      break
    }
  }
  if (-not $expected) { throw "Asset $AssetName is missing from SHA256SUMS.txt." }
  $actual = (Get-FileHash -LiteralPath $AssetPath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $expected) { throw "SHA-256 mismatch for ${AssetName}: expected $expected, got $actual" }
  Write-DbppLog "Verified SHA-256 checksum for $AssetName."
}

function Assert-InstallDirSafe([string]$TargetDir) {
  if ([string]::IsNullOrWhiteSpace($TargetDir)) { throw 'InstallDir cannot be empty.' }
  $resolved = [System.IO.Path]::GetFullPath($TargetDir).TrimEnd('\')
  if ((Test-Path -LiteralPath (Join-Path $resolved 'data\dbpp.sqlite3')) -or (Test-Path -LiteralPath (Join-Path $resolved 'dbpp.sqlite3'))) {
    throw "Refusing InstallDir '$resolved': contains dbpp.sqlite3 user data."
  }
  return $resolved
}

function Stop-RunningDbpp([bool]$AllowForce) {
  $procs = @(Get-Process -Name 'dbpp' -ErrorAction SilentlyContinue)
  if ($procs.Count -eq 0) { return }
  if (-not $AllowForce) {
    throw 'db++ is currently running (check the system tray). Exit db++ first or re-run with -Force ($env:DBPP_FORCE=1).'
  }
  Write-DbppLog 'Stopping running db++ processes...'
  foreach ($p in $procs) {
    Stop-Process -Id $p.Id -Force -ErrorAction Stop
    $null = $p.WaitForExit(5000)
  }
}

function Broadcast-EnvironmentChange {
  try {
    if (-not ('DbppNative.Win32' -as [type])) {
      Add-Type -Namespace DbppNative -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Auto)]
public static extern System.IntPtr SendMessageTimeout(
  System.IntPtr hWnd, uint Msg, System.UIntPtr wParam, string lParam,
  uint fuFlags, uint uTimeout, out System.UIntPtr lpdwResult);
'@
    }
    $result = [System.UIntPtr]::Zero
    $null = [DbppNative.Win32]::SendMessageTimeout([System.IntPtr]0xffff, 0x001A, [System.UIntPtr]::Zero, 'Environment', 0x0002, 1000, [ref]$result)
  } catch { }
}

function Update-UserPath([string]$Dir, [bool]$Add) {
  $norm = $Dir.TrimEnd('\')
  $raw = [Environment]::GetEnvironmentVariable('Path', 'User')
  $entries = if ([string]::IsNullOrEmpty($raw)) { @() } else { @($raw -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) }
  $filtered = @($entries | Where-Object { $_.TrimEnd('\').ToLowerInvariant() -ne $norm.ToLowerInvariant() })

  if ($Add) {
    if ($filtered.Count -eq $entries.Count) {
      [Environment]::SetEnvironmentVariable('Path', (@($entries + $norm) -join ';'), 'User')
      Broadcast-EnvironmentChange
      Write-DbppLog "Added $norm to User PATH."
    }
    $sessionEntries = @($env:Path -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if (@($sessionEntries | Where-Object { $_.TrimEnd('\').ToLowerInvariant() -eq $norm.ToLowerInvariant() }).Count -eq 0) {
      $env:Path = ($sessionEntries + $norm) -join ';'
    }
  } elseif ($filtered.Count -ne $entries.Count) {
    [Environment]::SetEnvironmentVariable('Path', ($filtered -join ';'), 'User')
    Broadcast-EnvironmentChange
    Write-DbppLog "Removed $norm from User PATH."
  }
}

function Get-ShortcutFolder { return (Join-Path ([Environment]::GetFolderPath('Programs')) 'db++') }

function Install-StartMenuShortcut([string]$InstallDir) {
  $folder = Get-ShortcutFolder
  New-Item -ItemType Directory -Path $folder -Force | Out-Null
  $lnkPath = Join-Path $folder 'db++.lnk'
  $exePath = Join-Path $InstallDir 'dbpp.exe'
  $shell = New-Object -ComObject WScript.Shell
  try {
    $shortcut = $shell.CreateShortcut($lnkPath)
    $shortcut.TargetPath = $exePath
    $shortcut.WorkingDirectory = $InstallDir
    $shortcut.IconLocation = "$exePath,0"
    $shortcut.Description = 'db++ ADBC Data Manager'
    $shortcut.Save()
  } finally { [void][System.Runtime.InteropServices.Marshal]::ReleaseComObject($shell) }
  Write-DbppLog "Created Start Menu shortcut: $lnkPath"
}

function Remove-StartMenuShortcut {
  $folder = Get-ShortcutFolder
  if (Test-Path -LiteralPath $folder) {
    Remove-Item -LiteralPath $folder -Recurse -Force
    Write-DbppLog "Removed Start Menu shortcut folder: $folder"
  }
}

function Write-UninstallScript([string]$InstallDir) {
  $scriptText = @'
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$installDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$procs = @(Get-Process -Name 'dbpp' -ErrorAction SilentlyContinue)
if ($procs.Count -gt 0) {
  if (-not $Force) { throw 'db++ is currently running. Close it from the system tray first.' }
  $procs | Stop-Process -Force
}
$norm = $installDir.TrimEnd('\').ToLowerInvariant()
$raw = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($raw) {
  $kept = @($raw -split ';' | Where-Object { $_ -and $_.TrimEnd('\').ToLowerInvariant() -ne $norm })
  [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
}
$shortcutDir = Join-Path ([Environment]::GetFolderPath('Programs')) 'db++'
if (Test-Path -LiteralPath $shortcutDir) { Remove-Item -LiteralPath $shortcutDir -Recurse -Force }
$regKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\dbpp'
if (Test-Path -LiteralPath $regKey) { Remove-Item -LiteralPath $regKey -Force }
Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', "ping 127.0.0.1 -n 2 > nul & rmdir /s /q `"$installDir`"" -WindowStyle Hidden
'@
  Set-Content -LiteralPath (Join-Path $InstallDir 'uninstall.ps1') -Value $scriptText -Encoding UTF8
}

function Register-UninstallEntry([string]$InstallDir, [string]$Ver) {
  $regPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\dbpp'
  New-Item -Path $regPath -Force | Out-Null
  $exePath = Join-Path $InstallDir 'dbpp.exe'
  $uninstallScript = Join-Path $InstallDir 'uninstall.ps1'
  $sizeBytes = (Get-ChildItem -LiteralPath $InstallDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
  Set-ItemProperty -Path $regPath -Name 'DisplayName' -Value 'db++'
  Set-ItemProperty -Path $regPath -Name 'DisplayVersion' -Value $Ver
  Set-ItemProperty -Path $regPath -Name 'Publisher' -Value 'FarnaHerry'
  Set-ItemProperty -Path $regPath -Name 'InstallLocation' -Value $InstallDir
  Set-ItemProperty -Path $regPath -Name 'DisplayIcon' -Value "$exePath,0"
  Set-ItemProperty -Path $regPath -Name 'UninstallString' -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$uninstallScript`""
  Set-ItemProperty -Path $regPath -Name 'NoModify' -Value 1 -Type DWord
  Set-ItemProperty -Path $regPath -Name 'NoRepair' -Value 1 -Type DWord
  Set-ItemProperty -Path $regPath -Name 'EstimatedSize' -Value ([int][Math]::Ceiling($sizeBytes / 1KB)) -Type DWord
}

function Remove-UninstallEntry {
  $regPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\dbpp'
  if (Test-Path -LiteralPath $regPath) { Remove-Item -LiteralPath $regPath -Force; Write-DbppLog 'Removed Windows Uninstall registry entry.' }
}

function Swap-DirectoryAtomically([string]$SourceDir, [string]$TargetDir) {
  $parentDir = Split-Path -Parent $TargetDir
  $leafName = Split-Path -Leaf $TargetDir
  New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
  $stamp = [System.Diagnostics.Process]::GetCurrentProcess().Id
  $newDir = Join-Path $parentDir ".$leafName.new.$stamp"
  $oldDir = Join-Path $parentDir ".$leafName.old.$stamp"
  if (Test-Path -LiteralPath $newDir) { Remove-Item -LiteralPath $newDir -Recurse -Force }
  if (Test-Path -LiteralPath $oldDir) { Remove-Item -LiteralPath $oldDir -Recurse -Force }
  Copy-Item -LiteralPath $SourceDir -Destination $newDir -Recurse -Force
  $hadExisting = Test-Path -LiteralPath $TargetDir
  if ($hadExisting) { Move-Item -LiteralPath $TargetDir -Destination $oldDir -Force }
  try {
    Move-Item -LiteralPath $newDir -Destination $TargetDir -Force
  } catch {
    if (Test-Path -LiteralPath $newDir) { Remove-Item -LiteralPath $newDir -Recurse -Force -ErrorAction SilentlyContinue }
    if ($hadExisting -and (Test-Path -LiteralPath $oldDir)) { Move-Item -LiteralPath $oldDir -Destination $TargetDir -Force -ErrorAction SilentlyContinue }
    throw
  }
  if ($hadExisting -and (Test-Path -LiteralPath $oldDir)) { Remove-Item -LiteralPath $oldDir -Recurse -Force -ErrorAction SilentlyContinue }
}

function Uninstall-Dbpp([string]$TargetDir, [bool]$AllowForce) {
  $safeDir = Assert-InstallDirSafe $TargetDir
  Stop-RunningDbpp $AllowForce
  Update-UserPath $safeDir $false
  Remove-StartMenuShortcut
  Remove-UninstallEntry
  if (Test-Path -LiteralPath $safeDir) { Remove-Item -LiteralPath $safeDir -Recurse -Force; Write-DbppLog "Removed $safeDir" }
  $dataDir = Join-Path $env:LOCALAPPDATA 'dbpp\data'
  Write-DbppLog "Uninstalled db++. User data in '$dataDir' and ADBC drivers were preserved."
}

function Main {
  $ErrorActionPreference = 'Stop'
  $ProgressPreference = 'SilentlyContinue'
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  Get-WindowsX64Check

  $effVersion = if (-not [string]::IsNullOrWhiteSpace($Version)) { $Version } else { [string]$env:DBPP_VERSION }
  $effInstallDir = if (-not [string]::IsNullOrWhiteSpace($InstallDir)) { $InstallDir } elseif (-not [string]::IsNullOrWhiteSpace($env:DBPP_INSTALL_DIR)) { $env:DBPP_INSTALL_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\dbpp' }
  $effDownloadBase = if (-not [string]::IsNullOrWhiteSpace($DownloadBase)) { $DownloadBase } else { [string]$env:DBPP_DOWNLOAD_BASE }
  $effRepo = if (-not [string]::IsNullOrWhiteSpace($Repo)) { $Repo } elseif (-not [string]::IsNullOrWhiteSpace($env:DBPP_REPO)) { $env:DBPP_REPO } else { 'FarnaHerry/dbpp' }

  $effUseInstaller = $UseInstaller.IsPresent -or (Test-EnvFlag 'DBPP_USE_INSTALLER')
  $effForce = $Force.IsPresent -or (Test-EnvFlag 'DBPP_FORCE')
  $effNoModifyPath = $NoModifyPath.IsPresent -or (Test-EnvFlag 'DBPP_NO_MODIFY_PATH')
  $effNoShortcut = $NoShortcut.IsPresent -or (Test-EnvFlag 'DBPP_NO_SHORTCUT')
  $effNoUninstallEntry = $NoUninstallEntry.IsPresent -or (Test-EnvFlag 'DBPP_NO_UNINSTALL_ENTRY')
  $effRequireChecksum = $RequireChecksum.IsPresent -or (Test-EnvFlag 'DBPP_REQUIRE_CHECKSUM')
  $effSkipChecksum = $SkipChecksum.IsPresent -or (Test-EnvFlag 'DBPP_SKIP_CHECKSUM')
  $effUninstall = $Uninstall.IsPresent -or (Test-EnvFlag 'DBPP_UNINSTALL')

  if ($effUninstall) { Uninstall-Dbpp $effInstallDir $effForce; return }

  $effVersion = $effVersion.Trim().TrimStart('v')
  if ([string]::IsNullOrWhiteSpace($effVersion)) {
    Write-DbppLog 'Resolving latest db++ release...'
    $effVersion = Resolve-LatestDbppVersion $effRepo
  }

  $assetName = if ($effUseInstaller) { "dbpp-$effVersion-windows-x86_64.exe" } else { "dbpp-$effVersion-windows-x86_64.zip" }
  $effDownloadBase = if ([string]::IsNullOrWhiteSpace($effDownloadBase)) { "https://github.com/$effRepo/releases/download/v$effVersion" } else { $effDownloadBase.TrimEnd('/', '\') }

  $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("dbpp-install-" + [System.Guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
  try {
    $assetPath = Join-Path $tempDir $assetName
    $sep = if ($effDownloadBase -match '^https?://|^file://') { '/' } else { '\' }
    Write-DbppLog "Downloading $assetName..."
    Copy-OrDownloadFile "$effDownloadBase$sep$assetName" $assetPath
    Verify-DbppChecksum $assetPath $assetName "${effDownloadBase}${sep}SHA256SUMS.txt" $tempDir $effRequireChecksum $effSkipChecksum

    if ($effUseInstaller) {
      Write-DbppLog "Launching graphical installer: $assetPath"
      $proc = Start-Process -FilePath $assetPath -PassThru -Wait
      if ($proc.ExitCode -ne 0) { throw "Installer exited with code $($proc.ExitCode)." }
      return
    }

    $safeInstallDir = Assert-InstallDirSafe $effInstallDir
    Stop-RunningDbpp $effForce
    $extractDir = Join-Path $tempDir 'extract'
    Expand-Archive -LiteralPath $assetPath -DestinationPath $extractDir -Force

    $payloadDir = $extractDir
    if (-not (Test-Path -LiteralPath (Join-Path $extractDir 'dbpp.exe') -PathType Leaf)) {
      $nested = Get-ChildItem -LiteralPath $extractDir -Filter 'dbpp.exe' -Recurse -File | Select-Object -First 1
      if (-not $nested) { throw 'Invalid Windows archive: dbpp.exe not found.' }
      $payloadDir = $nested.Directory.FullName
    }

    Swap-DirectoryAtomically $payloadDir $safeInstallDir
    if (-not $effNoModifyPath) { Update-UserPath $safeInstallDir $true }
    if (-not $effNoShortcut) { Install-StartMenuShortcut $safeInstallDir }
    if (-not $effNoUninstallEntry) { Write-UninstallScript $safeInstallDir; Register-UninstallEntry $safeInstallDir $effVersion }

    $versionOut = & (Join-Path $safeInstallDir 'dbpp.exe') --version 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Installed dbpp.exe failed self-check (--version): $versionOut" }
    Write-DbppLog "Installed: $versionOut"
    Write-DbppLog "Location: $safeInstallDir"
  } finally {
    if (Test-Path -LiteralPath $tempDir) { Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue }
  }
}

Main
