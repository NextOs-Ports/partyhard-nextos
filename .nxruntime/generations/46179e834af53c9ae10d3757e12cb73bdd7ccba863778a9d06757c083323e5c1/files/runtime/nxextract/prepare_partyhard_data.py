#!/usr/bin/env python3
"""Prepare NXExtract-authenticated Party Hard GO owner data.

NXExtract authenticates the ARM64 payload before this hook runs.  The hook
unbundles the Play Asset Delivery pack `assets/bin/Data/datapack.unity3d`
into the seven loose serialized files the Unity engine already looks for
(`resources.assets`, `.resS`, `sharedassets1/2`, `level1`, `level2`), byte for
byte as they are stored inside the container, and then removes the container
from the stage.  Nothing is reserialized and nothing is written outside
NXExtract's disposable stage.  No APK, game asset or binary delta is
distributed.
"""

from __future__ import print_function

import argparse
import json
import os
import stat
import sys


FORMAT = 1
PACKAGE = "com.tinybuild.PartyHardGO"
PREPARATION = "partyhard-datapack-loose-layout-v1"
MARKER = ".partyhard-data.json"
DATAPACK = "assets/bin/Data/datapack.unity3d"
DATA_DIR = "assets/bin/Data"
LZ4_RELATIVE = "nxextract/lib/aarch64/liblz4.so.1"

# The seven loose files of the approved Mali-450 port, exactly as the
# container stores them (sizes).  Their content identity is closed by the
# recipe checkpoint (exact files/bytes and the CRC tree fingerprint of the
# staged assets/bin tree), never by a hash literal in hook logic; a different
# container content is refused before a byte is published.
EXPECTED_ENTRIES = {
    "level1": 111236,
    "level2": 69124,
    "resources.assets": 40255596,
    "resources.assets.resS": 247103760,
    "sharedassets1.assets": 161676,
    "sharedassets1.assets.resS": 156016,
    "sharedassets2.assets": 1101,
}


class PreparationError(RuntimeError):
    pass


def fail(message):
    raise PreparationError("Party Hard GO preparation failed: " + message)


def regular_file(path):
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def safe_relative(value, label):
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        fail("invalid %s" % label)
    parts = value.split("/")
    if value.startswith("/") or any(part in ("", ".", "..") for part in parts):
        fail("unsafe %s %r" % (label, value))
    return value


def inside(root, path):
    root = os.path.realpath(root)
    path = os.path.realpath(path)
    return path == root or path.startswith(root + os.sep)


def stage_path(stage, relative):
    relative = safe_relative(relative, "stage path")
    path = os.path.abspath(os.path.join(stage, *relative.split("/")))
    if not inside(stage, path):
        fail("stage path escaped its root")
    return path


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_bytes(path, payload):
    temporary = "%s.nxpart.%d" % (path, os.getpid())
    if os.path.lexists(temporary):
        fail("stale or unsafe temporary output")
    try:
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def validate_owner_input(stage):
    assets = stage_path(stage, "assets")
    if not os.path.isdir(assets) or os.path.islink(assets):
        fail("NXExtract-authenticated owner asset tree is missing or unsafe")
    library_root = stage_path(stage, "lib")
    if not os.path.isdir(library_root) or os.path.islink(library_root):
        fail("ARM64 library directory is missing or unsafe")
    actual_names = sorted(os.listdir(library_root))
    expected_names = ["libil2cpp.so", "libmain.so", "libunity.so"]
    if actual_names != expected_names:
        fail("NXExtract-authenticated ARM64 library set changed")
    for name in expected_names:
        path = os.path.join(library_root, name)
        if not regular_file(path) or os.path.islink(path):
            fail("NXExtract-authenticated owner library is missing or unsafe: " + name)
    datapack = stage_path(stage, DATAPACK)
    if not regular_file(datapack) or os.path.islink(datapack):
        fail("owner asset pack datapack.unity3d is missing or unsafe")
    for name in ("data.unity3d", "boot.config", "unity_app_guid",
                 "Managed/Metadata/global-metadata.dat"):
        path = stage_path(stage, DATA_DIR + "/" + name)
        if not regular_file(path) or os.path.islink(path):
            fail("owner Unity data is missing or unsafe: " + name)


def reject_stale_python_cache(game_dir):
    root = os.path.join(game_dir, "nxextract")
    if not os.path.isdir(root) or os.path.islink(root):
        fail("packaged Python root is missing or unsafe")
    for current, directories, files in os.walk(root, followlinks=False):
        for name in directories:
            path = os.path.join(current, name)
            if name == "__pycache__":
                fail("stale Python bytecode cache is forbidden")
            if os.path.islink(path):
                fail("packaged Python path contains a symbolic link")
        for name in files:
            if name.endswith(".pyc"):
                fail("stale Python bytecode is forbidden")


