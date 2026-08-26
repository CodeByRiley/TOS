$port = 6000
$udpClient = $null

try {
    $udpClient = [System.Net.Sockets.UdpClient]::new($port)

    # Make Receive() return periodically so Ctrl+C can be processed
    $udpClient.Client.ReceiveTimeout = 500

    $remoteEP = [System.Net.IPEndPoint]::new(
        [System.Net.IPAddress]::Any,
        0
    )

    Write-Host "UDP Server listening on port $port..."
    Write-Host "Press Ctrl+C to stop."

    while ($true) {
        try {
            $bytes = $udpClient.Receive([ref]$remoteEP)
        }
        catch [System.Net.Sockets.SocketException] {
            # WSAETIMEDOUT = 10060; continue checking for Ctrl+C
            if ($_.Exception.ErrorCode -eq 10060) {
                continue
            }

            throw
        }

        $message = [System.Text.Encoding]::UTF8.GetString($bytes)

        Write-Host "Received from $remoteEP`: $message"

        $reply = "Hello from Windows Host!"
        $replyBytes = [System.Text.Encoding]::UTF8.GetBytes($reply)

        [void]$udpClient.Send(
            $replyBytes,
            $replyBytes.Length,
            $remoteEP
        )

        Write-Host "Sent reply."
    }
}
catch {
    if ($_.Exception -is [System.Management.Automation.PipelineStoppedException] -or
        $_.Exception -is [System.OperationCanceledException] -or
        $_.FullyQualifiedErrorId -like "*OperationStopped*") {
        Write-Host "`nServer stopped."
    }
    else {
        Write-Error "UDP server failed: $($_.Exception.Message)"
    }
}
finally {
    if ($null -ne $udpClient) {
        $udpClient.Close()
        $udpClient.Dispose()
        $udpClient = $null
    }

    Write-Host "Socket closed."
}
