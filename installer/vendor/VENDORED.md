# Vendored shared binaries

These three files are built in the tpf2-multiplayer repository and copied here
unchanged. Both packages ship them under the SAME component GUIDs (see
PluginHost.wxs), so they must be the same bytes. Regenerate with
`tools\vendor_host.ps1`; never edit or rebuild them here.

source repo:   https://github.com/silver2127/tpf2-multiplayer
source commit: c90e97d81c97018f3fc45468419923a97da94f7d (c90e97d, branch plugin-host)
source tree:   clean
vendored on:   2026-09-07 14:12

| file | bytes | sha256 |
| --- | --- | --- |
| alut.dll | 161792 | f218c90fd24479930c0f7f07693a8922710d6569befb250c854b3329cee934bb |
| tpf2_pluginhost.dll | 219136 | 4c3bbdbe1241c0db72da5d5f80bbaa89ed8f1ece81b6b0e51d9ad7c4ab2165c1 |
| tpf2ca.dll | 139264 | 83d4e3d9fcf8c5551287cb3d7180e6cf9e2d35d39486d38d1a7bf982b78254b8 |
