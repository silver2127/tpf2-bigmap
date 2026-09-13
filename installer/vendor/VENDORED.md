# Vendored shared binaries

These three files are built in the tpf2-multiplayer repository and copied here
unchanged. Both packages ship them under the SAME component GUIDs (see
PluginHost.wxs), so they must be the same bytes. Regenerate with
`tools\vendor_host.ps1`; never edit or rebuild them here.

source repo:    https://github.com/silver2127/tpf2-multiplayer
source release: v0.4.19 (TpF2Multiplayer-v0.4.19.msi, sha256 5c4f94cee2c2ac31668bcbd28ce5914124ea91790ea42588e2cf9d9a133ab630)
source commit:  ea402aa230dadc08d14279dd4b0434fe5e7267e5 (the tag)
extracted:      alut.dll and tpf2_pluginhost.dll from an administrative image, tpf2ca.dll from the Binary table
vendored on:    2026-09-13 12:58

| file | bytes | sha256 |
| --- | --- | --- |
| alut.dll | 177664 | c1a7c8e01eb10b3f478d50012b5b8f39105740785d7ff028fde8b2ef84387965 |
| tpf2_pluginhost.dll | 218624 | cde64c6542e89d3bf931c0677d9d5f248ac0a08b3107da451da69074144f5fa4 |
| tpf2ca.dll | 216064 | fb97fb38851a7e5d87a8479ae19535947bc80b83b92f063eda21501b51aa60e4 |
