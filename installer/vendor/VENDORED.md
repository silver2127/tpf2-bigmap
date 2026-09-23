# Vendored shared binaries

`alut.dll` and `tpf2_pluginhost.dll` are built in the tpf2-multiplayer repository
and copied here unchanged. Both packages install them under the SAME component
GUIDs (see PluginHost.wxs), so they must be the same bytes. Regenerate with
`tools\vendor_host.ps1`; never edit or rebuild them here.

source repo:    https://github.com/silver2127/tpf2-multiplayer
source release: v0.6.1 (TpF2Multiplayer.msi, sha256 e866ea59ec0c6ab6fe2702d95bc8d4d73c8254b7e62c14256902a521b1005cd7)
source commit:  652316432a6ca44ec1b9345a999e0ecfccc6e958 (the tag)
extracted:      from an administrative image
vendored on:    2026-09-17 20:15

| file | bytes | sha256 |
| --- | --- | --- |
| alut.dll | 214016 | 36d41b77fd46bd05a4c667bf2567f03a6f7aafc837887326f3ed158416bfa444 |
| tpf2_pluginhost.dll | 219136 | e87234ab76a2ac9692210d47ed49638368286600b23016436a16b5dcdeccbf44 |

## tpf2ca.dll is a deliberate exception

The installer custom-action DLL is **not** installed into the game folder: both
packages carry it as an embedded `Binary` (PluginHost.wxs, `Tpf2CustomActions`),
so no component GUID is shared and the two copies do not have to match. This
copy is built from the tpf2-multiplayer checkout:

source commit:  513d0d597e475ccb60d05a35603935b29e81e2b8 (branch gog/custom-action-gog-build)
built by:       installer\ca\build_ca.bat in that checkout
built on:       2026-09-21
reason:         GogAllowed(). The game-folder check learned the two other
                places it had assumed Steam:
                  - the executable: the GOG 2024-12-12 build (TimeDateStamp
                    0x675ad7cc, SizeOfImage 0x0467d000) is now a second
                    recognised build;
                  - the game's own alut.dll, which the two builds ship as
                    different bytes under the same name and size
                    (Steam 3DF103AE..., GOG 53ED311A...).
                Both are accepted only when the package sets TPF2_ALLOW_GOG=1.
                The Big Maps GOG package does; every other package, multiplayer's
                included, leaves the property unset and keeps the previous
                Steam-only behaviour exactly.

| file | bytes | sha256 |
| --- | --- | --- |
| tpf2ca.dll | 217088 | dcf2f4337002dff21ef3add4647ea6544c68c47e398c7627802306f8766adc95 |

The multiplayer 0.6.1 release ships the older 216064-byte DLL
(sha256 55dd493bfd45fa21a05b8c78e1238505dd218e4e60d15a91a66e5f81a435b50f).
Re-vendor the CA from the next TpF2Multiplayer.msi once that release carries
this change, and drop this section then.
