# Vendored shared binaries

`alut.dll` and `tpf2_pluginhost.dll` are built in the tpf2-multiplayer repository
and copied here unchanged. Both packages install them under the SAME component
GUIDs (see PluginHost.wxs), so they must be the same bytes. Regenerate with
`tools\vendor_host.ps1`; never edit or rebuild them here.

source repo:    https://github.com/silver2127/tpf2-multiplayer
source release: v0.6.1.14 (TpF2Multiplayer.msi, sha256 7a907d792e9c7c18401f04e68f10a0652448fcdc1f155c73e9cda840b8edbab3)
source commit:  664110faba12260f732f7cf64921dcd81c9942aa (the tag)
extracted:      from an administrative image
vendored on:    2026-09-21 13:36

| file | bytes | sha256 |
| --- | --- | --- |
| alut.dll | 180224 | 7955c0338ed6e3870171f15bf60985594bf25ad7343e9af133c3ba61431ab15c |
| tpf2_pluginhost.dll | 219136 | a5ac23603cc8e47f760c17be37bc514261d4d12d991e557c19249f3760809f02 |

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

The multiplayer 0.6.1.14 release ships a 225792-byte DLL (sha256
c042dea695627ca78440c04f0b1442790e122a1f16b025affc1ee40d2dac72f2) that does not
contain the TPF2_ALLOW_GOG property (checked on the binary, 2026-11-24), so it
still refuses a GOG folder: the package built with it said so on a GOG machine.
Re-vendor the CA from the next TpF2Multiplayer.msi once that release carries
this change, and drop this section then.
