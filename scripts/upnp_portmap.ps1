<#
.SYNOPSIS
  UPnP (IGD) port-mapping helpers for KenshiCoop host sessions. Dot-source
  this file, then call Add-UpnpMapping / Remove-UpnpMapping / Test-UpnpHairpin.

.DESCRIPTION
  Two strategies, tried in order:
    1. The built-in Windows COM object HNetCfg.NATUPnP - instant when it
       works, but Windows disables its discovery on "Public" network
       profiles (very common on friends' machines).
    2. Direct SSDP discovery + SOAP calls to the router's WANIPConnection /
       WANPPPConnection service - pure PowerShell sockets/HTTP, works
       regardless of the Windows network profile.
  No admin rights, no extra binaries; ships in the remote kit as a plain
  script. Every failure is reported as Ok=$false with a reason (never a
  throw) so callers fall back to the manual port-forward checklist.
#>

function Get-LanIPv4 {
    # The local address the router should forward to. Routed-socket trick:
    # "connecting" a UDP socket toward a public address sends no packet but
    # makes the OS pick the outbound interface, whose address we read back.
    # Works without the NetTCPIP module.
    try {
        $u = New-Object System.Net.Sockets.UdpClient
        try {
            $u.Connect("8.8.8.8", 53)
            return $u.Client.LocalEndPoint.Address.ToString()
        } finally { $u.Close() }
    } catch { return $null }
}

# ---- Strategy 2 internals: SSDP + SOAP -------------------------------------------

function Find-UpnpIgd {
    # SSDP M-SEARCH for an Internet Gateway Device, then fetch each announced
    # device description and look for a WANIPConnection/WANPPPConnection
    # service. Returns @{ ControlUrl; ServiceType } or $null.
    $locations = @()
    try {
        $udp = New-Object System.Net.Sockets.UdpClient
        try {
            $udp.Client.ReceiveTimeout = 1000
            foreach ($st in @("urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                              "urn:schemas-upnp-org:device:InternetGatewayDevice:2")) {
                $msg = @(
                    "M-SEARCH * HTTP/1.1",
                    "HOST: 239.255.255.250:1900",
                    "MAN: `"ssdp:discover`"",
                    "MX: 2",
                    "ST: $st",
                    "", ""
                ) -join "`r`n"
                $bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
                [void]$udp.Send($bytes, $bytes.Length, "239.255.255.250", 1900)
            }
            $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $deadline = (Get-Date).AddSeconds(3)
            while ((Get-Date) -lt $deadline) {
                try {
                    $data = $udp.Receive([ref]$remote)
                    $text = [System.Text.Encoding]::ASCII.GetString($data)
                    if ($text -match "(?im)^LOCATION:\s*(\S+)") {
                        $loc = $Matches[1].Trim()
                        if ($locations -notcontains $loc) { $locations += $loc }
                    }
                } catch { }  # receive timeout; keep waiting until the deadline
            }
        } finally { $udp.Close() }
    } catch { return $null }

    foreach ($loc in $locations) {
        try {
            $resp = Invoke-WebRequest -Uri $loc -UseBasicParsing -TimeoutSec 5
            $xml = [xml]$resp.Content
            $base = New-Object System.Uri($loc)
            foreach ($svc in $xml.SelectNodes("//*[local-name()='service']")) {
                $type = "$($svc.serviceType)"
                if ($type -match "WAN(IP|PPP)Connection") {
                    $ctl = New-Object System.Uri($base, "$($svc.controlURL)")
                    return @{ ControlUrl = $ctl.AbsoluteUri; ServiceType = $type }
                }
            }
        } catch { }
    }
    return $null
}

function Invoke-UpnpSoap {
    # POST one SOAP action to the IGD control URL. Returns the response [xml].
    # Throws with a readable message on SOAP faults (UPnP errorCode included).
    param(
        [string]$ControlUrl,
        [string]$ServiceType,
        [string]$Action,
        [System.Collections.Specialized.OrderedDictionary]$Arguments
    )
    $argXml = ""
    if ($null -ne $Arguments) {
        foreach ($k in $Arguments.Keys) {
            $v = [System.Security.SecurityElement]::Escape("$($Arguments[$k])")
            $argXml += "<$k>$v</$k>"
        }
    }
    $body = "<?xml version=`"1.0`"?>" +
        "<s:Envelope xmlns:s=`"http://schemas.xmlsoap.org/soap/envelope/`" " +
        "s:encodingStyle=`"http://schemas.xmlsoap.org/soap/encoding/`">" +
        "<s:Body><u:$Action xmlns:u=`"$ServiceType`">$argXml</u:$Action></s:Body></s:Envelope>"
    try {
        $resp = Invoke-WebRequest -Uri $ControlUrl -Method Post -UseBasicParsing -TimeoutSec 10 `
            -ContentType 'text/xml; charset="utf-8"' `
            -Headers @{ "SOAPAction" = "`"$ServiceType#$Action`"" } `
            -Body $body
        return [xml]$resp.Content
    } catch [System.Net.WebException] {
        # SOAP faults arrive as HTTP 500 with a UPnPError body.
        $detail = ""
        try {
            $stream = $_.Exception.Response.GetResponseStream()
            $reader = New-Object System.IO.StreamReader($stream)
            $errBody = $reader.ReadToEnd()
            if ($errBody -match "<errorCode>(\d+)</errorCode>") { $detail = "UPnP error $($Matches[1])" }
            if ($errBody -match "<errorDescription>([^<]+)</errorDescription>") { $detail += " ($($Matches[1]))" }
        } catch { }
        if ($detail -eq "") { $detail = $_.Exception.Message }
        throw "$Action failed: $detail"
    }
}

# ---- Public API --------------------------------------------------------------------

function Add-UpnpMapping {
    # Ask the router (via UPnP/IGD) to forward UDP $Port to this PC.
    # Returns @{ Ok; ExternalIp; LanIp; Error }. ExternalIp is the address the
    # IGD itself reports - if it differs from what api.ipify.org sees, the
    # host is likely behind a second NAT (CGNAT) and the mapping won't help.
    param(
        [Parameter(Mandatory = $true)][int]$Port,
        [string]$Description = "KenshiCoop"
    )
    $result = [pscustomobject]@{ Ok = $false; ExternalIp = $null; LanIp = $null; Error = "" }
    $lanIp = Get-LanIPv4
    if (-not $lanIp) {
        $result.Error = "could not determine this PC's LAN IP"
        return $result
    }
    $result.LanIp = $lanIp

    # -- Strategy 1: Windows COM object (fast, but off on Public network profiles) --
    try {
        $nat = New-Object -ComObject HNetCfg.NATUPnP
        $col = $nat.StaticPortMappingCollection
        if ($null -ne $col) {
            [void]$col.Add($Port, "UDP", $Port, $lanIp, $true, $Description)
            $check = $col.Item($Port, "UDP")
            if ($null -ne $check) {
                $result.Ok = $true
                try { $result.ExternalIp = $check.ExternalIPAddress } catch {}
                return $result
            }
        }
    } catch { }

    # -- Strategy 2: direct SSDP discovery + SOAP (network-profile independent) --
    $igd = Find-UpnpIgd
    if ($null -eq $igd) {
        $result.Error = "router did not answer UPnP (it may be disabled in the router settings)"
        return $result
    }
    try {
        $addArgs = New-Object System.Collections.Specialized.OrderedDictionary
        $addArgs["NewRemoteHost"]             = ""
        $addArgs["NewExternalPort"]           = $Port
        $addArgs["NewProtocol"]               = "UDP"
        $addArgs["NewInternalPort"]           = $Port
        $addArgs["NewInternalClient"]         = $lanIp
        $addArgs["NewEnabled"]                = 1
        $addArgs["NewPortMappingDescription"] = $Description
        $addArgs["NewLeaseDuration"]          = 0
        [void](Invoke-UpnpSoap -ControlUrl $igd.ControlUrl -ServiceType $igd.ServiceType `
                               -Action "AddPortMapping" -Arguments $addArgs)

        # Verify the router actually kept the mapping.
        $qryArgs = New-Object System.Collections.Specialized.OrderedDictionary
        $qryArgs["NewRemoteHost"]   = ""
        $qryArgs["NewExternalPort"] = $Port
        $qryArgs["NewProtocol"]     = "UDP"
        $qry = Invoke-UpnpSoap -ControlUrl $igd.ControlUrl -ServiceType $igd.ServiceType `
                               -Action "GetSpecificPortMappingEntry" -Arguments $qryArgs
        $client = $qry.SelectSingleNode("//*[local-name()='NewInternalClient']")
        if ($null -eq $client -or "$($client.InnerText)" -ne $lanIp) {
            $result.Error = "router accepted the mapping but it points at '$($client.InnerText)' instead of $lanIp"
            return $result
        }

        $result.Ok = $true
        try {
            $ext = Invoke-UpnpSoap -ControlUrl $igd.ControlUrl -ServiceType $igd.ServiceType `
                                   -Action "GetExternalIPAddress" -Arguments $null
            $node = $ext.SelectSingleNode("//*[local-name()='NewExternalIPAddress']")
            if ($null -ne $node -and "$($node.InnerText)" -ne "") { $result.ExternalIp = $node.InnerText }
        } catch { }
        return $result
    } catch {
        $result.Error = "UPnP request failed: $_"
        return $result
    }
}

function Remove-UpnpMapping {
    param([Parameter(Mandatory = $true)][int]$Port)
    # Best effort on both paths; whichever created the mapping can delete it,
    # and deleting a nonexistent mapping is harmless.
    try {
        $nat = New-Object -ComObject HNetCfg.NATUPnP
        $col = $nat.StaticPortMappingCollection
        if ($null -ne $col) { $col.Remove($Port, "UDP") }
    } catch {}
    try {
        $igd = Find-UpnpIgd
        if ($null -ne $igd) {
            $delArgs = New-Object System.Collections.Specialized.OrderedDictionary
            $delArgs["NewRemoteHost"]   = ""
            $delArgs["NewExternalPort"] = $Port
            $delArgs["NewProtocol"]     = "UDP"
            [void](Invoke-UpnpSoap -ControlUrl $igd.ControlUrl -ServiceType $igd.ServiceType `
                                   -Action "DeletePortMapping" -Arguments $delArgs)
        }
    } catch {}
}

function Test-UpnpHairpin {
    # Best-effort external-reachability probe: listen on UDP $Port, fire a
    # datagram at our own public IP:$Port, and see if the router loops it back
    # through the fresh mapping (NAT hairpinning). Many consumer routers don't
    # hairpin at all, so the only trustworthy answers are "reachable" and
    # "inconclusive" - never treat this probe alone as a failure.
    # Call BEFORE the game launches (the game needs the port).
    param(
        [Parameter(Mandatory = $true)][int]$Port,
        [Parameter(Mandatory = $true)][string]$ExternalIp
    )
    $listener = $null
    $sender = $null
    try {
        $token = "KenshiCoopHairpin_" + [Guid]::NewGuid().ToString("N")
        $payload = [System.Text.Encoding]::ASCII.GetBytes($token)
        $listener = New-Object System.Net.Sockets.UdpClient($Port)
        $listener.Client.ReceiveTimeout = 2500
        $sender = New-Object System.Net.Sockets.UdpClient
        [void]$sender.Send($payload, $payload.Length, $ExternalIp, $Port)
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $deadline = (Get-Date).AddSeconds(3)
        while ((Get-Date) -lt $deadline) {
            try {
                $data = $listener.Receive([ref]$remote)
                if ([System.Text.Encoding]::ASCII.GetString($data) -eq $token) { return "reachable" }
            } catch { break }  # receive timeout
        }
        return "inconclusive"
    } catch {
        return "inconclusive"
    } finally {
        if ($null -ne $sender)   { $sender.Close() }
        if ($null -ne $listener) { $listener.Close() }
    }
}
