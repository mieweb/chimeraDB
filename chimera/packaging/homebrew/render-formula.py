#!/usr/bin/env python3
"""Render source-only tap formulae using the published source archive's checksum."""
import argparse
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--url", required=True, help="versioned source archive (file:// also works for local staging)")
parser.add_argument("--sha256", required=True)
parser.add_argument("--output", type=Path, required=True, help="tap Formula directory")
args = parser.parse_args()
if not re.fullmatch(r"[a-fA-F0-9]{64}", args.sha256):
    parser.error("--sha256 must be a complete SHA-256 checksum")
here = Path(__file__).resolve().parent
version = (here.parent.parent / "VERSION").read_text().strip()
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
