#!/usr/bin/env python3
"""Build the self-contained SteamShine frontend asset tree and manifest."""

import argparse
import hashlib
import json
import shutil
from pathlib import Path


def digest(path: Path) -> str:
    """Return the SHA-256 digest of one generated asset."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    """Copy the source frontend into its generated delivery tree."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    required = ("index.html", "app.css", "app.js")
    for name in required:
        if not (source / name).is_file():
            raise SystemExit(f"missing SteamShine frontend source: {source / name}")
    if output.exists():
        shutil.rmtree(output)
    shutil.copytree(source, output)
    files = {
        name: {"sha256": digest(output / name), "bytes": (output / name).stat().st_size}
        for name in required
    }
    (output / "manifest.json").write_text(json.dumps({"version": 1, "files": files}, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
