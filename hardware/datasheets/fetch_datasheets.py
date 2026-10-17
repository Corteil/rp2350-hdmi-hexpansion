#!/usr/bin/env python3
"""Download every datasheet listed in parts.json into this folder.

Run from anywhere:  python fetch_datasheets.py
Options:            --dry-run   list what would be fetched, download nothing
                    --force     re-download files that already exist

Each part is tried against its URLs in order (LCSC first, then the
manufacturer fallback if one is listed). A download only counts if the
response is really a PDF (starts with %PDF), so an HTML error page is
never saved as a datasheet. Standard library only.
"""
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
UA = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")


def fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "application/pdf,*/*"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def main():
    dry = "--dry-run" in sys.argv
    force = "--force" in sys.argv
    parts = json.loads((HERE / "parts.json").read_text(encoding="utf-8"))
    ok, skipped, failed = 0, 0, []
    for p in parts:
        dest = HERE / p["file"]
        label = f'{p["mpn"]} ({",".join(p["refs"][:3])})'
        if dest.exists() and not force:
            skipped += 1
            print(f"skip   {label}: already have {dest.name}")
            continue
        if dry:
            print(f"would  {label}: {p['urls'][0]} -> {dest.name}")
            continue
        for url in p["urls"]:
            try:
                data = fetch(url)
            except (urllib.error.URLError, TimeoutError, OSError) as e:
                print(f"  try  {url}: {e}")
                continue
            if not data.startswith(b"%PDF"):
                print(f"  try  {url}: not a PDF (got {len(data)} bytes)")
                continue
            dest.write_bytes(data)
            ok += 1
            print(f"ok     {label}: {dest.name} ({len(data) // 1024} KB)")
            break
        else:
            failed.append(label)
            print(f"FAILED {label}")
    if not dry:
        print(f"\n{ok} downloaded, {skipped} already present, {len(failed)} failed")
        if failed:
            print("Failed parts: download manually from the LCSC/manufacturer page and save with the name in parts.json:")
            for f in failed:
                print("  -", f)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
