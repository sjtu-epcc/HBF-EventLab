"""Plot observed scheduling/window timelines; requires matplotlib."""
import argparse
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[2]
if __package__ in (None, ""):
    sys.path.insert(0,str(ROOT))
from experiments.read_scheduling.workload import START, cases
from experiments.common.config import load_run_configuration
from experiments.read_scheduling.analyze import analyze
from experiments.read_scheduling.run import verify_comparisons


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("input",type=Path)
    args=parser.parse_args();root=args.input.resolve()
    manifest=json.loads((root/"manifest.json").read_text())
    if not manifest.get("complete"):
        parser.error("only completed, validated runs can be plotted")
    rows=json.loads((root/"summary.json").read_text());verify_comparisons(rows,root)
    data={}; configs={}
    for c in cases():
        data[c["name"]]=json.loads((root/c["name"]/"statistics.json").read_text())
        configs[c["name"]] = load_run_configuration(root/c["name"])
        analyze(data[c["name"]],c,config=configs[c["name"]])
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch
    dest=root/"figures";dest.mkdir(exist_ok=True)
    colors={"erase":"#7b7b7b","program":"#e68613","sense":"#1976b5","window":"#9e77c2"}
    us=lambda t:(t-START)/1e6
    def save(fig,name):
        for ext in ("png","svg"):
            fig.savefig(dest/(name+"."+ext),dpi=170,bbox_inches="tight")
        plt.close(fig)
    def window_rows(ax,d):
        paths=d["read_paths"];attempts={a["id"]:a for a in d["attempts"]}
        banks={b["ticket"]:b for b in d["bank_operations"]}
        for i,p in enumerate(paths):
            if p["window_entered_at"] is not None:
                ax.broken_barh([(us(p["window_entered_at"]),(p["window_left_at"]-p["window_entered_at"])/1e6)],
                               (i-.23,.23),facecolors=colors["window"])
            if p["bank_ticket"] is not None:
                b=banks[p["bank_ticket"]]
                ax.broken_barh([(us(b["issued_at"]),(b["completed_at"]-b["issued_at"])/1e6)],(i,.23),facecolors=colors["sense"])
            else:
                ax.plot(us(p["service_at"]),i+.1,"D",color="#218844",markersize=5)
            ax.plot(us(attempts[p["attempt"]]["delivered"]),i,"|",color="black",markersize=13)
        ax.set_yticks(range(len(paths)),[f"R{i+1}" for i in range(len(paths))]);ax.set_ylim(len(paths)-.5,-.5)
        ax.grid(axis="x",alpha=.2);ax.set_xlabel("Simulated time after measurement start (us)")
    fig,axes=plt.subplots(2,1,figsize=(10,6),sharex=True)
    for ax,policy in zip(axes,("fcfs","read_priority")):
        d=data[f"competition-w50000-{policy}"]
        for b in d["bank_operations"]:
            if b["issued_at"]<START:continue
            y={"erase":0,"program":1,"sense":2}[b["kind"]]
            ax.broken_barh([(us(b["issued_at"]),(b["completed_at"]-b["issued_at"])/1e6)],(y-.3,.6),facecolors=colors[b["kind"]])
        p=d["read_paths"][0];a=next(a for a in d["attempts"] if a["id"]==p["attempt"])
        ax.axvline(us(p["window_left_at"]),ls="--",color=colors["window"],label="Read window released")
        ax.axvline(us(a["delivered"]),ls=":",color="black",label="Read response delivered")
        ax.set_yticks([0,1,2],["Erase W","Program W","Sense R"]);ax.invert_yaxis();ax.grid(axis="x",alpha=.2)
        ax.set_title(f"{policy}: read accepted-to-response {(a['delivered']-a['accepted'])/1e6:.6f} us")
    axes[0].legend(loc="upper left",fontsize=8);axes[-1].set_xlabel("Simulated time after measurement start (us)")
    fig.suptitle("Same trace, same Bank; window = 50 ns\nFunctional model, uncalibrated");fig.tight_layout();save(fig,"contention")

    by_name={r["name"]:r for r in rows};fig,axes=plt.subplots(1,2,figsize=(11,4))
    for k,policy in enumerate(("fcfs","read_priority")):
        selected=[by_name[f"competition-w{w}-{policy}"] for w in (0,50000,1000000)]
        x=[i+(k-.5)*.35 for i in range(3)]
        axes[0].bar(x,[r["read_max_end_to_end_ps"]/1e6 for r in selected],width=.35,label=policy)
        axes[1].bar(x,[(r["program_finished_ps"]-START)/1e6 for r in selected],width=.35,label=policy)
    for ax in axes:
        ax.set_xticks(range(3),[f'{configs[f"competition-w{w}-fcfs"].integer("execution.read_dispatch.wait_ps")/1000:g}' for w in (0,50000,1000000)]);ax.set_xlabel("Read window wait (ns)");ax.grid(axis="y",alpha=.2)
    axes[0].set_ylabel("Read end-to-end latency (us)");axes[1].set_ylabel("W program finish (us)\nafter measurement start")
    axes[0].legend();fig.suptitle("An unreleased read cannot win arbitration\nFunctional model, uncalibrated");fig.tight_layout();save(fig,"window_gating")

    fig,axes=plt.subplots(2,2,figsize=(12,7),sharex=True)
    for ax,name in zip(axes.flat,("timer","threshold","tail","single")):
        d=data[name];window_rows(ax,d);b=d["read_window_releases"][0]
        ax.set_title(f"{name}: {len(b['members'])} reads, {b['reason']} at {us(b['at']):.6f} us")
    legend=[Patch(color=colors["window"],label="Waiting in read window"),Patch(color=colors["sense"],label="Core sense")]
    fig.legend(handles=legend,loc="lower center",ncol=2);fig.suptitle("200 ns window; quantity trigger versus timer, including final singleton\nBlack tick: response delivered. Functional model, uncalibrated")
    fig.tight_layout(rect=(0,.06,1,.94));save(fig,"window_triggers")

    fig,ax=plt.subplots(figsize=(10,4));window_rows(ax,data["cache-removal"])
    ax.set_title("Same-page reads: R3 leaves its window on a Cache hit, without window release\nGreen diamond: Cache service; black tick: response. Functional model, uncalibrated")
    fig.legend(handles=legend,loc="lower center",ncol=2);fig.tight_layout(rect=(0,.12,1,1));save(fig,"cache_removal")
    print(dest)


if __name__=="__main__":main()
