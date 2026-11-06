# Copyright 2026 Ji Mao Cai
"""Build the offline x86_64 instrumentation APK; never connect to a device."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', required=True, type=Path)
    parser.add_argument('--java-home', required=True, type=Path)
    parser.add_argument('--android-jar', required=True, type=Path,
                        help='existing API 34 or newer android.jar')
    parser.add_argument('--build-tools', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--ndk-host', help='one NDK prebuilt directory tag')
    parser.add_argument('--keystore', type=Path, help='reuse an existing local probe debug signing key')
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    bridge = source.parent.parent
    output = args.out.expanduser().resolve()
    if output == source or output in source.parents:
        parser.error('--out must be separate from the source and its ancestors')
    suffix = '.exe' if os.name == 'nt' else ''
    host = args.ndk_host or {'win32': 'windows-x86_64', 'linux': 'linux-x86_64',
                            'darwin': 'darwin-x86_64'}.get(sys.platform)
    if not host or Path(host).name != host or '/' in host or '\\' in host:
        parser.error('invalid --ndk-host')
    ndk_bin = args.ndk.expanduser().resolve() / 'toolchains/llvm/prebuilt' / host / 'bin'
    java_bin = args.java_home.expanduser().resolve() / 'bin'
    tools = args.build_tools.expanduser().resolve()
    android_jar = args.android_jar.expanduser().resolve()
    compiler, readelf = ndk_bin / ('clang++' + suffix), ndk_bin / ('llvm-readelf' + suffix)
    java, javac, keytool = [java_bin / (name + suffix) for name in ('java', 'javac', 'keytool')]
    aapt, align = tools / ('aapt2' + suffix), tools / ('zipalign' + suffix)
    d8, signer = tools / 'lib/d8.jar', tools / 'lib/apksigner.jar'
    listener = bridge / 'companion/jni/download_listener.cc'
    for path in [compiler, readelf, java, javac, keytool, aapt, align, d8, signer, android_jar,
                 listener, listener.with_suffix('.h')]:
        if not path.is_file():
            parser.error('missing tool/input: ' + str(path))
    output.mkdir(parents=True, exist_ok=True)
    keystore = args.keystore.expanduser().resolve() if args.keystore else output / 'probe-debug.keystore'
    if args.keystore and not keystore.is_file():
        parser.error('--keystore must point to an existing probe debug key')
    environment = dict(os.environ)
    environment['JAVA_HOME'] = str(java_bin.parent)
    environment['PATH'] = str(java_bin) + os.pathsep + environment.get('PATH', '')
    records = []

    def run(arguments):
        argv = [str(value) for value in arguments]
        try:
            result = subprocess.run(argv, cwd=source, env=environment, capture_output=True, timeout=90)
            records.append({'args': argv, 'returncode': result.returncode,
                            'stdout': result.stdout.decode('utf-8', 'replace'),
                            'stderr': result.stderr.decode('utf-8', 'replace')})
        except subprocess.TimeoutExpired as error:
            records.append({'args': argv, 'timeoutSeconds': 90,
                            'stdout': (error.stdout or b'').decode('utf-8', 'replace'),
                            'stderr': (error.stderr or b'').decode('utf-8', 'replace')})
            raise
        finally:
            (output / 'build-log.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
        result.check_returncode()

    with tempfile.TemporaryDirectory(prefix='.stage-', dir=output) as temporary:
        stage = Path(temporary).resolve()
        assert output in stage.parents
        classes, dex = stage / 'classes', stage / 'dex'
        classes.mkdir()
        dex.mkdir()
        library = stage / 'libdownload_listener_probe.so'
        run([compiler, '--target=x86_64-linux-android30', '-std=c++17', '-O2',
             '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared', '-fno-exceptions',
             '-fno-rtti', '-static-libstdc++', source / 'listener_app_probe.cc', listener,
             '-Wl,--build-id=sha1', '-o', library])
        run([javac, '-source', '8', '-target', '8', '-bootclasspath', android_jar,
             '-d', classes, source / 'ProbeInstrumentation.java'])
        run([java, '-cp', d8, 'com.android.tools.r8.D8', '--min-api', '30',
             '--lib', android_jar, '--output', dex, *sorted(classes.rglob('*.class'))])
        base = stage / 'base.apk'
        run([aapt, 'link', '-I', android_jar, '--manifest', source / 'AndroidManifest.xml',
             '--min-sdk-version', '30', '--target-sdk-version', '35', '-o', base])
        with zipfile.ZipFile(base, 'a', compression=zipfile.ZIP_DEFLATED) as archive:
            archive.write(dex / 'classes.dex', 'classes.dex')
            archive.write(library, 'lib/x86_64/libdownload_listener_probe.so')
        aligned = stage / 'aligned.apk'
        run([align, '-f', '4', base, aligned])
        if not keystore.exists():
            run([keytool, '-genkeypair', '-noprompt', '-keystore', keystore,
                 '-alias', 'androiddebugkey', '-storepass', 'android', '-keypass', 'android',
                 '-dname', 'CN=Local Download Listener App Test', '-keyalg', 'RSA',
                 '-keysize', '2048', '-validity', '3650'])
        apk = stage / 'download-listener-app.apk'
        run([java, '-jar', signer, 'sign', '--ks', keystore, '--ks-key-alias', 'androiddebugkey',
             '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', apk, aligned])
        run([java, '-jar', signer, 'verify', '--verbose', '--print-certs', apk])
        run([aapt, 'dump', 'badging', apk])
        run([readelf, '-h', library])
        run([readelf, '--dyn-syms', library])
        header = library.read_bytes()[:64]
        if header[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 62:
            raise RuntimeError('probe must be little-endian ELF64 x86_64')
        with zipfile.ZipFile(apk) as archive:
            if [name for name in archive.namelist() if name.startswith('lib/')] != [
                    'lib/x86_64/libdownload_listener_probe.so']:
                raise RuntimeError('APK must contain only the x86_64 probe library')
        sources = {str(path.relative_to(bridge)).replace('\\', '/'): sha256(path)
                   for path in [source / 'listener_app_probe.cc', source / 'ProbeInstrumentation.java',
                                source / 'AndroidManifest.xml', source / 'build.py', source / 'run.py',
                                listener, listener.with_suffix('.h')]}
        manifest = {'package': 'org.modavd.tests.downloadlistener', 'abi': 'x86_64',
                    'runner': '.ProbeInstrumentation', 'minSdk': 30, 'targetSdk': 35,
                    'apkSha256': sha256(apk), 'librarySha256': sha256(library),
                    'sourceSha256': sources, 'ndkHost': host, 'elfMachine': 'x86_64',
                    'lateJniOnly': True, 'deviceOperations': False, 'gameFilesModified': False,
                    'reusedSigningKey': bool(args.keystore)}
        for artifact in [library, apk]:
            shutil.copyfile(artifact, output / artifact.name)
        idsig = apk.with_suffix('.apk.idsig')
        if idsig.exists():
            shutil.copyfile(idsig, output / idsig.name)
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2), flush=True)


if __name__ == '__main__':
    main()
