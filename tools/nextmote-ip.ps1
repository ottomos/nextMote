[CmdletBinding()]
param(
    [string]$InterfaceAlias
)

$ErrorActionPreference = 'Stop'

function ConvertTo-IPv4Integer {
    param([string]$Address)

    $bytes = [System.Net.IPAddress]::Parse($Address).GetAddressBytes()
    [uint64]$value = 0
    foreach ($byte in $bytes) {
        $value = ($value -shl 8) -bor [uint64]$byte
    }
    return $value
}

function ConvertTo-IPv4Address {
    param([uint64]$Value)

    $bytes = [byte[]]@(
        [byte](($Value -shr 24) -band 255),
        [byte](($Value -shr 16) -band 255),
        [byte](($Value -shr 8) -band 255),
        [byte]($Value -band 255)
    )
    return ([System.Net.IPAddress]::new($bytes)).ToString()
}

if ($InterfaceAlias) {
    $adapter = Get-NetAdapter -Name $InterfaceAlias -ErrorAction Stop
    $interfaceIndex = $adapter.ifIndex
}
else {
    $defaultRoute = Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' |
        Sort-Object RouteMetric |
        Select-Object -First 1
    if (-not $defaultRoute) {
        throw 'No default IPv4 network route found. Specify -InterfaceAlias.'
    }
    $interfaceIndex = $defaultRoute.InterfaceIndex
}

$localAddress = Get-NetIPAddress -InterfaceIndex $interfaceIndex -AddressFamily IPv4 |
    Where-Object {
        $_.AddressState -eq 'Preferred' -and
        $_.IPAddress -notmatch '^(127\.|169\.254\.)'
    } |
    Sort-Object PrefixLength -Descending |
    Select-Object -First 1

if (-not $localAddress) {
    throw "No usable IPv4 address found on interface index $interfaceIndex."
}

$prefixLength = [int]$localAddress.PrefixLength
$addressCount = [uint64]1 -shl (32 - $prefixLength)
if ($addressCount -gt 256) {
    $networkText = "$($localAddress.IPAddress)/$prefixLength"
    throw "$networkText has more than 256 addresses; refusing a broad scan. Choose a smaller local interface."
}

$mask = ([uint64]4294967295 -shl (32 - $prefixLength)) -band [uint64]4294967295
$localInteger = ConvertTo-IPv4Integer $localAddress.IPAddress
$networkInteger = $localInteger -band $mask
$broadcastInteger = $networkInteger + $addressCount - 1

if ($prefixLength -ge 31) {
    $firstHost = $networkInteger
    $lastHost = $broadcastInteger
}
else {
    $firstHost = $networkInteger + 1
    $lastHost = $broadcastInteger - 1
}

$addresses = @()
for ([uint64]$value = $firstHost; $value -le $lastHost; $value++) {
    if ($value -ne $localInteger) {
        $addresses += ConvertTo-IPv4Address $value
    }
}

if ($addresses.Count -eq 0) {
    throw 'No other IPv4 host addresses are available to search on this subnet.'
}

if (-not ('NextMoteHttpProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Text;
using System.Threading.Tasks;

public static class NextMoteHttpProbe
{
    private static string ToRecord(string address, HttpWebResponse response)
    {
        var body = "";
        try
        {
            using (var stream = response.GetResponseStream())
            using (var memory = new MemoryStream())
            {
                if (stream != null)
                {
                    var buffer = new byte[512];
                    var total = 0;
                    int read;
                    while (total < 2048 &&
                           (read = stream.Read(buffer, 0, Math.Min(buffer.Length, 2048 - total))) > 0)
                    {
                        memory.Write(buffer, 0, read);
                        total += read;
                    }
                    body = Encoding.UTF8.GetString(memory.ToArray());
                }
            }
        }
        catch (IOException)
        {
        }

        return address + "\t" + (int)response.StatusCode + "\t" + body;
    }

    public static string[] GetResponses(string[] addresses, string path)
    {
        var responses = new ConcurrentBag<string>();
        Parallel.ForEach(
            addresses,
            new ParallelOptions { MaxDegreeOfParallelism = 16 },
            address =>
            {
                var request = (HttpWebRequest)WebRequest.Create("http://" + address + path);
                request.Method = "GET";
                request.Proxy = null;
                request.Timeout = 2000;
                request.ReadWriteTimeout = 2000;

                try
                {
                    using (var response = (HttpWebResponse)request.GetResponse())
                    {
                        responses.Add(ToRecord(address, response));
                    }
                }
                catch (WebException exception)
                {
                    using (var response = exception.Response as HttpWebResponse)
                    {
                        if (response != null)
                            responses.Add(ToRecord(address, response));
                    }
                }
            });

        return responses.ToArray();
    }
}
'@
}

Write-Host "Searching $($localAddress.IPAddress)/$prefixLength on '$($localAddress.InterfaceAlias)' (HTTP status checks only; nextMote will not be reset)..."

$matches = @()
$normalMatches = @()
$httpResponders = @()
foreach ($response in [NextMoteHttpProbe]::GetResponses([string[]]$addresses, '/api/status')) {
    $firstSeparator = $response.IndexOf("`t")
    $secondSeparator = $response.IndexOf("`t", $firstSeparator + 1)
    if ($firstSeparator -lt 0 -or $secondSeparator -lt 0) {
        continue
    }

    $address = $response.Substring(0, $firstSeparator)
    $status = [int]$response.Substring($firstSeparator + 1, $secondSeparator - $firstSeparator - 1)
    $body = $response.Substring($secondSeparator + 1)
    $httpResponders += $address
    if ($status -ne 200) {
        continue
    }

    try {
        $payload = ConvertFrom-Json -InputObject $body
    }
    catch {
        continue
    }

    $properties = @($payload.PSObject.Properties.Name)
    if (
        $properties -contains 'pairing_profile' -and
        $properties -contains 'pairing_phase' -and
        $properties -contains 'sync_window'
    ) {
        $normalMatches += $address
        $matches += [pscustomobject]@{ Address = $address; Mode = 'normal' }
    }
}

$setupAddresses = @($httpResponders | Sort-Object -Unique | Where-Object { $_ -notin $normalMatches })
foreach ($response in [NextMoteHttpProbe]::GetResponses([string[]]$setupAddresses, '/api/wifi/state')) {
    $firstSeparator = $response.IndexOf("`t")
    $secondSeparator = $response.IndexOf("`t", $firstSeparator + 1)
    if ($firstSeparator -lt 0 -or $secondSeparator -lt 0) {
        continue
    }

    $address = $response.Substring(0, $firstSeparator)
    $status = [int]$response.Substring($firstSeparator + 1, $secondSeparator - $firstSeparator - 1)
    if ($status -ne 200) {
        continue
    }
    $body = $response.Substring($secondSeparator + 1)
    try {
        $payload = ConvertFrom-Json -InputObject $body
    }
    catch {
        continue
    }

    if (
        $payload.mode -eq 'setup' -and
        $payload.ap -is [string] -and
        $payload.ap.StartsWith('nextMote-Setup-')
    ) {
        $matches += [pscustomobject]@{ Address = $address; Mode = 'setup' }
    }
}

if ($matches.Count -eq 0) {
    Write-Error 'No nextMote found. Check that it is on this network and its web server is running.'
}

foreach ($match in $matches) {
    $suffix = if ($match.Mode -eq 'setup') { ' (setup mode)' } else { '' }
    Write-Output "nextMote found$suffix`: http://$($match.Address)/"
}
