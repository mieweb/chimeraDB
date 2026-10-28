#!/usr/bin/env python3
"""Build a signed, immutable Debian repository from verified package sets.

No signing keys are generated. The selected primary key must already exist in
GNUPGHOME. Run --help for the publishing interface; see test-repository.py for
server-free tests with disposable keys and actual Debian package archives.
"""

import argparse
import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
DEB = HERE.parent / "deb"
SUITES = {
    "bookworm": ("10.11", "Debian 12 (bookworm)"),
    "trixie": ("11.8", "Debian 13 (trixie)"),
}
KEYRING = "chimeradb-archive-keyring.gpg"
SAFE_NAME = re.compile(r"^[a-zA-Z0-9.+:~_-]+\.deb$")
SAFE_VERSION = re.compile(r"^[0-9][a-zA-Z0-9.+:~_-]*$")


def fail(message):
    raise ValueError(message)


def run(args, **kwargs):
    result = subprocess.run([str(arg) for arg in args], check=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)
    return result.stdout


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def regular(path):
    if not path.is_file() or path.is_symlink():
        fail(f"not a regular file: {path}")
    return path.read_bytes()


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(0o644)


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, indent=2) + "\n").encode()


def package_fields(path):
    fields = run(["dpkg-deb", "-f", path, "Package", "Version", "Architecture"])
    values = {}
    for line in fields.decode().splitlines():
        key, value = line.split(": ", 1)
        values[key.lower()] = value
    if set(values) != {"package", "version", "architecture"}:
        fail(f"missing Debian package identity: {path}")
    return values


def verify_set(directory, suite):
    series, distribution = SUITES[suite]
    for name in ("SHA256SUMS", "build-info.txt"):
        regular(directory / name)
    files = run(["bash", DEB / "verify-packages.sh", directory, series, "amd64",
                 distribution, "--runtime-files"]).decode().splitlines()
    if len(files) != 3:
        fail("validator did not return exactly three runtime packages")
    return [Path(name) for name in files]


def safe_version(version):
    if not isinstance(version, str) or not SAFE_VERSION.fullmatch(version):
        fail(f"unsafe Debian package version: {version!r}")
    return version


def package_record(path, suite):
    data = regular(path)
    fields = package_fields(path)
    safe_version(fields["version"])
    if not SAFE_NAME.fullmatch(path.name):
        fail(f"unsafe Debian archive filename: {path.name!r}")
    return {**fields, "filename": f"pool/{suite}/{fields['version']}/{path.name}",
            "size": len(data), "sha256": sha256(data)}


def release_hashes(data):
    hashes = {}
    in_sha256 = False
    for line in data.decode().splitlines():
        if line == "SHA256:":
            if in_sha256 or hashes:
                fail("duplicate SHA256 section in prior Release")
            in_sha256 = True
        elif line.startswith(" ") and in_sha256:
            parts = line.split()
            if len(parts) != 3:
                fail("invalid SHA256 entry in prior Release")
            digest, size, filename = parts
            if (not re.fullmatch(r"[0-9a-f]{64}", digest)
                    or not size.isdigit() or filename in hashes):
                fail("invalid or duplicate checksum in prior Release")
            hashes[filename] = (digest, int(size))
        elif not line.startswith(" "):
            in_sha256 = False
    expected = {"chimeradb.json", "main/binary-amd64/Packages",
                "main/binary-amd64/Packages.gz"}
    if set(hashes) != expected:
        fail("prior Release does not cover exactly the supported repository indices")
    return hashes


def copy_immutable(source, destination):
    data = regular(source)
    if destination.exists():
        if regular(destination) != data:
            fail(f"attempted to replace immutable repository file: {destination}")
    else:
        write(destination, data)


def validate_record(record, suite):
    if not isinstance(record, dict) or set(record) != {
            "package", "version", "architecture", "filename", "size", "sha256"}:
        fail("invalid prior package record")
    filename = record["filename"]
    if not isinstance(filename, str):
        fail("invalid prior package path")
    path = PurePosixPath(filename)
    safe_version(record["version"])
    if (len(path.parts) != 4 or path.parts[:2] != ("pool", suite)
            or path.parts[2] != record["version"]
            or not SAFE_NAME.fullmatch(path.name)
            or not isinstance(record["sha256"], str)
            or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])
            or type(record["size"]) is not int or record["size"] < 0):
        fail("unsafe prior package path or checksum")


