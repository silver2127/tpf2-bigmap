"""Black-box standalone, upgrade and multiplayer coexistence tests."""
from pathlib import Path
import subprocess,tempfile,sys,os
package=Path(sys.argv[1]).resolve()
def run(*args,**kwargs):
    return subprocess.run([str(a) for a in args],check=True,capture_output=True,text=True,**kwargs)
with tempfile.TemporaryDirectory(prefix='bigmap-install-') as tmp:
    root=Path(tmp)/'tpf2mp'
    run('bash',package/'install.sh','--prefix',root)
    plugin=root/'data/plugins/tpf2_bigmap.so';cfg=plugin.with_suffix('.cfg')
    assert plugin.is_file() and cfg.is_file()
    run(root/'tpf2-bigmap-launch','/usr/bin/true')
    assert 'not TransportFever2' in (root/'data/tpf2mp_host.log').read_text()
    cfg.write_text('[tpf2_bigmap]\nenabled=0\n')
    shared=root/'tpf2_pluginhost.so';before=shared.read_bytes()
    saves=root/'data/save.sav';saves.write_bytes(b'user save')
    # An existing multiplayer launcher/host is never overwritten.
    mp=root/'tpf2mp-launch';mp.write_text('#!/bin/sh\nprintf coexist-ok\n');mp.chmod(0o755)
    run('bash',package/'install.sh','--prefix',root)
    assert shared.read_bytes()==before and 'enabled=0' in cfg.read_text()
    assert cfg.with_suffix('.cfg.example').exists()
    assert run(root/'tpf2-bigmap-launch','/usr/bin/true').stdout=='coexist-ok'
    run('bash',package/'uninstall.sh','--prefix',root)
    assert not plugin.exists() and cfg.exists() and saves.read_bytes()==b'user save'
    assert shared.read_bytes()==before and mp.exists()
print('PASS: checksums, standalone host launch, config-preserving upgrade, multiplayer coexistence, uninstall/save preservation')
