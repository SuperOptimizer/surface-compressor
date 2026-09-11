from pathlib import Path
import hashlib,json,struct
root=Path(__file__).resolve().parent/'fixtures'
manifest=json.loads((root/'manifest.json').read_text())
assert {p.name for p in root.glob('*.sfc')}==set(manifest)
for name,info in manifest.items():
    data=(root/name).read_bytes()
    assert len(data)==info['size'],name
    assert hashlib.sha256(data).hexdigest()==info['sha256'],name
    assert struct.unpack_from('<I',data,4)[0]==info['version'],name
print('Permanent compatibility fixture identities verified')
