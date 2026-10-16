# Ocarina of Time co-op launcher.
# Asks for a player name and room code once, writes them into shipofharkinian.json
# with Anchor switched on, then starts the game. Run with -Reset to change them.
param([switch]$Reset)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName Microsoft.VisualBasic
Add-Type -AssemblyName System.Windows.Forms

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$exe = Join-Path $root 'soh.exe'
if (-not (Test-Path $exe)) {
    [System.Windows.Forms.MessageBox]::Show("soh.exe was not found next to this launcher.`nUnzip the whole download into one folder and try again.", 'OoT Co-op') | Out-Null
    exit 1
}

$cfgPath = Join-Path $root 'shipofharkinian.json'
$cfg = $null
if (Test-Path $cfgPath) {
    try { $cfg = Get-Content $cfgPath -Raw | ConvertFrom-Json } catch { $cfg = $null }
}
if ($null -eq $cfg) { $cfg = New-Object PSObject }

# Walk or create nested objects, e.g. CVars.gRemote.Anchor
function Get-Node($obj, [string[]]$path) {
    foreach ($key in $path) {
        if ($null -eq $obj.PSObject.Properties[$key] -or -not ($obj.$key -is [PSCustomObject])) {
            $obj | Add-Member -NotePropertyName $key -NotePropertyValue (New-Object PSObject) -Force
        }
        $obj = $obj.$key
    }
    return $obj
}
function Set-Value($obj, [string]$key, $value) {
    $obj | Add-Member -NotePropertyName $key -NotePropertyValue $value -Force
}

$anchor = Get-Node $cfg @('CVars', 'gRemote', 'Anchor')
$name = [string]$anchor.Name
$room = [string]$anchor.RoomId

if ($Reset -or [string]::IsNullOrWhiteSpace($name)) {
    $name = [Microsoft.VisualBasic.Interaction]::InputBox('Your player name (others see it above your Link):', 'OoT Co-op', $name)
    if ([string]::IsNullOrWhiteSpace($name)) { exit 0 }
}
if ($Reset -or [string]::IsNullOrWhiteSpace($room)) {
    if ([string]::IsNullOrWhiteSpace($room)) { $room = 'hyrule-' + (Get-Random -Minimum 1000 -Maximum 9999) }
    $room = [Microsoft.VisualBasic.Interaction]::InputBox("Room code.`n`nEveryone who types the same code plays in the same world. If a friend sent you a code, type theirs. Otherwise send them this one.", 'OoT Co-op', $room)
    if ([string]::IsNullOrWhiteSpace($room)) { exit 0 }
}

Set-Value $anchor 'Name' $name.Trim()
Set-Value $anchor 'RoomId' $room.Trim()
Set-Value $anchor 'Enabled' 1
if ([string]::IsNullOrWhiteSpace([string]$anchor.Host)) { Set-Value $anchor 'Host' 'anchor.hm64.org' }
if ($null -eq $anchor.Port -or [int]$anchor.Port -le 0) { Set-Value $anchor 'Port' 43383 }
if ([string]::IsNullOrWhiteSpace([string]$anchor.TeamId)) { Set-Value $anchor 'TeamId' 'default' }

$json = $cfg | ConvertTo-Json -Depth 32
[System.IO.File]::WriteAllText($cfgPath, $json, (New-Object System.Text.UTF8Encoding($false)))

Start-Process -FilePath $exe -WorkingDirectory $root