def previous_suite(previous, stage, suite, keyring, scratch):
    directory = previous / "dists" / suite
    release = regular(directory / "Release")
    # Validate both signatures, and insist the clear-signed bytes are exactly
    # the Release validated by the detached signature.
    run(["gpgv", "--homedir", scratch, "--keyring", keyring,
         directory / "Release.gpg", directory / "Release"])
    cleartext = scratch / f"{suite}-Release"
    run(["gpgv", "--homedir", scratch, "--keyring", keyring, "--output", cleartext,
         directory / "InRelease"])
    if cleartext.read_bytes() != release:
        fail("prior InRelease and Release differ")
    for filename, (digest, size) in release_hashes(release).items():
        data = regular(directory / filename)
        if len(data) != size or sha256(data) != digest:
            fail(f"prior signed index has changed: {suite}/{filename}")
    metadata = json.loads(regular(directory / "chimeradb.json"))
    if (not isinstance(metadata, dict) or set(metadata) != {
            "schema", "suite", "architecture", "mariadb_series", "package_sets"}
            or type(metadata["schema"]) is not int or metadata["schema"] != 1 or metadata["suite"] != suite
            or metadata["architecture"] != "amd64"
            or metadata["mariadb_series"] != SUITES[suite][0]
            or not isinstance(metadata["package_sets"], list)
            or not metadata["package_sets"]):
        fail("invalid prior signed repository metadata")
    seen_versions = set()
    for package_set in metadata["package_sets"]:
        if (not isinstance(package_set, dict) or set(package_set) != {
                "version", "source_ref", "build_info", "packages"}
                or not isinstance(package_set["source_ref"], str)
                or not re.fullmatch(r"[0-9a-f]{40}", package_set["source_ref"])
                or not isinstance(package_set["build_info"], str)
                or not isinstance(package_set["packages"], list)
                or len(package_set["packages"]) != 3):
            fail("invalid prior package set")
        version = safe_version(package_set["version"])
        if version in seen_versions:
            fail("duplicate prior package set version")
        seen_versions.add(version)
        with tempfile.TemporaryDirectory(dir=scratch) as verification:
            verification = Path(verification)
            filenames = set()
            checksums = []
            for record in package_set["packages"]:
                validate_record(record, suite)
                path = previous / record["filename"]
                if record != package_record(path, suite) or record["version"] != version:
                    fail(f"prior package bytes or metadata have changed: {path}")
                if path.name in filenames:
                    fail("duplicate prior package filename")
                filenames.add(path.name)
                copy_immutable(path, verification / path.name)
                checksums.append(f"{record['sha256']}  ./{path.name}\n")
            write(verification / "build-info.txt", package_set["build_info"].encode())
            write(verification / "SHA256SUMS", "".join(checksums).encode())
            verify_set(verification, suite)
        for record in package_set["packages"]:
            copy_immutable(previous / record["filename"], stage / record["filename"])
    # Old index hashes remain addressable during an APT update that straddles
    # a Pages deployment. Their names authenticate their bytes independently.
    by_hash = directory / "main/binary-amd64/by-hash/SHA256"
    if by_hash.exists():
        for path in sorted(by_hash.iterdir()):
            data = regular(path)
            if not re.fullmatch(r"[0-9a-f]{64}", path.name) or sha256(data) != path.name:
                fail("invalid retained by-hash index")
            write(stage / path.relative_to(previous), data)
    return metadata


