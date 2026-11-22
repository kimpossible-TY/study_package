$ErrorActionPreference = "Stop"

$client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 1055)
$stream = $client.GetStream()
$stream.ReadTimeout = 5000

# SOCKS5 greeting: VER=5, NMETHODS=1, NO_AUTH=0
$stream.Write([byte[]](5, 1, 0), 0, 3)
$greet = New-Object byte[] 2
$got = $stream.Read($greet, 0, 2)
if ($got -ne 2 -or $greet[0] -ne 5 -or $greet[1] -ne 0) {
    throw "SOCKS5 greeting failed"
}

# SOCKS5 CONNECT 100.80.74.94:5900 (ATYP=1 IPv4)
$req = [byte[]](5, 1, 0, 1, 100, 80, 74, 94, 0x17, 0x0C)
$stream.Write($req, 0, $req.Length)

$rep = New-Object byte[] 10
$got = $stream.Read($rep, 0, 10)
if ($got -ne 10 -or $rep[1] -ne 0) {
    throw "SOCKS5 connect failed: reply=$($rep[1])"
}

$bannerBytes = New-Object byte[] 12
$got = $stream.Read($bannerBytes, 0, 12)
$banner = [System.Text.Encoding]::ASCII.GetString($bannerBytes, 0, $got).Trim()
$client.Close()

Write-Output "SOCKS5_OK banner=[$banner]"
