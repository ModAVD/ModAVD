# Copyright 2026 Ji Mao Cai
"""Install and run only the self-contained listener probe on an explicit test device."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

PACKAGE = 'org.modavd.tests.downloadlistener'


def passed_report(report):
    lines = [line.strip() for line in report.splitlines()]
    return ('PASS late-jni listener app probe' in lines and
            'INSTRUMENTATION_CODE: -1' in lines and
            any('selinux=u:r:untrusted_app' in line for line in lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', required=True, type=Path)
    parser.add_argument('--port', required=True, type=int)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--apk', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.port == 5037:
        parser.error('--port must be the test root private port, never 5037')
    if not args.adb.is_file() or not args.apk.is_file():
        parser.error('ADB and APK must already exist')
    output = args.out.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    prefix = [str(args.adb.resolve()), '-P', str(args.port), '-s', args.serial]
    records = []

    def adb(stage, *arguments, timeout=30):
        command = prefix + list(arguments)
        try:
            result = subprocess.run(command, capture_output=True, timeout=timeout)
            stdout, stderr = [value.decode('utf-8', 'replace') for value in (result.stdout, result.stderr)]
            if stage == 'instrument':
                (output / 'instrumentation.txt').write_bytes(result.stdout)
            records.append({'stage': stage, 'command': command, 'returncode': result.returncode,
                            'stdout': stdout, 'stderr': stderr})
        except subprocess.TimeoutExpired as error:
            if stage == 'instrument':
                (output / 'instrumentation.txt').write_bytes(error.stdout or b'')
            records.append({'stage': stage, 'command': command, 'timeoutSeconds': timeout,
                            'stdout': (error.stdout or b'').decode('utf-8', 'replace'),
                            'stderr': (error.stderr or b'').decode('utf-8', 'replace')})
            raise
        finally:
            (output / 'device-log.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
        result.check_returncode()
        return stdout

    print('apkSha256=' + hashlib.sha256(args.apk.read_bytes()).hexdigest(), flush=True)
    try:
        adb('install', 'install', '-r', str(args.apk.resolve()))
        adb('fresh-process', 'shell', 'am', 'force-stop', PACKAGE)
        report = adb('instrument', 'shell', 'am', 'instrument', '-w', '-r',
                     PACKAGE + '/.ProbeInstrumentation', timeout=25)
        print(report, flush=True)
        if not passed_report(report):
            raise RuntimeError('instrumentation did not prove PASS in an untrusted_app context')
    finally:
        # A native watchdog crash/host timeout can leave descendants. Stop only
        # our test package; never change global properties or game processes.
        adb('stop-own-package', 'shell', 'am', 'force-stop', PACKAGE, timeout=10)


if __name__ == '__main__':
    main()
