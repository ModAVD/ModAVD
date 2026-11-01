# Copyright 2026 Ji Mao Cai
"""Build the standalone ARM64 fixture offline; never run ADB or a device."""

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
    parser.add_argument('--ndk', required=True, type=Path, help='NDK r27c root')
    parser.add_argument('--java-home', required=True, type=Path, help='JDK 21 root')
    parser.add_argument('--android-jar', required=True, type=Path, help='API 34 android.jar')
    parser.add_argument('--build-tools', required=True, type=Path, help='SDK build-tools 35 directory')
    parser.add_argument('--out', required=True, type=Path, help='dedicated output, e.g. guest_fork/out')
    parser.add_argument('--ndk-host', help='optional prebuilt directory tag, e.g. linux-x86_64')
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    output = args.out.expanduser().resolve()
    if output == source or output in source.parents:
        parser.error('--out must be a separate output directory, not the source or its ancestor')
    suffix = '.exe' if os.name == 'nt' else ''
    host = args.ndk_host or {
        'win32': 'windows-x86_64', 'linux': 'linux-x86_64', 'darwin': 'darwin-x86_64'
    }.get(sys.platform)
    if not host or Path(host).name != host or '/' in host or '\\' in host:
        parser.error('pass --ndk-host with one NDK prebuilt directory tag')
    ndk_bin = args.ndk.expanduser().resolve() / 'toolchains/llvm/prebuilt' / host / 'bin'
    java_bin = args.java_home.expanduser().resolve() / 'bin'
    tools = args.build_tools.expanduser().resolve()
    android_jar = args.android_jar.expanduser().resolve()
    compiler = ndk_bin / ('clang++' + suffix)
    readelf = ndk_bin / ('llvm-readelf' + suffix)
    java = java_bin / ('java' + suffix)
    javac = java_bin / ('javac' + suffix)
    keytool = java_bin / ('keytool' + suffix)
    aapt = tools / ('aapt2' + suffix)
    align = tools / ('zipalign' + suffix)
    d8 = tools / 'lib/d8.jar'
    signer = tools / 'lib/apksigner.jar'
    for path in [compiler, readelf, java, javac, keytool, aapt, align, d8, signer, android_jar]:
        if not path.is_file():
            parser.error('missing tool/input: ' + str(path))
    output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment['JAVA_HOME'] = str(java_bin.parent)
    environment['PATH'] = str(java_bin) + os.pathsep + environment.get('PATH', '')
    records = []

    def run(arguments):
        argv = [str(value) for value in arguments]
        try:
            result = subprocess.run(argv, cwd=source, env=environment,
                                    capture_output=True, timeout=90)
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

    # A fresh staging directory prevents stale Java inner classes or dex entries.
    # Only this newly-created directory is removed on exit, never prior outputs.
    with tempfile.TemporaryDirectory(prefix='.stage-', dir=output) as temporary:
        stage = Path(temporary).resolve()
        assert output in stage.parents
        classes, dex = stage / 'classes', stage / 'dex'
        classes.mkdir()
        dex.mkdir()
        library = stage / 'libguest_fork_probe.so'
        run([compiler, '--target=aarch64-linux-android30', '-std=c++17', '-O2',
             '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared', '-fno-exceptions',
             '-fno-rtti', '-static-libstdc++', source / 'guest_fork_probe.cc',
             '-llog', '-ldl', '-o', library])
        run([javac, '-source', '8', '-target', '8', '-bootclasspath', android_jar,
             '-d', classes, source / 'ProbeActivity.java'])
        run([java, '-cp', d8, 'com.android.tools.r8.D8', '--min-api', '23',
             '--lib', android_jar, '--output', dex, *sorted(classes.rglob('*.class'))])
        base = stage / 'base.apk'
        run([aapt, 'link', '-I', android_jar, '--manifest', source / 'AndroidManifest.xml',
             '--min-sdk-version', '23', '--target-sdk-version', '34', '-o', base])
        with zipfile.ZipFile(base, 'a') as archive:
            archive.write(dex / 'classes.dex', 'classes.dex')
            archive.write(library, 'lib/arm64-v8a/libguest_fork_probe.so')
        aligned = stage / 'aligned.apk'
        run([align, '-f', '4', base, aligned])
        keystore = output / 'probe-debug.keystore'
        if not keystore.exists():
            run([keytool, '-genkeypair', '-noprompt', '-keystore', keystore,
                 '-alias', 'androiddebugkey', '-storepass', 'android', '-keypass', 'android',
                 '-dname', 'CN=Local Guest Fork Test', '-keyalg', 'RSA', '-keysize', '2048',
                 '-validity', '3650'])
        apk = stage / 'guest-fork-probe.apk'
        run([java, '-jar', signer, 'sign', '--ks', keystore, '--ks-key-alias', 'androiddebugkey',
             '--ks-pass', 'pass:android', '--key-pass', 'pass:android', '--out', apk, aligned])
        run([java, '-jar', signer, 'verify', '--verbose', apk])
        run([readelf, '-h', library])
        run([readelf, '--dyn-syms', library])
        header = library.read_bytes()[:64]
        if header[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 183:
            raise RuntimeError('fixture library is not little-endian ELF64 AArch64')
        with zipfile.ZipFile(apk) as archive:
            if [name for name in archive.namelist() if name.startswith('lib/')] != [
                    'lib/arm64-v8a/libguest_fork_probe.so']:
                raise RuntimeError('APK must contain only the ARM64 fixture library')
        manifest = {
            'package': 'org.modavd.tests.guestfork', 'abi': 'arm64-v8a',
            'apkSha256': sha256(apk), 'librarySha256': sha256(library),
            'sourceSha256': {name: sha256(source / name) for name in
                             ['guest_fork_probe.cc', 'ProbeActivity.java', 'AndroidManifest.xml', 'build.py']},
            'ndkHost': host, 'elfMachine': 'AArch64', 'fixturesOnly': True,
            'deviceOperations': False, 'gameFilesModified': False,
        }
        for artifact in [library, apk]:
            shutil.copyfile(artifact, output / artifact.name)
        idsig = apk.with_suffix('.apk.idsig')
        if idsig.exists():
            shutil.copyfile(idsig, output / idsig.name)
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2), flush=True)


if __name__ == '__main__':
    main()
