"""Join successful Host choices to Device acceptance and SenseOrder subscription intervals."""


class ReadOrder:
    def __init__(self):
        self.choices = {}
        self.accepted = {}
        self.pages = {}
        self.open = {}
        self.intervals = []

    def consume(self, kind, fields):
        if kind == "host-read-option":
            tick, attempt, channel, selected, unit, parent, address = map(int, fields)
            choice = self.choices.setdefault(attempt, dict(at=tick, channel=channel,
                                             selected=selected, options=[]))
            choice["options"].append(dict(unit=unit, parent=parent, local_byte=address))
        elif kind == "order-page":
            tick, attempt, *page = map(int, fields)
            if len(page) != 5 or attempt in self.pages:
                raise ValueError("invalid physical service page")
            self.pages[attempt] = dict(at=tick, page=page)
        elif kind == "order-accepted":
            tick, attempt, sequence = map(int, fields)
            if attempt in self.accepted:
                raise ValueError("duplicate acceptance")
            self.accepted[attempt] = (tick, sequence)
        elif kind == "order-wait-begin":
            tick, attempt, prior, *pages = map(int, fields)
            if len(pages) != 10 or pages[:3] != pages[5:8] or attempt in self.open:
                raise ValueError("invalid SenseOrder subscription")
            self.open[attempt] = dict(start=tick, attempt=attempt, prior=prior,
                                      page=pages[:5], prior_page=pages[5:])
        elif kind == "order-wait-end":
            tick, attempt = map(int, fields)
            span = self.open.pop(attempt)
            if tick < span["start"]:
                raise ValueError("negative SenseOrder wait")
            self.intervals.append(dict(span, end=tick, duration=tick-span["start"]))
        else:
            return False
        return True

    def finish(self):
        if self.open or self.choices.keys() != self.accepted.keys():
            raise ValueError("incomplete read-order observations")
        for attempt, choice in self.choices.items():
            choice["accepted_at"], choice["accepted_sequence"] = self.accepted[attempt]
            choice["physical_service"] = self.pages.get(attempt)
            if sum(o["unit"] == choice["selected"] for o in choice["options"]) != 1:
                raise ValueError("selected read missing or duplicated")
        for span in self.intervals:
            if self.accepted[span["prior"]][1] >= self.accepted[span["attempt"]][1]:
                raise ValueError("SenseOrder predecessor was not earlier")
        return dict(handoffs=self.choices, wait_intervals=self.intervals)
