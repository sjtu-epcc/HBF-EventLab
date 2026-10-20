"""Optional scientific plots from recorded CSV/JSON; requires matplotlib."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def read_csv(path: Path) -> list[dict]:
    with path.open(encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--case", action="append", default=[], help="repeat to add per-channel resources and release/assembly timelines")
    args = parser.parse_args()
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from matplotlib.ticker import MaxNLocator
    import numpy as np

    root = args.run.resolve()
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    rows = json.loads((root / "summary.json").read_text(encoding="utf-8"))
    plan = manifest["plan"]
    output = root / "plots"
    output.mkdir(exist_ok=True)

    def save(fig, name):
        fig.savefig(output / (name + ".png"), dpi=180)
        fig.savefig(output / (name + ".svg"))
        plt.close(fig)

    xs, ys = sorted(plan["delta_ps"]), sorted(plan["interleave_bytes"])
    for size in plan["sizes_bytes"]:
        lookup = {(r["interleave_bytes"], r["delta_ps"]): r for r in rows if r["size_bytes"] == size}
        fig, axes = plt.subplots(2, 2, figsize=(13, 8), constrained_layout=True)
        metrics = [("dlu_success_ratio", "Successful original DLUs / planned DLUs", 1),
                   ("timeout_context_ratio", "Timed-out contexts / opened contexts", 1),
                   ("mean_bank_utilization", "Mean Bank utilization over [0, stop]", 1),
                   ("first_ready_ps", "First DLU Ready time (us)", 1e-6)]
        for ax, (key, title, scale) in zip(axes.flat, metrics):
            values = np.full((len(ys), len(xs)), np.nan)
            for iy, interleave in enumerate(ys):
                for ix, delta in enumerate(xs):
                    r = lookup.get((interleave, delta))
                    if r and r["analysis_status"] == "ok" and r["stop_reason"] == "Completed" and r.get(key) is not None:
                        values[iy, ix] = r[key] * scale
            cmap = plt.get_cmap("viridis").copy()
            cmap.set_bad("#dddddd")
            vmax = max(.001, float(np.nanmax(values))) if key == "first_ready_ps" and np.isfinite(values).any() else 1
            im = ax.imshow(np.ma.masked_invalid(values), origin="lower", aspect="auto", cmap=cmap, vmin=0, vmax=vmax)
            for iy in range(len(ys)):
                for ix in range(len(xs)):
                    v = values[iy, ix]
                    ax.text(ix, iy, f"{v:.2f}" if np.isfinite(v) else "N/A", ha="center", va="center",
                            fontsize=8, color="black" if not np.isfinite(v) or v > vmax*.5 else "white")
            ax.set_xticks(range(len(xs)), [f"{x/1000:g}" for x in xs])
            ax.set_yticks(range(len(ys)), ys)
            ax.set_xlabel("Global 64B release interval (ns)")
            ax.set_ylabel("Interleave (bytes)")
            ax.set_title(title, fontsize=10)
            fig.colorbar(im, ax=ax, shrink=.8)
        fig.suptitle(f'{size//1024} KiB; {plan["channels"]} channels; FCFS; MOCS=1024; Pending=16\n'
                     'Functional, uncalibrated. N/A = absent milestone, unfinished run, or missing/error record.')
        save(fig, f"n{size}-overview")

    fig, ax = plt.subplots(figsize=(9, 5), constrained_layout=True)
    for interleave in ys:
        spans = [(64*plan["channels"]-(plan["channels"]-1)*(interleave//64)-1)*d/1e6 for d in xs]
        ax.plot([d/1000 for d in xs], spans, marker="o", label=f"{interleave} B")
    ax.axhline(plan["assembly_timeout_ps"]/1e6, color="red", linestyle="--", label="Assembly deadline duration")
    ax.set_xlabel("Global 64B release interval (ns)")
    ax.set_ylabel("First-to-last planned release of one local DLU (us)")
    ax.set_title("Analytical input span (verified against each mapped footprint)\nNot an actual assembly duration or a sufficient timeout test")
    ax.legend()
    ax.grid(alpha=.2)
    save(fig, "analytical-release-span")

    for name in args.case:
        matching = [r for r in rows if r["case"] == name and r["analysis_status"] == "ok"]
        if len(matching) != 1:
            parser.error(f"case must identify one analyzed run: {name}")
        row, path = matching[0], root / "cases" / name
        points = [{k: int(v) for k, v in r.items()} for r in read_csv(path / "resources.csv")]
        fig, axes = plt.subplots(4, plan["channels"], figsize=(max(7, plan["channels"]*4), 10),
                                 sharex=True, sharey="row", squeeze=False, constrained_layout=True)
        for ch in range(plan["channels"]):
            p = [r for r in points if r["channel"] == ch]
            t = [r["at_ps"]/1e6 for r in p]
            axes[0, ch].step(t, [r["mocs"] for r in p], where="post", label="Occupied")
            axes[0, ch].axhline(1024, color="red", linestyle="--", label="Selected MOCS")
            axes[0, ch].set_title(f"Channel {ch}")
            for field in ("total", "collecting", "ready_waiting", "programming"):
                axes[1, ch].step(t, [r[field] for r in p], where="post", label=field)
            axes[1, ch].axhline(16, color="red", linestyle="--", label="Pending limit")
            axes[2, ch].step(t, [r["host_queued"] for r in p], where="post", label="Released, not handed off")
            axes[2, ch].set_ylim(0, max(1, max(r["host_queued"] for r in points))*1.05)
            axes[2, ch].yaxis.set_major_locator(MaxNLocator(integer=True))
            axes[3, ch].step(t, [r["bank_busy"] for r in p], where="post", label="Erase + program")
            axes[3, ch].set_yticks([0, 1])
            axes[3, ch].set_xlabel("Simulated time (us)")
            for ax in axes[:, ch]:
                ax.grid(alpha=.2)
        for ax, ylabel in zip(axes[:, 0], ("Commands", "DLUs", "Host queued fragments", "Bank busy (0/1)")):
            ax.set_ylabel(ylabel)
            ax.legend(loc="upper right", fontsize=7)
        fig.suptitle(f'{name}: {row["successful_dlus"]}/{row["planned_dlus"]} successful DLUs\nFunctional model, uncalibrated')
        save(fig, name + "-resources")

        dlus, contexts = read_csv(path / "dlus.csv"), read_csv(path / "contexts.csv")
        fig, ax = plt.subplots(figsize=(12, max(5, len(dlus)*.27+2)), constrained_layout=True)
        for index, d in enumerate(dlus):
            lo, hi = int(d["planned_first_release"])/1e6, int(d["planned_last_release"])/1e6
            ax.plot([lo, hi], [index+.14]*2, color="#777777", linewidth=4, marker="|", markersize=7)
            for context in contexts:
                if (context["channel"], context["local_dlu"]) != (d["channel"], d["local_dlu"]):
                    continue
                end = context["ready_at"] or context["retired_at"] or row["simulation_end_ps"]
                color = "#228833" if context["ready_at"] else "#cc3311" if context["termination"] == "assembly_timeout" else "#eeaa33"
                ax.plot([int(context["opened_at"])/1e6, int(end)/1e6], [index-.14]*2, color=color, linewidth=4)
        ax.set_yticks(range(len(dlus)), [f'CH{d["channel"]} / DLU{d["local_dlu"]}' for d in dlus], fontsize=8)
        ax.invert_yaxis()
        ax.set_xlabel("Simulated time (us)")
        ax.set_title(name + "\nPlanned input availability vs. observed collecting interval (not program time)")
        ax.grid(axis="x", alpha=.2)
        ax.legend(handles=[Line2D([0], [0], color=c, lw=4, label=l) for c, l in
                           [("#777777", "Planned first-to-last release"), ("#228833", "Opened to Ready"),
                            ("#cc3311", "Opened to assembly timeout"), ("#eeaa33", "Other/unfinished collecting")]],
                  loc="upper center", bbox_to_anchor=(.5, -.22), ncol=2, fontsize=8)
        save(fig, name + "-release-assembly")
    print(output)


if __name__ == "__main__":
    main()
