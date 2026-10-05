#!/usr/bin/env python3
"""Render source-only tap formulae using the published source archive's checksum."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--url", required=True, help="versioned source archive (file:// also works for local staging)")
parser.add_argument("--sha256", required=True)
parser.add_argument("--source-archive", type=Path, required=True,
                    help="local copy of the source archive; checksum and embedded version are verified")
parser.add_argument("--output", type=Path, required=True, help="tap Formula directory")
args = parser.parse_args()
if not re.fullmatch(r"[a-fA-F0-9]{64}", args.sha256):
    parser.error("--sha256 must be a complete SHA-256 checksum")
here = Path(__file__).resolve().parent
try:
    with args.source_archive.open("rb") as source:
        digest = hashlib.sha256()
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
        if digest.hexdigest() != args.sha256.lower():
            raise ValueError("source archive does not match --sha256")
        source.seek(0)
        with tarfile.open(fileobj=source, mode="r:*") as archive:
            versions = [member for member in archive.getmembers()
                        if re.fullmatch(r"[^/]+/chimera/VERSION", member.name)]
            if len(versions) != 1 or not versions[0].isfile() or not 0 < versions[0].size <= 128:
                raise ValueError("source archive must contain exactly one regular chimera/VERSION file")
            version = archive.extractfile(versions[0]).read().decode("utf-8").strip()
            if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?", version):
                raise ValueError("invalid ChimeraDB version in source archive")
            if versions[0].name != f"chimeradb-{version}/chimera/VERSION":
                raise ValueError("source archive prefix does not match its ChimeraDB version")
except (OSError, tarfile.TarError, UnicodeDecodeError, ValueError) as error:
    parser.error(str(error))
template = (here / "formula.rb.in").read_text()
args.output.mkdir(parents=True, exist_ok=True)
for series, name, klass, conflict in (
    ("11.8", "chimeradb", "Chimeradb", "chimeradb@10.11"),
    ("10.11", "chimeradb@10.11", "ChimeradbAT1011", "chimeradb"),
):
    text = template
    for key, value in {
        "CLASS": klass, "NAME": name, "SERVER": series, "URL": json.dumps(args.url),
        "VERSION": json.dumps(version), "SHA256": json.dumps(args.sha256.lower()),
        "CONFLICT": json.dumps(conflict),
    }.items():
        text = text.replace(f"@{key}@", value)
    destination = args.output / f"{name}.rb"
    destination.write_text(text)
    print(destination)