def safe_packaged_file(game_dir, relative, label):
    relative = safe_relative(relative, label + " path")
    cursor = game_dir
    parts = relative.split("/")
    for index, part in enumerate(parts):
        cursor = os.path.join(cursor, part)
        try:
            mode = os.lstat(cursor).st_mode
        except OSError:
            fail("%s is missing or unsafe" % label)
        if stat.S_ISLNK(mode):
            fail("%s path contains a symbolic link" % label)
        if index < len(parts) - 1 and not stat.S_ISDIR(mode):
            fail("%s parent is not a directory" % label)
    if not regular_file(cursor) or os.path.islink(cursor):
        fail("%s is missing or unsafe" % label)
    if not inside(game_dir, cursor):
        fail("%s must be supplied under the port directory" % label)
    return os.path.realpath(cursor)


def configure_python(game_dir):
    reject_stale_python_cache(game_dir)
    vendor = os.path.join(game_dir, "nxextract", "vendor", "python")
    if not os.path.isdir(vendor) or os.path.islink(vendor):
        fail("vendored Python runtime is missing or unsafe")
    lz4_path = safe_packaged_file(game_dir, LZ4_RELATIVE, "packaged LZ4 runtime")
    sys.path.insert(0, vendor)
    # The vendored ctypes LZ4 shim looks at this variable first and falls back
    # to a system liblz4 (host-side recipe checks) when the packaged AArch64
    # object cannot be loaded on the current machine.
    os.environ["PF2_LZ4_LIBRARY"] = lz4_path
    try:
        import UnityPy
    except Exception as error:
        fail("could not load the pinned Unity preparation runtime: %s" % error)
    return UnityPy


def progress(done, total, text):
    print("NXEXTRACT_PROGRESS %d %d %s" % (done, total, text))
    sys.stdout.flush()


def unbundle(stage, unitypy):
    datapack = stage_path(stage, DATAPACK)
    try:
        environment = unitypy.load(datapack)
    except Exception as error:
        fail("cannot open datapack.unity3d: %s" % error)
    files = list(environment.files.values())
    if len(files) != 1:
        fail("datapack.unity3d is not a single UnityFS container")
    bundle = files[0]
    entries = getattr(bundle, "files", None)
    if not entries:
        fail("datapack.unity3d has no inner files")
    names = sorted(entries)
    if names != sorted(EXPECTED_ENTRIES):
        fail("datapack.unity3d inner file set differs from the approved build: %r" % names)
    total = len(names)
    written = 0
    for index, name in enumerate(names, 1):
        progress(index - 1, total, "PREPARANDO %d/%d" % (index, total))
        entry = entries[name]
        reader = getattr(entry, "reader", entry)
        try:
            payload = bytes(reader.bytes)
        except Exception as error:
            fail("cannot read inner file %s: %s" % (name, error))
        expected_size = EXPECTED_ENTRIES[name]
        if len(payload) != expected_size:
            fail("inner file %s has %d bytes, expected %d (different build)"
                 % (name, len(payload), expected_size))
        target = stage_path(stage, DATA_DIR + "/" + name)
        if os.path.lexists(target) and (os.path.islink(target) or not regular_file(target)):
            fail("unsafe existing output %s" % name)
        atomic_bytes(target, payload)
        written += len(payload)
    progress(total, total, "PREPARANDO %d/%d" % (total, total))
    # The container itself never reaches the device: the engine reads the loose
    # layout and the pack would only duplicate 50 MB on the card.
    os.unlink(datapack)
    return total, written


def write_marker(stage, files, written):
    document = {
        "format": FORMAT,
        "inputs": "nxextract-authenticated",
        "loose_files": files,
        "loose_bytes": written,
        "package": PACKAGE,
        "preparation": PREPARATION,
    }
    payload = (json.dumps(document, ensure_ascii=True, sort_keys=True,
                          separators=(",", ":")) + "\n").encode("ascii")
    atomic_bytes(os.path.join(stage, MARKER), payload)


def prepare(stage, game_dir):
    unitypy = configure_python(game_dir)
    print("[prepare] checking NXExtract-authenticated ARM64 inputs")
    validate_owner_input(stage)
    print("[prepare] unbundling the asset pack into the loose Unity layout")
    files, written = unbundle(stage, unitypy)
    for name in EXPECTED_ENTRIES:
        path = stage_path(stage, DATA_DIR + "/" + name)
        if not regular_file(path) or os.path.islink(path):
            fail("loose asset is missing after preparation: " + name)
        if os.path.getsize(path) != EXPECTED_ENTRIES[name]:
            fail("loose asset size changed after preparation: " + name)
    write_marker(stage, files, written)
    print("[prepare] Party Hard GO owner data ready: %d loose files, %d bytes"
          % (files, written))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--game-dir", required=True)
    args = parser.parse_args(argv)
    if os.path.islink(args.stage) or not os.path.isdir(args.stage):
        fail("NXExtract stage is missing or unsafe")
    if os.path.islink(args.game_dir) or not os.path.isdir(args.game_dir):
        fail("port directory is missing or unsafe")
    prepare(os.path.realpath(args.stage), os.path.realpath(args.game_dir))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PreparationError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
