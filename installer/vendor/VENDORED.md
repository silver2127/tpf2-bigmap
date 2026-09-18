# Vendored shared binaries

These three files are built in the tpf2-multiplayer repository and copied here
unchanged. Both packages ship them under the SAME component GUIDs (see
PluginHost.wxs), so they must be the same bytes. Regenerate with
`tools\vendor_host.ps1`; never edit or rebuild them here.

source repo:    https://github.com/silver2127/tpf2-multiplayer
source release: v0.6.1 (TpF2Multiplayer.msi, sha256 e866ea59ec0c6ab6fe2702d95bc8d4d73c8254b7e62c14256902a521b1005cd7)
source commit:  652316432a6ca44ec1b9345a999e0ecfccc6e958 (the tag)
extracted:      alut.dll and tpf2_pluginhost.dll from an administrative image, tpf2ca.dll from the Binary table
vendored on:    2026-09-17 20:15

| file | bytes | sha256 |
| --- | --- | --- |
| alut.dll | 214016 | 36d41b77fd46bd05a4c667bf2567f03a6f7aafc837887326f3ed158416bfa444 |
| tpf2_pluginhost.dll | 219136 | e87234ab76a2ac9692210d47ed49638368286600b23016436a16b5dcdeccbf44 |
| tpf2ca.dll | 216064 | 55dd493bfd45fa21a05b8c78e1238505dd218e4e60d15a91a66e5f81a435b50f |
