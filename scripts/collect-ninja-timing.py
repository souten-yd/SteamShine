#!/usr/bin/env python3
"""Summarize Ninja compile and link work without changing the build tree."""

import json
import pathlib
import sys


def classify(output: str) -> str:
    """Return the timing bucket for one Ninja output path."""
    if output.endswith(('.o', '.obj')):
        return 'compile'
    if pathlib.PurePosixPath(output).name in {'sunshine', 'sunshine.exe'}:
        return 'link'
    return 'other'


def main() -> int:
    """Write a deterministic aggregate report from a Ninja .ninja_log file."""
    if len(sys.argv) != 3:
        raise SystemExit(f'Usage: {sys.argv[0]} <.ninja_log> <output.json>')

    log_path = pathlib.Path(sys.argv[1])
    output_path = pathlib.Path(sys.argv[2])
    buckets = {name: {'tasks': 0, 'milliseconds': 0} for name in ('compile', 'link', 'other')}
    for line in log_path.read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'):
            continue
        fields = line.split('\t')
        if len(fields) < 4:
            continue
        try:
            start, end = int(fields[0]), int(fields[1])
        except ValueError:
            continue
        bucket = buckets[classify(fields[3])]
        bucket['tasks'] += 1
        bucket['milliseconds'] += max(0, end - start)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps({'schema': 1, 'source': str(log_path), **buckets}, indent=2) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
