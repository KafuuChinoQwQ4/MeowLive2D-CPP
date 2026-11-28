param(
    [Parameter(Mandatory = $true)]
    [string]$Runtime,
    [Parameter(Mandatory = $true)]
    [string]$DataDirectory
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$origin = 'http://127.0.0.1:19601'
$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$desktop = Join-Path $runtimePath 'meowlive-desktop.exe'
$server = Join-Path $runtimePath 'meowlive-server.exe'
if (!(Test-Path -LiteralPath $desktop) -or !(Test-Path -LiteralPath $server)) {
    throw 'Place both native applications and the Qt dependencies in Runtime'
}
if (Test-Path -LiteralPath $DataDirectory) {
    throw 'Choose a new directory for isolated test data'
}
if (Get-NetTCPConnection -LocalPort 19601 -State Listen -ErrorAction SilentlyContinue) {
    throw 'Port 19601 is in use'
}
$dataPath = (New-Item -ItemType Directory -Path $DataDirectory).FullName
$env:MEOWLIVE_DATA_DIR = $dataPath
Remove-Item Env:MEOWLIVE_SERVER_URL -ErrorAction SilentlyContinue
Remove-Item Env:MEOWLIVE_DEVICE_TOKEN -ErrorAction SilentlyContinue
Remove-Item Env:MEOWLIVE_DEVICE_TOKEN_FILE -ErrorAction SilentlyContinue

function Wait-ForHealth([bool]$paired) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $health = Invoke-RestMethod "$origin/api/health" -TimeoutSec 2
            if ($health.service -eq 'meowlive' -and
                $health.protocol_version -eq 3 -and
                (!$paired -or $health.bridge_connected)) {
                return
            }
        }
        catch {
            Start-Sleep -Milliseconds 200
        }
        Start-Sleep -Milliseconds 150
    }
    throw 'Managed health or dual WebSocket readiness timed out'
}

function Close-Desktop($process) {
    $process.Refresh()
    if (!$process.HasExited) {
        if (!$process.CloseMainWindow() -or !$process.WaitForExit(30000)) {
            throw 'Desktop did not exit cleanly'
        }
    }
}

function Wait-ForNoServer {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        $own = Get-ManagedServer
        if (!$own) {
            return
        }
        Start-Sleep -Milliseconds 200
    }
    throw 'Managed native server remained running after desktop exit'
}

function Get-ManagedServer {
    Get-CimInstance Win32_Process -Filter "Name='meowlive-server.exe'" |
        Where-Object {
            $_.ExecutablePath -eq $server -and $_.CommandLine -and
            $_.CommandLine.Replace('/', '\').IndexOf($dataPath, [StringComparison]::OrdinalIgnoreCase) -ge 0
        }
}

function Get-RetryButton($process) {
    $process.Refresh()
    if ($process.HasExited) {
        throw 'Desktop unexpectedly exited before showing a retry button'
    }
    if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
        $element = [Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
        $label = [string]::Concat([char[]](0x91CD, 0x8BD5, 0x542F, 0x52A8, 0x4E3B, 0x670D, 0x52A1))
        $condition = New-Object Windows.Automation.PropertyCondition(
            [Windows.Automation.AutomationElement]::NameProperty, $label)
        $element.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
    }
}

function Wait-ForRetry($process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline) {
        $control = Get-RetryButton $process
        if ($control -and !$control.Current.IsOffscreen) {
            return $control
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'Desktop did not show a retry button'
}

$window = $null
$external = $null
try {
    $window = Start-Process -FilePath $desktop -WorkingDirectory $runtimePath -PassThru
    Wait-ForHealth $true
    $loaded = @($window.Modules | Where-Object { $_.ModuleName -like 'Qt6*.dll' -or $_.ModuleName -eq 'qwindows.dll' })
    foreach ($dependency in 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll', 'Qt6WebSockets.dll', 'qwindows.dll') {
        if (!($loaded | Where-Object {
            $_.ModuleName -eq $dependency -and
            $_.FileName.StartsWith($runtimePath + '\', [StringComparison]::OrdinalIgnoreCase)
        })) {
            throw "Qt did not load $dependency from the installed runtime"
        }
    }
    $probe = Invoke-RestMethod "$origin/api/speech/preview" -Method Post
    if (!$probe) {
        throw 'Trial PCM command returned no result'
    }
    $end = [DateTime]::UtcNow.AddSeconds(10)
    $played = $false
    while ([DateTime]::UtcNow -lt $end) {
        $status = Invoke-RestMethod "$origin/api/status"
        if ($status.speeches | Where-Object { $_.status -eq 'completed' }) {
            $played = $true
            break
        }
        Start-Sleep -Milliseconds 150
    }
    if (!$played) {
        throw 'The execution device did not confirm trial playback'
    }
    Invoke-RestMethod "$origin/api/stop" -Method Post | Out-Null

    $managed = @(Get-ManagedServer)
    if ($managed.Count -ne 1) {
        throw 'The native desktop did not own a server using the isolated data directory'
    }
    Stop-Process -Id $managed[0].ProcessId
    $retry = Wait-ForRetry $window
    $invoke = $retry.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern)
    $invoke.Invoke()
    Wait-ForHealth $true
    $replacement = @(Get-ManagedServer)
    if ($replacement.Count -ne 1 -or $replacement[0].ProcessId -eq $managed[0].ProcessId) {
        throw 'Retry did not start a new managed server'
    }

    Close-Desktop $window
    Wait-ForNoServer
    $window = Start-Process -FilePath $desktop -WorkingDirectory $runtimePath -PassThru
    Wait-ForHealth $true
    Close-Desktop $window
    Wait-ForNoServer

    $externalDirectory = Join-Path $dataPath 'isolated external'
    $external = Start-Process -FilePath $server -WorkingDirectory $runtimePath -WindowStyle Hidden -PassThru `
        -ArgumentList '--data-dir', ('"' + $externalDirectory + '"')
    Wait-ForHealth $false
    $window = Start-Process -FilePath $desktop -WorkingDirectory $runtimePath -PassThru
    Wait-ForHealth $true
    Close-Desktop $window
    $external.Refresh()
    if ($external.HasExited) {
        throw 'Desktop exited an external server'
    }
    'Native managed startup, trial playback, crash recovery, shutdown, restart and external reuse passed.'
}
finally {
    if ($null -ne $window -and !$window.HasExited) {
        $window.CloseMainWindow() | Out-Null
        $window.WaitForExit(30000) | Out-Null
    }
    if ($null -ne $external -and !$external.HasExited) {
        $external.Kill()
        $external.WaitForExit(3000) | Out-Null
    }
}
