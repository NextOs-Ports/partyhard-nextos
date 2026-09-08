#!/usr/bin/env bash
# Package a built, pinned and committed candidate exactly once. No device I/O.
set -euo pipefail
PORT_DIR=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
: "${NEXTOS_FRAMEWORK_REPO:?set NEXTOS_FRAMEWORK_REPO to the pinned framework Git repository}"
[[ $# == 1 ]] || { printf 'Usage: %s NEW_OUTPUT_DIRECTORY\n' "$0" >&2; exit 2; }
printf 'Party Hard GO candidate: checking frozen source and framework pins\n'
python3 - "$PORT_DIR" "$NEXTOS_FRAMEWORK_REPO" "$1" <<'PY'
from pathlib import Path
import hashlib
import json
import subprocess
import sys

port, repository, destination = map(lambda p: Path(p).resolve(), sys.argv[1:])
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
destination.mkdir(parents=True, exist_ok=False)
identity = {'schema': 'partyhard-test-candidate/1', 'port_commit': commit,
            'executable_sha256': binary_hash, 'framework_pin_sha256': sha(port / 'FRAMEWORK-PIN.json'),
            'physical_validation': 'pending', 'publication': 'test-candidate-only'}
(destination / 'ATTEMPT.json').write_text(json.dumps(identity, indent=2) + '\n')
snapshot = destination / 'framework'
run('python3', repository / 'framework/nxgenerator/framework_pin.py', 'materialize',
    '--repository', repository, '--pin', port / 'FRAMEWORK-PIN.json', '--destination', snapshot)
framework = snapshot / 'framework'
generated = destination / 'generated'
run('python3', framework / 'nxgenerator/nxgenerator.py', port / 'nxproject.json',
    '--source-root', port, '--output', generated)
run('python3', framework / 'nxrelease/nx-render-manifest.py', '--generator-root', generated,
    '--framework-root', framework, '--source-url', 'https://github.com/NextOs-Ports/partyhard-nextos',
    '--source-date-epoch', git('show', '-s', '--format=%ct', 'HEAD'), '--max-glibc', '2.30')
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
