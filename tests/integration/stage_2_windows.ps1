param(
    [Parameter(Mandatory = $true)]
    [string] $RuntimeDirectory,

    [string] $RustRuntimeDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$cppServer = (Resolve-Path -LiteralPath (Join-Path $runtime 'meowlive-server.exe')).Path
$cppDesktop = (Resolve-Path -LiteralPath (Join-Path $runtime 'meowlive-desktop.exe')).Path
$runDirectory = Join-Path ([IO.Path]::GetTempPath()) ('MeowLive2D stage 2 ' + [Guid]::NewGuid())
$null = New-Item -ItemType Directory -Path $runDirectory

function Stop-TestProcess([Diagnostics.Process] $Process) {
    if ($null -ne $Process -and -not $Process.HasExited) {
        $Process.Kill()
        if (-not $Process.WaitForExit(5000)) {
            throw "Process $($Process.Id) did not exit"
        }
    }
}

function Read-ServerJson([string] $BaseUri, [string] $Path) {
    $request = [Net.HttpWebRequest] [Net.WebRequest]::Create($BaseUri + $Path)
    $request.Proxy = $null
    $request.Timeout = 1000
    $request.ReadWriteTimeout = 1000
    $response = $request.GetResponse()
    try {
        $reader = [IO.StreamReader]::new($response.GetResponseStream())
        try {
            return $reader.ReadToEnd() | ConvertFrom-Json
        } finally {
            $reader.Dispose()
        }
    } finally {
        $response.Dispose()
    }
}

function Wait-Bridge([Diagnostics.Process] $Server, [string] $BaseUri, [bool] $Connected,
                     [Diagnostics.Process] $Client) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt 15000) {
        if ($Server.HasExited) {
            throw "Server exited ($($Server.ExitCode))"
        }
        if ($null -ne $Client -and $Client.HasExited) {
            throw "Client exited ($($Client.ExitCode))"
        }
        try {
            $health = Read-ServerJson $BaseUri '/api/health'
            if ($health.service -ne 'meowlive' -or $health.protocol_version -ne 3) {
                throw 'Health did not report MeowLive protocol v3'
            }
            if ($health.bridge_connected -eq $Connected) {
                return $health
            }
        } catch [Net.WebException] {
            # Startup and restart temporarily leave the test port without a listener.
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for bridge_connected=$Connected at $BaseUri"
}

function Start-TestServer([string] $Executable, [string] $WorkingDirectory,
                          [bool] $UsesCppConfig, [string] $LogPrefix) {
    $options = @{
        FilePath = $Executable
        ArgumentList = @('--config', 'server.toml')
        WorkingDirectory = $WorkingDirectory
        PassThru = $true
        RedirectStandardOutput = Join-Path $WorkingDirectory "$LogPrefix.stdout.txt"
        RedirectStandardError = Join-Path $WorkingDirectory "$LogPrefix.stderr.txt"
    }
    if ($UsesCppConfig) {
        $options.ArgumentList += @('--data-dir', 'data')
    }
    return Start-Process @options
}

function Test-Pair([string] $Name, [string] $ServerExecutable, [bool] $UsesCppConfig,
                   [string] $ClientExecutable, [bool] $UsesQtClient) {
    $directory = Join-Path $runDirectory $Name
    $null = New-Item -ItemType Directory -Path $directory
    $portReservation = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $portReservation.Start()
    try {
        $port = $portReservation.LocalEndpoint.Port
    } finally {
        $portReservation.Stop()
    }
    $baseUri = "http://127.0.0.1:$port"
    $serverConfig = @"
[server]
listen_address = "127.0.0.1:$port"

[auth]
enabled = false
"@
    if (-not $UsesCppConfig) {
        $serverConfig += @"

[resources]
directory = "resources"

[viewers]
enabled = false

[live]
enabled = false
"@
    }
    Set-Content -LiteralPath (Join-Path $directory 'server.toml') -Value $serverConfig -Encoding Ascii
    $clientConfig = 'server_url = "{0}"' -f $baseUri
    Set-Content -LiteralPath (Join-Path $directory 'desktop.toml') -Value $clientConfig -Encoding Ascii

    $server = $null
    $client = $null
    try {
        $server = Start-TestServer $ServerExecutable $directory $UsesCppConfig 'server'
        $null = Wait-Bridge $server $baseUri $false $null
        $initial = Read-ServerJson $baseUri '/api/status'
        if ([string]::IsNullOrEmpty($initial.session_id)) {
            throw 'The initial server session ID was empty'
        }
        $listener = @{
            LocalAddress = '127.0.0.1'
            LocalPort = $port
            State = 'Listen'
            OwningProcess = $server.Id
            ErrorAction = 'SilentlyContinue'
        }
        if (-not (Get-NetTCPConnection @listener)) {
            throw "The test server did not own its listener on port $port"
        }
        $clientOptions = @{
            FilePath = $ClientExecutable
            WorkingDirectory = $directory
            PassThru = $true
        }
        if ($UsesQtClient) {
            $env:MEOWLIVE_SERVER_URL = $baseUri
        } else {
            $clientOptions.ArgumentList = @('--config', 'desktop.toml', '--simulate')
            $clientOptions.RedirectStandardOutput = Join-Path $directory 'client.stdout.txt'
            $clientOptions.RedirectStandardError = Join-Path $directory 'client.stderr.txt'
        }
        $client = Start-Process @clientOptions

        $health = Wait-Bridge $server $baseUri $true $client
        $timer = [Diagnostics.Stopwatch]::StartNew()
        Stop-TestProcess $server
        $server = Start-TestServer $ServerExecutable $directory $UsesCppConfig 'server-restarted'
        $null = Wait-Bridge $server $baseUri $true $client
        $reconnectMs = $timer.ElapsedMilliseconds
        $restarted = Read-ServerJson $baseUri '/api/status'
        if ([string]::IsNullOrEmpty($restarted.session_id) -or
            $restarted.session_id -eq $initial.session_id) {
            throw 'The restarted server did not use a new session ID'
        }

        $windowTitle = $null
        if ($UsesQtClient) {
            $client.Refresh()
            $windowTitle = $client.MainWindowTitle
            if ($windowTitle -ne 'MeowLive2D' -or -not $client.CloseMainWindow() -or
                -not $client.WaitForExit(5000) -or $client.ExitCode -ne 0) {
                throw "Qt main window did not close cleanly (title: $windowTitle)"
            }
        } else {
            Stop-TestProcess $client
        }
        $null = Wait-Bridge $server $baseUri $false $null
        [pscustomobject] @{
            pair = $Name
            protocol_version = $health.protocol_version
            loopback_port = $port
            initial_server_session = $initial.session_id
            restarted_server_session = $restarted.session_id
            reconnect_ms = $reconnectMs
            disconnect_detected = $true
            qt_window_title = $windowTitle
        }
    } finally {
        try {
            Stop-TestProcess $client
        } finally {
            Stop-TestProcess $server
        }
    }
}

$previousOrigin = $env:MEOWLIVE_SERVER_URL
$previousDeviceToken = $env:MEOWLIVE_DEVICE_TOKEN
$previousDeviceTokenFile = $env:MEOWLIVE_DEVICE_TOKEN_FILE
$env:MEOWLIVE_DEVICE_TOKEN = $null
$env:MEOWLIVE_DEVICE_TOKEN_FILE = $null
try {
    $pairs = @(Test-Pair 'cpp-server_cpp-desktop' $cppServer $true $cppDesktop $true)
    if ($RustRuntimeDirectory) {
        $rustRuntime = (Resolve-Path -LiteralPath $RustRuntimeDirectory).Path
        $rustServer = (Resolve-Path -LiteralPath (Join-Path $rustRuntime 'meowlive-server.exe')).Path
        $rustClient = (Resolve-Path -LiteralPath (Join-Path $rustRuntime 'meowlive-client.exe')).Path
        $pairs += Test-Pair 'rust-server_cpp-desktop' $rustServer $false $cppDesktop $true
        $pairs += Test-Pair 'cpp-server_rust-client' $cppServer $true $rustClient $false
    }
    [pscustomobject] @{
        windows = [Environment]::OSVersion.VersionString
        run_directory = $runDirectory
        pairs = $pairs
    } | ConvertTo-Json -Depth 4
} finally {
    $env:MEOWLIVE_SERVER_URL = $previousOrigin
    $env:MEOWLIVE_DEVICE_TOKEN = $previousDeviceToken
    $env:MEOWLIVE_DEVICE_TOKEN_FILE = $previousDeviceTokenFile
}
