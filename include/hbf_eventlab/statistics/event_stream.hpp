#pragma once
#include "hbf_eventlab/interfaces/statistics_sink.hpp"
#include <ostream>

namespace hbf_eventlab {
// Append-only JSON Lines, schema 1. Sequence is observation order, not a new
// simulation event. No history, lookup table, threads, or simulation clock here.
class StatisticsEventStream final : public IStatisticsSink {
public:
    explicit StatisticsEventStream(std::ostream&,std::size_t buffer_bytes=65536);
    StatisticsEventStream(const StatisticsEventStream&)=delete;
    StatisticsEventStream& operator=(const StatisticsEventStream&)=delete;
    void observe(const StatisticsEvent&) override;
    void flush(); // checked write + ostream flush; call at pause/shutdown
    void finish(Tick,const std::string& stop_reason); // final marker, then flush
    std::size_t buffered_bytes() const {return buffer_.size();}
    std::size_t peak_buffered_bytes() const {return peak_;}
    std::uint64_t records() const {return sequence_;}
private:
    void append(std::string);
    void write(const std::string&);
    std::ostream& out_;
    std::size_t limit_,peak_=0;
    std::uint64_t sequence_=0;
    bool closed_=false,failed_=false;
    std::string buffer_;
};
// Non-owning fan-out: all targets must outlive this object and its producers.
// Useful for validation; enabling a full collector still retains full history.
class StatisticsFanout final : public IStatisticsSink {
public:
    explicit StatisticsFanout(std::vector<IStatisticsSink*> sinks):sinks_(std::move(sinks)) {
        for(auto* sink:sinks_)if(!sink)throw std::invalid_argument("null statistics sink");
    }
    void observe(const StatisticsEvent& e) override {for(auto* sink:sinks_)sink->observe(e);}
private: std::vector<IStatisticsSink*> sinks_;
};
} // namespace hbf_eventlab