def add_set(stage, metadata, suite, source_ref, directory):
    files = verify_set(directory, suite)
    records = [package_record(path, suite) for path in files]
    version = records[0]["version"]
    old_sets = metadata["package_sets"]
    for old in old_sets:
        comparison = subprocess.run(["dpkg", "--compare-versions", version, "lt", old["version"]],
                                    check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if comparison.returncode not in (0, 1):
            fail(f"cannot compare published package versions in {suite}")
        if comparison.returncode == 0:
            fail(f"version rollback in {suite}: {version} is older than {old['version']}")
        if old["version"] == version:
            old_identities = {(item["package"], item["architecture"]): item for item in old["packages"]}
            for record in records:
                identity = (record["package"], record["architecture"])
                if record != old_identities.get(identity):
                    fail(f"immutable package version changed in {suite}: {record['package']} {version}; bump its revision")
            if source_ref != old["source_ref"]:
                fail(f"published version {version} is already bound to another source commit")
            return
    for source, record in zip(files, records):
        copy_immutable(source, stage / record["filename"])
    old_sets.append({"version": version, "source_ref": source_ref,
                     "build_info": regular(directory / "build-info.txt").decode(),
                     "packages": records})


def sign_suite(stage, suite, metadata, key, timestamp, scratch):
    directory = stage / "dists" / suite
    indices = directory / "main/binary-amd64"
    indices.mkdir(parents=True, exist_ok=True)
    packages = run(["dpkg-scanpackages", "--multiversion", f"pool/{suite}", "/dev/null"], cwd=stage)
    write(indices / "Packages", packages)
    write(indices / "Packages.gz", gzip.compress(packages, mtime=0))
    for name in ("Packages", "Packages.gz"):
        data = regular(indices / name)
        write(indices / "by-hash/SHA256" / sha256(data), data)
    write(directory / "chimeradb.json", json_bytes(metadata))
    date = datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc)
    release = (f"Origin: ChimeraDB\nLabel: ChimeraDB\nSuite: {suite}\nCodename: {suite}\n"
               f"Date: {date.strftime('%a, %d %b %Y %H:%M:%S +0000')}\n"
               "Architectures: amd64\nComponents: main\nAcquire-By-Hash: yes\n"
               f"Description: ChimeraDB for Debian {suite}\nSHA256:\n")
    for name in ("chimeradb.json", "main/binary-amd64/Packages", "main/binary-amd64/Packages.gz"):
        data = regular(directory / name)
        release += f" {sha256(data)} {len(data):16d} {name}\n"
    write(directory / "Release", release.encode())
    common = ["gpg", "--no-options", "--batch", "--no-tty", "--yes", "--local-user", key,
              "--digest-algo", "SHA256"]
    run([*common, "--clearsign", "--output", directory / "InRelease", directory / "Release"])
    run([*common, "--no-armor", "--detach-sign", "--output", directory / "Release.gpg", directory / "Release"])
    # Signing success alone is insufficient: verify the exact public key users
    # will receive can authenticate both published signatures.
    run(["gpgv", "--homedir", scratch, "--keyring", stage / KEYRING,
         directory / "InRelease"])
    run(["gpgv", "--homedir", scratch, "--keyring", stage / KEYRING,
         directory / "Release.gpg", directory / "Release"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-set", action="append", required=True, metavar="SUITE=DIRECTORY",
                        help="repeat for bookworm/10.11 and trixie/11.8; complete amd64 build output")
    parser.add_argument("--source-ref", required=True, help="full source commit SHA of all incoming packages")
    parser.add_argument("--signing-key", required=True, help="full 40-character primary GPG fingerprint")
    parser.add_argument("--output", required=True, type=Path, help="new output directory (must not exist)")
    parser.add_argument("--previous", type=Path, help="previous signed repository; retain and verify every published package")
    parser.add_argument("--timestamp", type=int, default=None, help="Release date, as Unix seconds (defaults to now)")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-fA-F]{40}", args.source_ref):
        fail("--source-ref must be a full source commit SHA")
    args.source_ref = args.source_ref.lower()
    args.signing_key = args.signing_key.upper()
    if not re.fullmatch(r"[0-9A-F]{40}", args.signing_key):
        fail("--signing-key must be the full primary key fingerprint")
    inputs = {}
    for entry in args.package_set:
        suite, separator, name = entry.partition("=")
        if not separator or suite not in SUITES or suite in inputs or not name:
            fail("each --package-set must specify a supported, unique SUITE=DIRECTORY")
        directory = Path(name).resolve()
        if not directory.is_dir():
            fail(f"missing package set directory: {directory}")
        inputs[suite] = directory
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        fail("--output must not exist; publish only after this command succeeds")
    for executable in ("bash", "dpkg", "dpkg-deb", "dpkg-scanpackages", "sha256sum", "gpg", "gpgv"):
        if not shutil.which(executable):
            fail(f"required executable is missing: {executable}")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=output.parent))
    stage.chmod(0o755)
    try:
        with tempfile.TemporaryDirectory(prefix="chimeradb-apt-") as scratch:
            scratch = Path(scratch)
            public = run(["gpg", "--no-options", "--batch", "--no-armor", "--export-options", "export-minimal",
                          "--export", args.signing_key])
            write(stage / KEYRING, public)
            run(["bash", DEB / "verify-keyring.sh", stage / KEYRING, args.signing_key])
            suites = {}
            if args.previous:
                previous = args.previous.resolve()
                if not previous.is_dir():
                    fail("previous repository does not exist")
                # Never follow a retained file/directory symlink outside the
                # artifact being authenticated (including a symlinked dists).
                for root, directories, files in os.walk(previous):
                    if any((Path(root) / name).is_symlink() for name in directories + files):
                        fail("previous repository contains a symlink")
                prior_dists = previous / "dists"
                if not prior_dists.is_dir():
                    fail("previous repository has no dists directory")
                for path in sorted(prior_dists.iterdir()):
                    if path.name not in SUITES or not path.is_dir():
                        fail(f"unsupported prior distribution: {path.name}")
                    suites[path.name] = previous_suite(previous, stage, path.name,
                                                       stage / KEYRING, scratch)
                if not suites:
                    fail("previous repository contains no distributions")
            for suite, directory in inputs.items():
                metadata = suites.setdefault(suite, {"schema": 1, "suite": suite,
                    "architecture": "amd64", "mariadb_series": SUITES[suite][0], "package_sets": []})
                add_set(stage, metadata, suite, args.source_ref, directory)
            timestamp = args.timestamp
            if timestamp is None:
                timestamp = int(datetime.datetime.now(datetime.timezone.utc).timestamp())
            if timestamp < 0:
                fail("--timestamp cannot be negative")
            for suite, metadata in sorted(suites.items()):
                sign_suite(stage, suite, metadata, args.signing_key, timestamp, scratch)
            write(stage / ".nojekyll", b"")
            # Publishing must not inherit a maintainer's restrictive umask or
            # GPG's signature-file mode: web servers and APT need public read
            # access throughout this tree. Only exported public material is
            # ever staged here; the secret key stays in GNUPGHOME.
            for path in stage.rglob("*"):
                path.chmod(0o755 if path.is_dir() else 0o644)
            os.rename(stage, output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print(f"Signed APT repository: {output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"repository generation failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors="replace").rstrip(), file=sys.stderr)
        sys.exit(1)
