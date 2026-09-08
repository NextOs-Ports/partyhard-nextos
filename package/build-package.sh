#!/usr/bin/env bash
# Package a built, pinned and committed candidate exactly once. No device I/O.
set -euo pipefail
PORT_DIR=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
: "${NEXTOS_FRAMEWORK_REPO:?set NEXTOS_FRAMEWORK_REPO to the pinned framework Git repository}"
[[ $# == 2 && ( $1 == prepare || $1 == bundle ) ]] || {
  printf 'Usage: %s prepare|bundle OUTPUT_DIRECTORY\n' "$0" >&2; exit 2;
}
printf 'Party Hard GO candidate: checking frozen source and framework pins\n'
python3 - "$PORT_DIR" "$NEXTOS_FRAMEWORK_REPO" "$1" "$2" <<'PY'
from pathlib import Path
import hashlib
import json
import subprocess
import sys

port, repository = map(lambda p: Path(p).resolve(), sys.argv[1:3])
phase, destination = sys.argv[3], Path(sys.argv[4]).resolve()
def run(*args):
    print('+', args[0], Path(args[1]).name if len(args) > 1 else '', flush=True)
    subprocess.run(list(map(str, args)), check=True)
def git(*args):
    return subprocess.check_output(['git', '-C', str(port), *args], text=True).strip()
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

if git('status', '--porcelain', '--untracked-files=no'):
    raise SystemExit('Commit all candidate changes before packaging')
commit = git('rev-parse', 'HEAD')
binary_hash = sha(port / 'partyhard-nextos')
metadata = json.loads((port / 'RELEASE-METADATA.json').read_text())
if metadata['runtime']['executable_sha256'] != binary_hash:
    raise SystemExit('Runtime differs from the frozen metadata')
identity = {'schema': 'partyhard-test-candidate/1', 'port_commit': commit,
            'executable_sha256': binary_hash, 'framework_pin_sha256': sha(port / 'FRAMEWORK-PIN.json'),
            'physical_validation': 'pending', 'publication': 'test-candidate-only',
            'src_tree': git('rev-parse', 'HEAD:src'),
            'vendor_tree': git('rev-parse', 'HEAD:vendor'),
            'build_recipe_sha256': sha(port / 'build_universal.sh')}
snapshot = destination / 'framework'
framework = snapshot / 'framework'
generated = destination / 'generated'
if phase == 'prepare':
    destination.mkdir(parents=True, exist_ok=False)
    (destination / 'ATTEMPT.json').write_text(json.dumps(identity, indent=2) + '\n')
    run('python3', repository / 'framework/nxgenerator/framework_pin.py', 'materialize',
        '--repository', repository, '--pin', port / 'FRAMEWORK-PIN.json', '--destination', snapshot)
    # V5's component snapshot does not include its separate APK contract.
    # Materialize that unchanged host tool from explicitly pinned Git blobs.
    tools_pin = json.loads((port / 'package/BUILD-TOOLS-PIN.json').read_text())
    for entry in tools_pin['files']:
        relative = Path(entry['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise SystemExit('Unsafe build-tool path')
        data = subprocess.check_output(['git', '-C', str(repository), 'show',
                                        tools_pin['commit'] + ':' + relative.as_posix()])
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise SystemExit('Build-tool pin mismatch: ' + str(relative))
        target = snapshot / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as stream:
            stream.write(data)
        target.chmod(0o644)
    run('python3', framework / 'nxgenerator/nxgenerator.py', port / 'nxproject.json',
        '--source-root', port, '--output', generated)
    run('python3', framework / 'nxrelease/nx-render-manifest.py', '--generator-root', generated,
        '--framework-root', framework, '--source-url', 'https://github.com/NextOs-Ports/partyhard-nextos',
        '--source-date-epoch', git('show', '-s', '--format=%ct', 'HEAD'), '--max-glibc', '2.30')
    print('Prepared without ZIP. Commit the generated deployment files, then run bundle.', flush=True)
    raise SystemExit(0)
prepared = json.loads((destination / 'ATTEMPT.json').read_text())
for key in ('executable_sha256', 'framework_pin_sha256', 'src_tree',
            'vendor_tree', 'build_recipe_sha256'):
    if prepared[key] != identity[key]:
        raise SystemExit('Prepared candidate changed: ' + key)
# The committed flat repository and generated package must carry the same
# deployment bytes before the single release gate starts.
for member in generated.rglob('*'):
    if not member.is_file():
        continue
    relative = member.relative_to(generated)
    if relative.parts[0] == 'partyhard':
        relative = Path(*relative.parts[1:])
    if not (port / relative).is_file() or sha(port / relative) != sha(member):
        raise SystemExit('Commit the generated deployment file: ' + str(relative))
attempts = port / 'build/package-attempts'
attempts.mkdir(parents=True, exist_ok=True)
with (attempts / (commit + '.json')).open('x') as stream:
    json.dump({'commit': commit, 'destination': str(destination)}, stream)
identity['prepared_from_commit'] = prepared['port_commit']
# The canonical bundle performs validate -> stage -> verify-stage -> one ZIP
# creation/reopen. Do not run another preflight or rebuild the frozen ELF.
run('python3', framework / 'nxrelease/nxrelease.py', 'bundle', '--authority', 'human',
    '--manifest', generated / 'nxrelease.json', '--stage', destination / 'stage',
    '--destination', destination / 'release', '--archive-name', 'partyhard.zip',
    '--max-glibc', '2.30')
identity['zip_sha256'] = sha(destination / 'release/partyhard.zip')
(destination / 'CANDIDATE.json').write_text(json.dumps(identity, indent=2) + '\n')
print('Candidate complete:', destination / 'release/partyhard.zip', flush=True)
print('SHA-256:', identity['zip_sha256'], flush=True)
PY
