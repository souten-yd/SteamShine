#!/usr/bin/env bash
# @file scripts/compare-steamos-ci-timings.sh
# @brief Compare two sets of SteamOS CI timing JSON reports without rebuilding.
set -euo pipefail

if [[ "$#" -lt 2 ]]; then
  echo "Usage: $0 <baseline-timings.json>... -- <candidate-timings.json>..." >&2
  exit 2
fi

python3 - "$@" <<'PY'
import json
import statistics
import sys

arguments = sys.argv[1:]
if '--' not in arguments:
    raise SystemExit('Use -- to separate baseline reports from candidate reports.')
separator = arguments.index('--')
baseline, candidate = arguments[:separator], arguments[separator + 1:]
if not baseline or not candidate:
    raise SystemExit('Both baseline and candidate report sets are required.')

def load(paths):
    values = {}
    for path in paths:
        with open(path, encoding='utf-8') as report:
            document = json.load(report)
            jobs = document if isinstance(document, list) else document.get('jobs', [])
            for job in jobs:
                if not job.get('started_at') or not job.get('completed_at'):
                    continue
                for step in job.get('steps', []):
                    if step.get('seconds') is not None:
                        values.setdefault(step['name'], []).append(step['seconds'])
    return values

old, new = load(baseline), load(candidate)
print('step\tbaseline_mean_s\tcandidate_mean_s\tdelta_s\tdelta_percent')
for name in sorted(set(old) | set(new)):
    if name not in old or name not in new:
        print(f'{name}\tmissing\tmissing\tmissing\tmissing')
        continue
    old_mean, new_mean = statistics.mean(old[name]), statistics.mean(new[name])
    delta = new_mean - old_mean
    percent = (delta / old_mean * 100) if old_mean else 0
    print(f'{name}\t{old_mean:.2f}\t{new_mean:.2f}\t{delta:.2f}\t{percent:.1f}%')
PY
