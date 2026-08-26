$hostName = "127.0.0.1"
$port = 5000
$message = "longer string bro I love UDP, TCP SUCKS!"
$bytes = [System.Text.Encoding]::UTF8.GetBytes($message)

# UDP
$udp = [System.Net.Sockets.UdpClient]::new()
# Set a 2-second timeout so it doesn't hang forever waiting for a reply
$udp.Client.ReceiveTimeout = 2000

try
{
	# Send the packet
	[void]$udp.Send($bytes, $bytes.Length, $hostName, $port)
	Write-Host "Sent UDP packet: $message"

	# Wait for the OS to bounce it back
	$remoteEP = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
	try
	{
		$reply = $udp.Receive([ref]$remoteEP)
		$replyText = [System.Text.Encoding]::UTF8.GetString($reply)
		Write-Host "Received reply: $replyText"
	} catch
	{
		Write-Host "No reply received (timeout)."
	}
} finally
{
	$udp.Dispose()
}
