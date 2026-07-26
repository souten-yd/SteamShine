#!/usr/bin/env python3
"""Validate generated Sunshine Web UI assets before they are packaged."""

import argparse
import json
import re
import sys
from html.parser import HTMLParser
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


ROUTE_PATHS = {
    "",
    "/",
    "apps",
    "clients",
    "config",
    "featured",
    "logout",
    "password",
    "pin",
    "troubleshooting",
    "welcome",
}


class LocalReferenceParser(HTMLParser):
    """Collect local src and href attribute values from an HTML document."""

    def __init__(self):
        super().__init__()
        self.references = []

    def handle_starttag(self, tag, attrs):
        for name, value in attrs:
            if name in {"href", "src"} and value:
                self.references.append(value)


def fail(message):
    """Print a validation failure and record it for the process exit status."""
    print(f"web asset validation: {message}", file=sys.stderr)
    return 1


def is_local_reference(value):
    """Return whether a reference must resolve within the delivered asset tree."""
    parsed = urlsplit(value)
    return bool(value) and not parsed.scheme and not parsed.netloc and not value.startswith("#") and not value.startswith("//")


def resolve_reference(asset_root, page, value):
    """Resolve a local HTML reference and reject paths escaping the asset root."""
    path = PurePosixPath(unquote(urlsplit(value).path))
    target = asset_root / path.relative_to("/") if path.is_absolute() else page.parent / path
    try:
        target.resolve().relative_to(asset_root.resolve())
    except ValueError:
        return None
    return target


def is_server_route(value):
    """Return whether a reference resolves through a known Web UI page route."""
    path = urlsplit(value).path.lstrip("./").rstrip("/")
    return path in ROUTE_PATHS


def validate_steamshine_assets(asset_root):
    """Validate the generated SteamShine entry page, bundles, and manifest."""
    errors = 0
    required = ("index.html", "app.css", "app.js", "manifest.json")
    for name in required:
        if not (asset_root / name).is_file():
            errors += fail(f"missing SteamShine asset: {name}")
    if errors:
        return errors
    content = (asset_root / "index.html").read_text(encoding="utf-8")
    if re.search(r"<%-|\{\{\s*\$t\(", content):
        errors += fail("unresolved template marker in SteamShine index.html")
    parser = LocalReferenceParser()
    parser.feed(content)
    for reference in parser.references:
        if is_local_reference(reference) and reference.startswith("/steamshine/"):
            target = asset_root / PurePosixPath(urlsplit(reference).path).name
            if not target.is_file():
                errors += fail(f"unresolved SteamShine reference: {reference}")
    try:
        manifest = json.loads((asset_root / "manifest.json").read_text(encoding="utf-8"))
        files = manifest["files"]
        for name in ("index.html", "app.css", "app.js"):
            if name not in files or not (asset_root / name).is_file():
                errors += fail(f"SteamShine manifest is missing {name}")
    except (KeyError, OSError, json.JSONDecodeError) as exc:
        errors += fail(f"cannot parse SteamShine asset manifest: {exc}")
    return errors


def main():
    """Validate required generated files, references, and template expansion."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asset_root", type=Path)
    parser.add_argument("--steamshine-root", type=Path, help="Validate the generated SteamShine asset tree.")
    parser.add_argument("--report", type=Path, help="Write a non-secret JSON validation report.")
    args = parser.parse_args()
    asset_root = args.asset_root.resolve()
    errors = 0

    if not asset_root.is_dir():
        return fail(f"asset root does not exist: {asset_root}")

    steamshine_root = args.steamshine_root.resolve() if args.steamshine_root else None
    if steamshine_root:
        if not steamshine_root.is_dir():
            return fail(f"SteamShine asset root does not exist: {steamshine_root}")
        errors += validate_steamshine_assets(steamshine_root)

    indexes = sorted(asset_root.glob("*.html"))
    if not indexes:
        errors += fail("no generated HTML entry pages found")
    if not (asset_root / "index.html").is_file():
        errors += fail("missing index.html")
    if not list(asset_root.rglob("*.js")):
        errors += fail("no JavaScript bundles found")
    if not list(asset_root.rglob("*.css")):
        errors += fail("no CSS bundles found")
    if not (asset_root / "assets/locale/en.json").is_file():
        errors += fail("missing English locale asset")

    manifest = asset_root / ".vite/manifest.json"
    if not manifest.is_file():
        errors += fail("missing Vite asset manifest")
    else:
        try:
            entries = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            errors += fail(f"cannot parse Vite asset manifest: {exc}")
        else:
            for entry in entries.values():
                for file_name in [entry.get("file"), *entry.get("css", [])]:
                    if file_name and not (asset_root / file_name).is_file():
                        errors += fail(f"manifest references missing file: {file_name}")

    unresolved = re.compile(r"<%-")
    for page in indexes:
        content = page.read_text(encoding="utf-8")
        if unresolved.search(content):
            errors += fail(f"unresolved template marker in {page.relative_to(asset_root)}")
        parser = LocalReferenceParser()
        parser.feed(content)
        for reference in parser.references:
            if is_local_reference(reference):
                if is_server_route(reference):
                    continue
                target = resolve_reference(asset_root, page, reference)
                if target is None or not target.is_file():
                    errors += fail(f"unresolved local reference in {page.relative_to(asset_root)}: {reference}")

    if errors:
        return 1
    if args.report:
        args.report.write_text(json.dumps({
            "asset_root": str(asset_root),
            "entry_pages": [str(page.relative_to(asset_root)) for page in indexes],
            "manifest": str(manifest.relative_to(asset_root)),
            "locale": "assets/locale/en.json",
            "unresolved_template_markers": False,
            "steamshine_asset_root": str(steamshine_root) if steamshine_root else None,
        }, indent=2) + "\n", encoding="utf-8")
    print(f"web asset validation passed: {asset_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
