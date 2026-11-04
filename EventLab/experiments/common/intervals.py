"""Half-open intervals; equal-Tick transitions are collapsed, unfinished intervals remain active."""
from collections import Counter

def ratio(n: int | float, d: int | float) -> float | None:
    return n / d if d else None

def interval_curve(intervals: list[tuple[int, int | None]], stop: int) -> tuple[dict, dict]:
    """Half-open occupancy, collapsed at equal Ticks; no fabricated instantaneous peaks."""
    if type(stop) is not int or stop < 0:
        raise ValueError("invalid observation boundary")
    changes = Counter({0: 0, stop: 0})
    area = 0
    for start, finish in intervals:
        end = stop if finish is None else finish
        if type(start) is not int or type(end) is not int or not 0 <= start <= end <= stop:
            raise ValueError("invalid interval or observation boundary")
        changes[start] += 1
        if finish is not None:
            changes[finish] -= 1
        area += end - start
    current, peak, full_ticks, integral = 0, 0, 0, 0
    curve = {}
    last = 0
    for at in sorted(changes):
        integral += current * (at - last)
        if current:
            full_ticks += at - last
        current += changes[at]
        if current < 0:
            raise ValueError("negative occupancy")
        curve[at] = current
        peak = max(peak, current)
        last = at
    if integral != area:
        raise ValueError("occupancy integral mismatch")
    return curve, {"peak_tick_boundary": peak, "area": area, "mean": ratio(area, stop),
                   "busy_ticks": full_ticks, "current": current}
