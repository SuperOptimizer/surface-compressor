"""Reproduce the pinned real-surface release checks with bounded downloads."""
import argparse,hashlib,json,subprocess,time,urllib.request
from pathlib import Path

def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1048576),b''):h.update(block)
    return h.hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--decoder',type=Path)
    parser.add_argument('--manifest',type=Path,default=Path(__file__).resolve().parents[1]/'tests/corpus/real.json')
    args=parser.parse_args();args.cache.mkdir(parents=True,exist_ok=True);args.output.mkdir(parents=True,exist_ok=True)
    encoder=str(args.exe.resolve());decoder=str((args.decoder or args.exe).resolve())
    version=subprocess.check_output([encoder,'--version'],text=True).strip();results=[]
    for entry in json.loads(args.manifest.read_text()):
        source=(args.cache/entry['id']).resolve()
        if not source.is_relative_to(args.cache.resolve()):raise ValueError('invalid cache path')
        source.mkdir(parents=True,exist_ok=True)
        for name,info in entry['files'].items():
            if Path(name).name!=name:raise ValueError('invalid source filename')
            path=source/name
            if not path.exists():
                partial=path.with_suffix(path.suffix+'.part')
                try:
                    with urllib.request.urlopen(entry['url']+name,timeout=60) as response,partial.open('wb') as out:
                        total=0
                        while block:=response.read(1048576):
                            total+=len(block)
                            if total>info['bytes']:raise ValueError('source exceeds pinned size')
                            out.write(block)
                    if partial.stat().st_size!=info['bytes'] or digest(partial)!=info['sha256']:raise ValueError('download identity mismatch')
                    partial.replace(path)
                finally:partial.unlink(missing_ok=True)
            if path.stat().st_size!=info['bytes'] or digest(path)!=info['sha256']:raise ValueError('cached source identity mismatch: '+str(path))
        name=entry['id'].replace('/','-');encoded=args.output/(name+'.sfc')
        start=time.monotonic();subprocess.run([encoder,'encode',str(source),str(encoded),'--error',str(entry['error'])],check=True,capture_output=True)
        seconds=time.monotonic()-start
        report=subprocess.run([decoder,'verify',str(encoded),'--reference',str(source)],check=True,capture_output=True,text=True).stdout
        assert 'quality xyz ' in report
        (args.output/(name+'.log')).write_text(report)
        result=dict(id=entry['id'],error=entry['error'],bytes=encoded.stat().st_size,encode_seconds=seconds,quality=report)
        results.append(result);print(next(line for line in report.splitlines() if line.startswith('quality xyz')),flush=True)
    report=dict(encoder_version=version,encoder_sha256=digest(args.exe),decoder_sha256=digest(args.decoder or args.exe),inputs=results)
    (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
