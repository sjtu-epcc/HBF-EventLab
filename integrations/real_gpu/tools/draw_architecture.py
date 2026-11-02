"""Render the implemented release architecture as editable SVG, PDF and PNG."""
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

OUT=Path(__file__).resolve().parents[1]/'docs'
OUT.mkdir(exist_ok=True)
fig,ax=plt.subplots(figsize=(16,9.5))
ax.set(xlim=(-.05,15.65),ylim=(-.15,9.5));ax.axis('off')
blue='#426b99';green='#347b6c';orange='#b97728';red='#a85a61';purple='#76679d'
ink='#1c2935'

def group(x,y,w,h,title,color,right=False):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle='round,pad=0.025,rounding_size=0.04',lw=1,edgecolor=color,facecolor='#fdfdfd',zorder=0))
    ax.text(x+w-.13 if right else x+.13,y+h-.12,title,color=color,weight='bold',fontsize=13.5,va='top',ha='right' if right else 'left')

def box(x,y,w,h,title,sub,color,fill):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle='round,pad=0.02,rounding_size=0.045',lw=1,edgecolor=color,facecolor=fill,zorder=3))
    ax.text(x+w/2,y+h*.65,title,ha='center',va='center',fontsize=11.5,weight='bold',color=ink,zorder=4)
    ax.text(x+w/2,y+h*.25,sub,ha='center',va='center',fontsize=9.1,color=ink,zorder=4)

def edge(points,color=blue,dashed=False,bidir=False):
    style='--' if dashed else '-'
    for a,b in zip(points[:-2],points[1:-1]):ax.plot([a[0],b[0]],[a[1],b[1]],color=color,lw=1.2,ls=style,zorder=2)
    ax.add_patch(FancyArrowPatch(points[-2],points[-1],arrowstyle='<->' if bidir else '-|>',mutation_scale=12,lw=1.2,color=color,linestyle=style,zorder=2))

def label(x,y,text,color=blue):
    ax.text(x,y,text,fontsize=10,color=color,ha='center',va='center',bbox=dict(facecolor='white',edgecolor='none',pad=1),zorder=5)

group(.3,7.65,6.2,1.5,'Host setup / framework adapter',blue)
box(.55,7.9,2.65,.78,'PTX rewriter','validate / intercept selected accesses',blue,'#e8f0fb')
box(3.55,7.9,2.65,.78,'Profile + registry','GGUF shapes / registered ranges',blue,'#e8f0fb')
group(.3,5.35,6.2,1.65,'Real GPU execution',blue)
box(.55,5.62,2.65,.86,'llama.cpp / CUDA','F16 compute / paged KV',blue,'#e8f0fb')
box(3.55,5.62,2.65,.86,'Device protocol','mailbox / grant / dependency wait',blue,'#e8f0fb')

group(9.15,7.65,6.2,1.5,'Host capacity service',green)
box(9.4,7.9,2.65,.78,'Backing store','memory or persistent page file',green,'#e7f2ef')
box(12.4,7.9,2.65,.78,'Prefetcher','bounded following-page reads',green,'#e7f2ef')
group(9.15,5.35,6.2,1.65,'GPU capacity tiers',green)
box(9.4,5.62,2.65,.86,'Native allocation','unselected model tensors',green,'#e7f2ef')
box(12.4,5.62,2.65,.86,'Frame cache + KV','leases / eviction / writeback',green,'#e7f2ef')

group(.3,.85,15.05,3.85,'Host-owned runtime and modeled HBF',orange,right=True)
box(.55,3.04,2.45,1.02,'Live timing owner','ordered GPU + media arrivals',orange,'#fbefdd')
box(3.35,3.04,2.45,1.02,'TimingBackend','Global ranges / block snapshots',orange,'#fbefdd')
box(6.15,3.04,2.45,1.02,'SimulationSession','one clock / causal feedback',orange,'#fbefdd')
box(8.95,3.04,2.45,1.02,'HostDriver','admit / split / retry / replay',orange,'#fbefdd')
box(11.75,3.04,3.3,1.02,'DeviceDriver','UCIe / NAND / bank cache',orange,'#fbefdd')
box(.55,1.15,3.65,.86,'Evidence + validation','accesses / waits / logits / drain',purple,'#eeeaf7')
box(6.15,1.15,4.15,.86,'Environment model','byte energy / temperature / exposure',red,'#f6e9eb')
box(11.75,1.15,3.3,.86,'Retention / refresh','explicit READ + block snapshot',red,'#f6e9eb')

edge([(1.88,7.9),(1.88,6.48)],dashed=True);label(1.88,7.25,'rewrite')
edge([(4.88,7.9),(4.88,6.48)],dashed=True);label(4.88,7.25,'lookup')
edge([(3.2,6.05),(3.55,6.05)])
edge([(6.2,6.05),(9.4,6.05)],green);label(7.8,6.24,'native path',green)
edge([(10.73,7.9),(10.73,7.3),(13.05,7.3),(13.05,6.48)],green);label(11.85,7.31,'page bytes',green)
edge([(14.25,7.9),(14.25,6.48)],green);label(14.5,7.16,'fill',green)
# Both registered GPU requests and CPU media work enter the same timing owner.
edge([(4.88,5.62),(4.88,4.97),(1.78,4.97),(1.78,4.06)],orange)
label(2.55,5.18,'registered requests',orange)
edge([(13.73,5.62),(13.73,4.97),(4.88,4.97)],green)
label(9.15,5.18,'page reads / writeback',green)
ax.plot(4.88,4.97,'o',ms=3.2,color=orange,zorder=4)
for x in [3.0,5.8,8.6,11.4]:edge([(x,3.55),(x+.35,3.55)],orange)
edge([(10.18,3.04),(10.18,2.65),(1.78,2.65),(1.78,3.04)],orange)
label(5.6,2.65,'final Global result -> parent completion',orange)
edge([(1.78,2.65),(1.78,2.01)],purple);label(1.05,2.29,'audit',purple)
edge([(13.4,3.04),(13.4,2.27),(8.23,2.27),(8.23,2.01)],red);label(11.1,2.27,'media counters',red)
edge([(10.3,1.58),(11.75,1.58)],red);label(11.03,1.78,'exposure',red)
edge([(13.4,1.15),(13.4,.48),(.12,.48),(.12,3.55),(.55,3.55)],red)
label(8.1,.48,'explicit maintenance service',red)
ax.text(7.85,.04,'Figure 1: RealGPUAcess_v0 architecture.',ha='center',fontsize=12,fontfamily='DejaVu Serif')
fig.subplots_adjust(left=.01,right=.99,bottom=.02,top=.99)
for ext in ['svg','pdf','png']:fig.savefig(OUT/f'architecture.{ext}',dpi=200,facecolor='white')
plt.close(fig)
