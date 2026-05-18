param(
    [string]$Server   = "192.168.2.1",
    [int]   $Port     = 5001,
    [int]   $TotalMb  = 1024,
    [int]   $ChunkKb  = 64
)

$client = New-Object System.Net.Sockets.TcpClient
$client.NoDelay      = $true
$client.SendBufferSize = 4MB
$client.ReceiveBufferSize = 4MB
$client.Connect($Server, $Port)
$stream = $client.GetStream()
"Connected to $($Server):$Port"

$chunkBytes = $ChunkKb * 1024
$buf = New-Object byte[] $chunkBytes
(New-Object Random).NextBytes($buf)

$iters    = ($TotalMb * 1024 * 1024) / $chunkBytes
$sw       = [Diagnostics.Stopwatch]::StartNew()
$report   = [Diagnostics.Stopwatch]::StartNew()
$sentBytes = 0L

for ($i = 0; $i -lt $iters; $i++) {
    $stream.Write($buf, 0, $buf.Length)
    $sentBytes += $buf.Length
    if ($report.Elapsed.TotalSeconds -ge 2) {
        $rate = [math]::Round($sentBytes / 1MB / $sw.Elapsed.TotalSeconds, 1)
        "  $([math]::Round($sentBytes / 1MB)) MB sent — instant avg $rate MB/s"
        $report.Restart()
    }
}
$stream.Flush()
$stream.Close()
$client.Close()
$sw.Stop()

$mbps = [math]::Round($TotalMb / $sw.Elapsed.TotalSeconds, 1)
""
"=== Result ==="
"Sent $TotalMb MB in $([math]::Round($sw.Elapsed.TotalSeconds,2)) s"
"Raw TCP throughput: $mbps MB/s ($([math]::Round($mbps * 8, 0)) Mbps)"
