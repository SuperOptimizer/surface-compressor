"""Exercise relocated static/shared installations from independent consumers."""
from pathlib import Path
import os, shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
def run(*args, **kwargs): subprocess.run([str(x) for x in args],check=True,**kwargs)
with tempfile.TemporaryDirectory(prefix='sfc-install-') as temp:
    temp=Path(temp)
    for shared in ('OFF','ON'):
        build=temp/('build-'+shared);prefix=temp/('prefix-'+shared);moved=temp/('moved-'+shared)
        run('cmake','-S',root,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DSFC_TESTS=OFF','-DSFC_TOOLS=OFF',
            '-DBUILD_SHARED_LIBS='+shared,'-DCMAKE_INSTALL_PREFIX='+str(prefix))
        run('cmake','--build',build,'-j2');run('cmake','--install',build)
        prefix.rename(moved)
        consumer=temp/('consumer-'+shared)
        run('cmake','-S',root/'examples/installed','-B',consumer,'-DCMAKE_PREFIX_PATH='+str(moved))
        run('cmake','--build',consumer,'-j2');run('ctest','--test-dir',consumer,'--output-on-failure')
        if not shutil.which('pkg-config'): raise RuntimeError('pkg-config required for install gate')
        pc=next(moved.glob('**/pkgconfig/surfcomp.pc'))
        env=dict(os.environ,PKG_CONFIG_PATH=str(pc.parent))
        flags=subprocess.check_output(['pkg-config','--cflags','--libs','--static','surfcomp'],env=env,text=True)
        import shlex
        exe=temp/('pkg-consumer-'+shared)
        args=shlex.split(os.environ.get('CC','cc'))+[str(root/'examples/installed/roundtrip.c'),'-o',str(exe)]+shlex.split(flags)
        if shared=='ON':args+=['-Wl,-rpath,'+str(pc.parent.parent)]
        run(*args);run(exe)
print('Relocated static/shared CMake and pkg-config consumers passed')
