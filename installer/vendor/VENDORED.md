# Vendored shared binaries

These three files are built in the tpf2-multiplayer repository and copied here
unchanged. Both packages ship them under the SAME component GUIDs (see
PluginHost.wxs), so they must be the same bytes. Regenerate with
`tools\vendor_host.ps1`; never edit or rebuild them here.

source repo:    https://github.com/silver2127/tpf2-multiplayer
source release: v0.4.18 (TpF2Multiplayer-0.4.18.msi, sha256 66b2367747fe2c092e011e2a49f60d9fb17052727e57ca6a1e28b12bcbaed3fe)
source commit:  5351b5824ae4e5c192ef6182c73617a1d3ce6130 (the tag)
extracted:      alut.dll and tpf2_pluginhost.dll from an administrative image, tpf2ca.dll from the Binary table
vendored on:    2026-09-12 20:11

| file | bytes | sha256 |
| --- | --- | --- |
| alut.dll | 177664 | c1a7c8e01eb10b3f478d50012b5b8f39105740785d7ff028fde8b2ef84387965 |
| tpf2_pluginhost.dll | 218624 | cde64c6542e89d3bf931c0677d9d5f248ac0a08b3107da451da69074144f5fa4 |
| tpf2ca.dll | 216064 | fb97fb38851a7e5d87a8479ae19535947bc80b83b92f063eda21501b51aa60e4 |
