#include "hbflab/backend.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace hbflab {
using namespace hbf_eventlab;
namespace {
void require(bool okay, const char *why) {
  if (!okay)
    throw std::runtime_error(why);
}
ResolvedSimulationConfiguration configuration(const BackendOptions &o) {
  if (o.geometry.host_channels != 1 || o.resident_fragments < 64 ||
      !o.max_events_per_batch)
    throw std::invalid_argument("one channel, at least 64 resident fragments "
                                "and a positive event budget required");
  SimulationConfiguration c;
  c.device.geometry = o.geometry;
  c.device.capabilities = {256, 4, false, true};
  c.device.selected = {o.outstanding, Tick{10000000},
                       o.geometry.block_group_dlus()};
  c.execution = o.timing;
  c.host.writes = WriteCommandMode::Burst4096;
  c.host.hints = ReadHintMode::AllRegular;
  c.host.max_resident_fragments = o.resident_fragments;
  c.host.max_logical_fragments = 1000000000;
  c.run.max_tick = std::nullopt;
  c.run.events = {o.max_events_per_batch, 100000, 100000};
  return resolve_configuration(c);
}
} // namespace
// Producer parent aggregation and logical snapshot ordering. The single Session
// owns Host/Device, command splitting, admission, retries and physical replay.
struct TimingBackend::Impl {
  struct Range {
    std::uint64_t address, bytes, content;
  };
  struct Parent {
    std::uint64_t sequence = 0, arrival = 0, logical = 0;
    bool write = false, built = false, wait_counted = false, released = false,
         initialization = false;
    std::set<std::uint64_t> blocks;
    std::vector<Range> ranges;
    std::size_t next = 0, pending = 0;
    std::array<std::uint64_t, 64> tokens{};
    ReadResult result;
    bool done() const {
      return released && built && next == ranges.size() && !pending;
    }
  };
  BackendOptions options;
  std::unique_ptr<SimulationSession> session;
  ReferenceAddressLayout layout;
  std::uint64_t allocated = 0, next_global = 0, next_parent = 0,
                last_arrival = 0, programs = 0, executed = 0;
  std::size_t parent_limit = 0;
  bool streaming = false, poisoned = false, pumping = false,
       logical_failure = false;
  BackendStats stats;
  std::map<std::uint64_t, std::shared_ptr<Parent>> parents;
  std::map<GlobalRequestId, std::shared_ptr<Parent>> globals;
  std::map<std::uint64_t, std::uint64_t> versions;
  std::vector<std::array<std::uint64_t, 64>> contents;
  explicit Impl(BackendOptions o)
      : options(o),
        session(std::make_unique<SimulationSession>(configuration(o))),
        layout(o.geometry) {
    if (o.fail_program || o.fail_program_after != UINT64_MAX)
      session->set_program_outcome([this](const auto &) {
        return options.fail_program || programs++ >= options.fail_program_after
                   ? ProgramOutcome::FailedReusable
                   : ProgramOutcome::Success;
      });
    session->set_completion_handler([this](const GlobalCompletion &e) {
      const auto found = globals.find(e.result.id);
      require(found != globals.end(), "unknown or duplicate Global completion");
      auto p = found->second;
      globals.erase(found);
      --p->pending;
      ++stats.completed_globals;
      if (e.result.state != LogicalResult::Success) {
        logical_failure = true;
        // Stop the unregistered producer suffix. Accepted Globals keep running
        // until their final callbacks; a normal logical failure is not a
        // callback bug.
        for (auto &item : parents) {
          auto &abandoned = *item.second;
          abandoned.result.status = Status::BackendError;
          abandoned.next = abandoned.ranges.size();
          abandoned.built = abandoned.released = true;
        }
      }
      p->result.end_ps = std::max(p->result.end_ps, e.at.value);
      if (p->done() && p->write && p->result.status == Status::Ok) {
        contents.at(p->logical / 4096) = p->tokens;
        if (!p->initialization) {
          ++stats.writebacks;
          stats.replayed_pages += p->ranges.size();
        }
      }
      pump(true);
      session->request_yield();
    });
    session->set_input_capacity_handler([this](InputCapacity) {
      pump(true);
      session->request_yield();
    });
  }
  std::uint64_t now() const { return session->now().value; }
  std::uint64_t block_base(std::uint64_t address) const {
    const auto t = layout.logical_view({0, LocalByteAddress{address}});
    return layout.encode_logical({t.block, 0, 0}).value;
  }
  std::vector<std::uint64_t> block_pages(std::uint64_t address) const {
    const auto t = layout.logical_view({0, LocalByteAddress{address}});
    std::vector<std::uint64_t> pages;
    for (std::uint32_t page = 0; page < options.geometry.pages_per_block;
         ++page) {
      const auto at = layout.encode_logical({t.block, page, 0}).value;
      if (at < allocated)
        pages.push_back(at);
    }
    return pages;
  }
  bool eligible(Parent &p) {
    if (p.arrival > now() || p.done())
      return false;
    p.released = true;
    for (const auto &item : parents) {
      const auto &older = *item.second;
      if (older.sequence >= p.sequence || older.done() ||
          (!older.write && !p.write))
        continue;
      for (auto block : p.blocks)
        if (older.blocks.count(block)) {
          if (!p.wait_counted) {
            p.wait_counted = true;
            ++stats.dependency_waits;
          }
          return false;
        }
    }
    if (!p.built) {
      if (p.write) {
        auto &content = versions[block_base(p.logical)];
        content = checked_add(content, 1);
        // Application block snapshot: unchanged real pages stay in
        // CapacityStore. This is normal Global input; Host still owns all
        // physical replay.
        for (auto at : block_pages(p.logical))
          p.ranges.push_back({at, 4096, content});
      } else
        for (auto &r : p.ranges)
          r.content = versions.at(block_base(r.address));
      p.built = true;
    }
    return p.next < p.ranges.size();
  }
  void pump(bool feedback) {
    if (logical_failure)
      return;
    if (pumping)
      return;
    pumping = true;
    try {
      for (const auto &item : parents) {
        auto p = item.second;
        if (!eligible(*p))
          continue;
        while (p->next < p->ranges.size()) {
          const auto &r = p->ranges[p->next];
          const auto capacity = session->input_capacity();
          if (capacity.limit &&
              r.bytes / 64 > capacity.limit - capacity.resident) {
            ++stats.backpressure;
            break;
          }
          const GlobalRequestId id{checked_add(next_global, 1)};
          GlobalRequest request{id, p->write ? Op::Write : Op::Read,
                                GlobalByteAddress{r.address}, r.bytes,
                                r.content};
          request.not_before = session->now();
          request.offered_at = Tick{p->arrival};
          if (!p->write)
            request.expected_content = r.content;
          const auto admitted = feedback ? session->try_submit_feedback(request)
                                         : session->try_submit(request);
          require(admitted.disposition != InputDisposition::TooLarge,
                  "Global exceeds resident budget");
          if (admitted.disposition == InputDisposition::Backpressured) {
            ++stats.backpressure;
            break;
          }
          next_global = id.value;
          globals.emplace(id, p);
          ++p->pending;
          ++p->next;
          ++stats.registered_globals;
          stats.registered_fragments += r.bytes / 64;
          stats.peak_registered_globals =
              std::max(stats.peak_registered_globals, globals.size());
          stats.peak_resident_fragments =
              std::max(stats.peak_resident_fragments,
                       session->input_capacity().resident);
        }
      }
      pumping = false;
    } catch (...) {
      pumping = false;
      throw;
    }
  }
  void continue_feedback() {
    pump(true);
    if (globals.empty() &&
        std::all_of(parents.begin(), parents.end(),
                    [](const auto &p) { return p.second->done(); }))
      session->request_yield();
  }
  void limits() {
    auto r = session->run_limits();
    r.events.total_events = checked_add(executed, options.max_events_per_batch);
    session->set_run_limits(r);
  }
  void report(const SessionReport &r) {
    executed = r.run.events;
    if (r.run.status == RunStatus::LimitReached ||
        r.run.status == RunStatus::ModelError ||
        r.run.status == RunStatus::ImplementationError ||
        r.run.status == RunStatus::Stalled)
      throw std::runtime_error(std::string(to_string(r.run.status)) + ": " +
                               r.run.explanation);
  }
  void drain_logical_failure() {
    if (!logical_failure)
      return;
    session->close_input();
    stats.input_closed = true;
    SessionReport final = session->run_to_completion();
    report(final);
    while (final.feedback_yield) {
      final = session->run_to_completion();
      report(final);
    }
    require(final.run.status == RunStatus::Completed && globals.empty() &&
                final.run.host.drained() && final.run.device.drained(),
            "logical failure left registered work undrained");
    throw std::runtime_error(
        "Host final logical failure; registered work drained");
  }
  bool advance(std::uint64_t bound) {
    require(!poisoned, "backend is poisoned");
    if (now() >= bound)
      return false;
    try {
      bool ready = false;
      std::optional<std::uint64_t> arrival;
      for (const auto &item : parents)
        if (!item.second->done()) {
          if (item.second->arrival <= now())
            ready = true;
          else if (!arrival || item.second->arrival < *arrival)
            arrival = item.second->arrival;
        }
      if (ready) {
        if (session->accepts_at_floor())
          pump(false);
        else if (session->can_resume_feedback())
          session->resume_feedback([this] { continue_feedback(); });
      }
      const auto before = executed;
      const auto next = session->next_event_time();
      if (arrival && *arrival < bound && (!next || *arrival <= next->value)) {
        if (*arrival > now())
          report(session->advance_before(Tick{*arrival}));
        drain_logical_failure();
        if (session->accepts_at_floor())
          pump(false);
        else if (session->can_resume_feedback())
          session->resume_feedback([this] { continue_feedback(); });
        return true;
      }
      if (!next || next->value >= bound)
        return false;
      if (bound == UINT64_MAX)
        report(session->advance_until_yield());
      else
        report(session->advance_before(Tick{bound}));
      drain_logical_failure();
      return executed != before;
    } catch (...) {
      poisoned = true;
      throw;
    }
  }
  std::shared_ptr<Parent> make_read(const TimedRead &input) {
    auto p = std::make_shared<Parent>();
    p->arrival = input.arrival_ps;
    p->sequence = next_parent = checked_add(next_parent, 1);
    p->result = {Status::Ok, input.arrival_ps, input.arrival_ps, 0};
    std::set<std::uint64_t> sectors;
    for (const auto &span : input.spans)
      for (auto sector = span.logical / 64;
           sector <= (span.logical + span.bytes - 1) / 64; ++sector)
        sectors.insert(sector);
    for (auto sector : sectors) {
      const auto address = sector * 64;
      p->blocks.insert(block_base(address));
      if (!p->ranges.empty() &&
          p->ranges.back().address + p->ranges.back().bytes == address &&
          address % 4096)
        p->ranges.back().bytes += 64;
      else
        p->ranges.push_back({address, 64, 0});
    }
    p->result.sectors = static_cast<std::uint32_t>(sectors.size());
    if (!input.spans.empty())
      ++stats.requests;
    stats.sectors += sectors.size();
    if (p->ranges.empty())
      p->built = true;
    return p;
  }
  bool valid(const TimedRead &input) const {
    if (input.spans.size() > max_spans)
      return false;
    std::uint64_t bytes = 0;
    for (const auto &span : input.spans) {
      bytes = checked_add(bytes, span.bytes);
      if (!span.bytes || span.bytes > max_read_bytes ||
          span.logical >= allocated || span.bytes > allocated - span.logical)
        return false;
    }
    return bytes <= max_read_bytes;
  }
  void wait() {
    while (advance(UINT64_MAX)) {
    }
    require(std::all_of(parents.begin(), parents.end(),
                        [](const auto &p) { return p.second->done(); }),
            "unfinished parent with no progress");
  }
};
TimingBackend::TimingBackend(BackendOptions o)
    : impl_(std::make_unique<Impl>(o)) {}
