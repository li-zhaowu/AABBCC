# Lzw custom features

The December 2025 custom server's automatic reverse-repeater connection is
available alongside the current clipboard, file manager, video and audio features.
Automatic connection is disabled by default.

Add the following section to the server's existing INI file:

```ini
[auto_reverse]
AutoReverseEnabled=1
AutoReverseRepeaterHost=your-repeater-host
AutoReverseRepeaterPort=5500
AutoReverseId=ID:123456
AutoReverseReconnectInterval=5
```

The server maintains one waiting/connected reverse client. When that client
disconnects, it reconnects after the configured interval. Configuration keys
match the old custom build and are preserved when saving server settings.
The existing reverse-authentication setting still applies.

For application mode the old switches are also supported:

```text
winvnc.exe -autoreverse -repeater your-repeater-host:5500 -id:123456 -reconnect 5 -run
```

Command-line values override the INI for that run. For service mode, configure
the INI used by the service worker. Hostnames and IPv4 addresses can have
`host:port` on the command line. For IPv6 use the INI's separate host/port
fields and enable the server's IPv6 setting.

The migration fixes the old 250-byte ID send, which read past the string.
The new registration is a zero-padded 250-byte record. Connection ownership
uses an atomic lease instead of the old unsynchronized connection counter.
The reconnect worker is joined before the server is destroyed. Shutdown
may wait for an in-progress operating-system DNS/connection attempt.

Local loopback smoke tests exercised the actual server executable with a mock
repeater. Both INI startup and the legacy command line registered the expected
250-byte ID twice after forced disconnects. CLI host, port and ID overrides
were checked against different INI values. Full remote-control interoperability,
service restart and a real repeater/network outage still need target-machine
validation.

## Independent legacy repeater

`lzw-repeater/LzwRepeater.sln` is a separate VS 2022 project imported from
`repeater_v140_src`. It builds `LzwRepeater.exe` and leaves the current
`repeater/repeater.sln` project in place. Its original GPL notices are retained.
It uses the source's existing configuration/UI; run it separately from any
other repeater bound to the same ports.

Build x64 Release using VS 2022 MSBuild:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe' lzw-repeater/LzwRepeater.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

Intermediate and output files use the repository's Z-drive build settings.
