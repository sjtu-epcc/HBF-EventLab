#pragma once
#include "hbf_eventlab/host/workload.hpp"
#include <istream>
namespace hbf_eventlab {
// Text adapter is a separate library; core Host accepts typed requests, not files.
// Parses one record at a time; scheduling and safe simulation horizons belong
// to the caller. A blocking stream is not itself an online execution driver.
class WorkloadReader {
public:
    explicit WorkloadReader(std::istream& input):input_(input) {}
    std::optional<GlobalRequest> next();
private:
    std::istream& input_;
    std::size_t lineno_=0;
    bool header_=false;
};
std::vector<GlobalRequest> read_workload(std::istream&);
}

