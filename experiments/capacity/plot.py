"""Optional static plots (requires matplotlib); analysis itself has no third-party dependency."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import sys
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from experiments.common.config import load_run_configuration
from experiments.common.model import FRAGMENTS, PAGE_BYTES, FRAGMENT_BYTES
from experiments.common.io import read_csv



def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--case", help="also plot resources.csv for this case name")
    args = parser.parse_args()
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.colors import Normalize
    import numpy as np

    root = args.run.resolve()
    rows = json.loads((root / "summary.json").read_text(encoding="utf-8"))
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    configs = {}
    for row in rows:
        if row["analysis_status"] != "ok":
            continue
        cfg = load_run_configuration(root / "cases" / row["case"])
        for field, key in (("mocs_selected", "device.selected.mocs_selected"),
                           ("pending_limit", "device.capabilities.max_pending_write_dlus"),
                           ("program_ps", "execution.media_timing.program_ps"),
                           ("open_dlus", "host.max_open_dlus_per_channel")):
            if row[field] != cfg.integer(key):
                raise ValueError("summary differs from effective configuration: " + field)
        configs[row["case"]] = cfg
    output = root / "plots"
    output.mkdir(exist_ok=True)
    plan = manifest["plan"]
    for order in plan["orders"]:
        for pending in plan["pending_limits"]:
            for program in plan["program_ps"]:
                subset = [r for r in rows if (r["order"], r["pending_limit"], r["program_ps"]) == (order, pending, program)]
                if not subset:
                    continue
                xs, ys = sorted(plan["mocs_selected"]), sorted(plan["open_dlus"])
                fig, axes = plt.subplots(1, 2, figsize=(10, max(3.5, len(ys) * .45 + 1.5)), constrained_layout=True)
                for ax, key, title in zip(axes, ("dlu_success_ratio", "timeout_context_ratio"),
                                          ("Successful original DLUs / planned DLUs", "Timed-out contexts / opened contexts")):
                    values = np.full((len(ys), len(xs)), np.nan)
                    lookup = {(r["open_dlus"], r["mocs_selected"]): r for r in subset}
                    for yi, y in enumerate(ys):
                        for xi, x in enumerate(xs):
                            r = lookup.get((y, x))
                            if r and r["analysis_status"] == "ok" and r["stop_reason"] == "Completed" and r.get(key) is not None:
                                values[yi, xi] = r[key]
                                ax.text(xi, yi, f'{r[key]:.2f}', ha="center", va="center", color="black" if r[key] > .45 else "white")
                            else:
                                ax.text(xi, yi, "N/A", ha="center", va="center")
                    cmap = plt.get_cmap("viridis").copy()
                    cmap.set_bad("#dddddd")
                    im = ax.imshow(np.ma.masked_invalid(values), cmap=cmap, norm=Normalize(0, 1), aspect="auto", origin="lower")
                    ax.set_xticks(range(len(xs)), xs)
                    ax.set_yticks(range(len(ys)), ys)
                    ax.set_xlabel("Selected MOCS")
                    ax.set_ylabel("Host open-DLU cap (not observed Pending)")
                    ax.set_title(title, fontsize=10)
                    fig.colorbar(im, ax=ax, shrink=.8)
                fig.suptitle(f'{order}; Pending={pending}; program={program / 1e6:g} us\nSynthetic, uncalibrated; gray = missing / unfinished / harness error')
                fig.savefig(output / f"{order}-p{pending}-t{program}.png", dpi=160)
                plt.close(fig)
    if args.case:
        matches = [r for r in rows if r["case"] == args.case and r["analysis_status"] == "ok"]
        if len(matches) != 1:
            parser.error("case must identify one successfully analyzed result")
        row = matches[0]
        with (root / "cases" / args.case / "resources.csv").open(encoding="utf-8", newline="") as source:
            points = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(source)]
        x = [p["at_ps"] / 1e6 for p in points]
        fig, axes = plt.subplots(3, 1, figsize=(11, 7), sharex=True, constrained_layout=True)
        axes[0].step(x, [p["mocs"] for p in points], where="post", label="MOCS occupied")
        axes[0].axhline(configs[args.case].selected_mocs, color="red", linestyle="--", label="Selected MOCS")
        for field in ("total", "collecting", "ready_waiting", "programming"):
            axes[1].step(x, [p[field] for p in points], where="post", label=field)
        axes[1].axhline(configs[args.case].pending_limit, color="red", linestyle="--", label="Pending limit")
        axes[2].step(x, [p["bank_busy"] for p in points], where="post", label="Bank busy: erase + program")
        for ax in axes:
            ax.legend(loc="upper right", fontsize=8)
            ax.grid(alpha=.2)
        axes[0].set_ylabel("Commands")
        axes[1].set_ylabel("DLUs")
        axes[2].set_ylabel("Busy (0/1)")
        axes[2].set_yticks([0, 1])
        axes[2].set_xlabel("Simulated time (us)")
        fig.suptitle(args.case + "\nFunctional model, uncalibrated")
        fig.savefig(output / f"{args.case}-resources.png", dpi=160)
        plt.close(fig)
    print(output)


if __name__ == "__main__":
    main()
