"""Optional plots of recorded policy-example handoffs, Assembly progress and media work."""
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
    args = parser.parse_args()
    root = args.run.resolve()
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if not manifest.get("complete"):
        parser.error("plot requires a complete, verified example")
    summary = json.loads((root / "summary.json").read_text(encoding="utf-8"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MaxNLocator

    output = root / "plots"
    output.mkdir(exist_ok=True)
    colors = {"A": "#0072B2", "B": "#D55E00"}
    for shape in ("batched", "sparse"):
        fig, axes = plt.subplots(3, 2, figsize=(12, 8), sharex=True, sharey="row", constrained_layout=True)
        for col, policy in enumerate(("fcfs", "fill_open")):
            directory = root / shape / policy
            row = next(r for r in summary if (r["shape"], r["policy"]) == (shape, policy))
            stop = row["simulation_end_ps"] / 1e6
            handoffs = read_csv(directory / "handoffs.csv")
            progress = read_csv(directory / "progress.csv")
            banks = read_csv(directory / "bank.csv")
            for page in ("A", "B"):
                handed = [int(r["handoff"]) / 1e6 for r in handoffs if r["page"] == page]
                axes[0, col].step([0] + handed + [stop], [0] + list(range(1, len(handed)+1)) + [len(handed)],
                                  where="post", color=colors[page], label=f"Page {page}")
                history = [r for r in progress if r["page"] == page]
                axes[1, col].step([int(r["at"])/1e6 for r in history],
                                  [int(r["received_fragments"]) for r in history], where="post",
                                  color=colors[page], label=f"Page {page}")
            for op in banks:
                page = "A" if int(op["block"]) == 0 else "B"
                y = 0 if op["kind"] == "erase" else 1
                axes[2, col].broken_barh([(int(op["issued_at"])/1e6,
                                          (int(op["completed_at"])-int(op["issued_at"]))/1e6)],
                                        (y+.1, .7), facecolors=colors[page], edgecolors="white")
            axes[0, col].set_title(f'{policy}\nA Ready: {row["A_ready_ps"]/1e6:.6f} us; '
                                   f'A programmed: {row["A_programmed_ps"]/1e6:.6f} us', fontsize=10)
            for ax in axes[:2, col]:
                ax.set_ylim(-1, 68)
                ax.yaxis.set_major_locator(MaxNLocator(integer=True))
                ax.legend(loc="lower right", fontsize=8)
                if shape == "batched":
                    for at in (0, 2, 4):
                        ax.axvline(at, linestyle=":", color="#777777", linewidth=.7)
            axes[2, col].set_yticks([.45, 1.45], ["Erase", "Program"])
            axes[2, col].set_ylim(0, 1.9)
            axes[2, col].set_xlabel("Simulated time (us)")
            for ax in axes[:, col]:
                ax.grid(axis="x", alpha=.2)
                ax.set_xlim(-stop*.015, stop*1.02)
        axes[0, 0].set_ylabel("Cumulative handed-off fragments")
        axes[1, 0].set_ylabel("Received fragments\nwithin each context lifetime")
        axes[2, 0].set_ylabel("Bank operation")
        fig.suptitle(f'{shape}: identical trace for both policies; two 4KiB pages in independent blocks\n'
                     '1 channel / 1 Bank; functional model, uncalibrated')
        fig.savefig(output / f"{shape}-policy-comparison.png", dpi=180)
        fig.savefig(output / f"{shape}-policy-comparison.svg")
        plt.close(fig)
    print(output)


if __name__ == "__main__":
    main()
