#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Gate each cache-switch capture against the independent encoded CF oracle."""
import json
import pathlib
import sys

from reference import capture_path, difference, expected, observe


def main():
    folder = pathlib.Path(sys.argv[1])
    sequence = sys.argv[2]
    if sequence not in ("rows-first", "ized-first"):
        raise SystemExit("Use rows-first or ized-first")
    modes = ("rows", "ized", "rows") if sequence == "rows-first" else ("ized", "rows", "ized")
    results = {}
    passed = True
    for step, mode in enumerate(modes, 1):
        panels = [(f"rows-step{step}", "rows-left")] if mode == "rows" else [
            (f"pair-ized-step{step}", "encoded-cf-back"),
            (f"pair-ized-step{step}-second", "encoded-cf-front"),
        ]
        for tag, formula in panels:
            if not capture_path(folder, tag).exists():
                results[tag] = {"missing_capture": True}
                passed = False
                continue
            actual = observe(folder, tag)
            prediction = expected(formula)
            prediction.save(folder / (tag + "-reference.png"))
            result = difference(actual, prediction)
            swapped = difference(actual, expected(formula, True))
            result["swapped_reference"] = swapped
            # CF back is (L+R)/2 and cannot detect an eye exchange. Its front
            # companion and every rows capture must reject the swapped oracle.
            result["swap_invariant"] = formula == "encoded-cf-back"
            swap_count = 0 if result["swap_invariant"] else 640 * 480
            result["passed"] = (result.get("different_pixels") == 0
                                and swapped.get("different_pixels") == swap_count)
            passed &= result["passed"]
            results[tag] = result
    report = {"sequence": sequence, "passed": passed, "captures": results}
    (folder / "comparisons.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