TimingBackend::~TimingBackend() = default;
std::uint64_t TimingBackend::now_ps() const { return impl_->now(); }
bool TimingBackend::drained() const {
  const auto &s = *impl_;
  return !s.poisoned && s.parents.empty() && s.globals.empty() &&
         s.session->host().activity().drained() &&
         s.session->device().activity().drained() &&
         !s.session->next_event_time();
}
BackendStats TimingBackend::statistics() const {
  auto out = impl_->stats;
  out.device = impl_->session->device().statistics();
  out.host = impl_->session->host().statistics();
  return out;
}
std::uint64_t TimingBackend::reserve(std::uint64_t bytes) {
  auto &s = *impl_;
  require(!s.streaming && !s.poisoned && !s.stats.input_closed,
          "backend unavailable for reservation");
  if (!bytes || bytes % 4096 ||
      bytes > s.options.geometry.channel_bytes() - s.allocated)
    throw std::invalid_argument("reservation must fit whole DLUs");
  const auto base = s.allocated;
  try {
    for (std::uint64_t offset = 0; offset < bytes; offset += 4096) {
      s.limits();
      auto p = std::make_shared<Impl::Parent>();
      p->sequence = ++s.next_parent;
      p->arrival = now_ps();
      p->write = true;
      p->initialization = true;
      p->built = true;
      p->logical = base + offset;
      auto &content = s.versions[s.block_base(base + offset)];
      if (!content)
        content = 1;
      p->ranges.push_back({base + offset, 4096, content});
      p->result = {Status::Ok, now_ps(), now_ps(), 0};
      for (unsigned i = 0; i < 64; ++i)
        p->tokens[i] = (base + offset) / 64 + i + 1;
      s.contents.push_back({});
      s.parents.emplace(1, p);
      s.wait();
      s.parents.clear();
    }
    s.allocated += bytes;
  } catch (...) {
    s.poisoned = true;
    throw;
  }
  return base;
}
std::vector<ReadResult>
TimingBackend::read_batch(const std::vector<TimedRead> &inputs) {
  auto &s = *impl_;
  require(!s.streaming && !s.poisoned && !s.stats.input_closed,
          "backend unavailable for synchronous reads");
  if (inputs.empty() || inputs.size() > max_threads)
    throw std::invalid_argument("invalid parent count");
  auto previous = now_ps();
  bool invalid = false;
  for (const auto &input : inputs) {
    if (input.arrival_ps < previous || input.arrival_ps == UINT64_MAX)
      throw std::invalid_argument("unsorted or late arrival");
    previous = input.arrival_ps;
    invalid |= !s.valid(input);
  }
  if (invalid)
    return std::vector<ReadResult>(
        inputs.size(), {Status::InvalidRange, now_ps(), now_ps(), 0});
  s.limits();
  std::vector<std::shared_ptr<Impl::Parent>> work;
  for (const auto &input : inputs) {
    auto p = s.make_read(input);
    work.push_back(p);
    s.parents.emplace(work.size(), p);
  }
  try {
    s.wait();
  } catch (...) {
    s.poisoned = true;
    throw;
  }
  std::vector<ReadResult> results;
  for (auto p : work)
    results.push_back(p->result);
  s.parents.clear();
  return results;
}
ReadResult TimingBackend::read(std::uint64_t logical, std::uint32_t bytes,
                               std::uint64_t gap) {
  return read_batch({{checked_add(now_ps(), gap), {{logical, bytes}}}}).front();
}
WriteResult
TimingBackend::write_page(std::uint64_t logical,
                          const std::array<std::uint64_t, 64> &tokens) {
  auto &s = *impl_;
  require(!s.streaming && !s.poisoned && !s.stats.input_closed,
          "backend unavailable for synchronous writes");
  if (logical % 4096 || logical >= s.allocated)
    throw std::invalid_argument("write requires a reserved page");
  s.limits();
  auto p = std::make_shared<Impl::Parent>();
  p->write = true;
  p->logical = logical;
  p->tokens = tokens;
  p->arrival = now_ps();
  p->sequence = ++s.next_parent;
  p->blocks.insert(s.block_base(logical));
  p->result = {Status::Ok, now_ps(), now_ps(), 0};
  s.parents.emplace(1, p);
  try {
    s.wait();
  } catch (...) {
    s.poisoned = true;
    throw;
  }
  WriteResult result{p->result.status, p->result.begin_ps, p->result.end_ps,
                     static_cast<std::uint32_t>(p->ranges.size())};
  s.parents.clear();
  return result;
}
void TimingBackend::begin_stream(std::size_t limit) {
  auto &s = *impl_;
  require(!s.streaming && drained() && !s.stats.input_closed,
          "stream requires drained backend");
  if (!limit || limit > 8192)
    throw std::invalid_argument("invalid parent bound");
  s.limits();
  s.streaming = true;
  s.parent_limit = limit;
  s.last_arrival = now_ps();
}
void TimingBackend::submit_read(std::uint64_t id, const TimedRead &input) {
  auto &s = *impl_;
  require(s.streaming && !s.poisoned, "no valid stream");
  if (!id || s.parents.count(id) || s.parents.size() >= s.parent_limit ||
      input.arrival_ps == UINT64_MAX || input.arrival_ps < s.last_arrival ||
      input.arrival_ps <= now_ps() || input.spans.empty() || !s.valid(input))
    throw std::invalid_argument("invalid stream read");
  auto p = s.make_read(input);
  s.parents.emplace(id, p);
  s.last_arrival = input.arrival_ps;
}
void TimingBackend::submit_write(std::uint64_t id, std::uint64_t arrival,
                                 std::uint64_t logical,
                                 const std::array<std::uint64_t, 64> &tokens) {
  auto &s = *impl_;
  require(s.streaming && !s.poisoned, "no valid stream");
  if (!id || s.parents.count(id) || s.parents.size() >= s.parent_limit ||
      arrival == UINT64_MAX || arrival < s.last_arrival ||
      arrival <= now_ps() || logical % 4096 || logical >= s.allocated)
    throw std::invalid_argument("invalid stream write");
  auto p = std::make_shared<Impl::Parent>();
  p->write = true;
  p->logical = logical;
  p->tokens = tokens;
  p->arrival = arrival;
  p->sequence = ++s.next_parent;
  p->blocks.insert(s.block_base(logical));
  p->result = {Status::Ok, arrival, arrival, 0};
  s.parents.emplace(id, p);
  s.last_arrival = arrival;
}
bool TimingBackend::advance_before(std::uint64_t bound) {
  require(impl_->streaming, "no stream");
  return impl_->advance(bound);
}
std::vector<StreamReadResult> TimingBackend::take_completed() {
  auto &s = *impl_;
  require(s.streaming && !s.poisoned, "no valid stream");
  std::vector<StreamReadResult> out;
  for (auto i = s.parents.begin(); i != s.parents.end();)
    if (!i->second->write && i->second->done()) {
      out.push_back({i->first, i->second->result});
      i = s.parents.erase(i);
    } else
      ++i;
  return out;
}
std::vector<StreamWriteResult> TimingBackend::take_completed_writes() {
  auto &s = *impl_;
  require(s.streaming && !s.poisoned, "no valid stream");
  std::vector<StreamWriteResult> out;
  for (auto i = s.parents.begin(); i != s.parents.end();) {
    auto p = i->second;
    if (p->write && p->done()) {
      out.push_back({i->first,
                     {p->result.status, p->result.begin_ps, p->result.end_ps,
                      static_cast<std::uint32_t>(p->ranges.size())}});
      i = s.parents.erase(i);
    } else
      ++i;
  }
  return out;
}
void TimingBackend::end_stream() {
  auto &s = *impl_;
  require(s.streaming && drained(), "stream has unfinished work/results");
  s.streaming = false;
}
void TimingBackend::finish() {
  auto &s = *impl_;
  require(!s.streaming && drained(),
          "finish requires drained producers and media");
  if (s.stats.input_closed)
    return;
  s.session->close_input();
  const auto r = s.session->run_to_completion();
  s.report(r);
  require(r.run.status == RunStatus::Completed && r.run.workload_success &&
              r.run.host.drained() && r.run.device.drained(),
          "closed session failed to drain");
  s.stats.input_closed = true;
}
} // namespace hbflab
