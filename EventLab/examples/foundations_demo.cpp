#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include "hbf_eventlab/runtime/read_path.hpp"
#include "support/bounded_read_results.hpp"
#include <iostream>

using namespace hbf_eventlab;
int main() {
    try {
        const auto d = demonstration_profile();
        ReferenceAddressLayout layout(d.geometry());
        ZoneMapping zones(d); BlockReplacement replacements(d.geometry());
        NandMediaState media(d.geometry()); EventQueue events;
        CapacityRetirementCoordinator capacity(d, layout, zones, replacements, media);
        AddressResolver resolver(capacity);
        AdmissionWaitRegistry signals(events);
        CommandAdmission admission(d.geometry().host_channels, d.configuration().mocs_selected,signals);
        test_support::BoundedReadResults results(64);
        WritePath writes(d, layout, capacity, media, events, admission);
        ReadPath reads(d, layout, capacity, media, admission, writes, results);
        const LogicalBlockKey block{0, 0, 0, 0};
        std::uint64_t id = 0;
        auto issue = [&](PageId page, std::uint64_t value) {
            const auto address = layout.encode_logical({block, page, 0});
            HostCommand cmd{CommandId{++id}, Op::Write, {0, address}, dlu_bytes, 0};
            auto result = writes.submit(cmd, std::vector<DataToken>(64, DataToken{value}));
            if (result.admission != SubmitResult::Accepted || result.immediate_status) throw std::runtime_error("demo write rejected");
            std::cout << "[Host] command=" << id << " logical page=" << page << " local_byte=" << address.value << '\n';
            return DluKey{0, LocalDlu{address.value / dlu_bytes}};
        };
        auto erase = [&] {
            for (auto op : writes.pending_erases()) {
                std::cout << "[erase] physical block=" << op.physical.block << '\n';
                writes.finish_erase(writes.issue_erase(op.id)->id);
            }
        };
        auto program = [&](DluKey key, ProgramOutcome outcome) {
            auto op = writes.issue_program(*writes.program_candidate(key));
            if (!op) throw std::runtime_error("program not legal");
            events.schedule_at(Tick{events.now().value + 5}, EventPhase::Completion,
                [&, ticket = op->id, outcome] { writes.finish_program(ticket, outcome); });
            events.run();
            for (auto reply : writes.responses()) {
                std::cout << "[response] command=" << reply.command.value << " status=" << static_cast<int>(reply.status)
                          << " info=" << static_cast<int>(reply.additional_info) << '\n';
                writes.response_delivered(reply.channel, reply.command);
            }
        };
        auto read = [&](PageId page, ReadStatus expected, const char* label) {
            HostCommand cmd{CommandId{++id}, Op::Read, {0, layout.encode_logical({block,page,0})}, host_unit_bytes, 0};
            const auto submitted=reads.submit(cmd);
            if(submitted.admission!=SubmitResult::Accepted) throw std::runtime_error("demo read backpressured");
            if(submitted.candidate) {
                const auto operation=reads.issue(*submitted.candidate);
                if(!operation) throw std::runtime_error("demo read cannot issue");
                events.schedule_at(Tick{events.now().value+3},EventPhase::Completion,
                    [&, ticket=operation->id]{reads.finish_read(ticket);});
                events.run();
            }
            const auto replies=reads.responses();
            if(replies.size()!=1 || replies.front().status!=expected) throw std::runtime_error("unexpected demo read result");
            const auto& reply=replies.front();
            std::cout << "[read " << label << "] command=" << reply.command.value << " page=" << page
                      << " status=" << static_cast<int>(reply.status);
            if(reply.data) std::cout << " token=" << reply.data->value;
            std::cout << '\n';
            reads.response_delivered(reply.channel,reply.command);
        };
        std::cout << "HBF EventLab explicit-issue component demo; completion delays are illustrative only\n";
        for (PageId page = 0; page < 2; ++page) {
            const auto key = issue(page, 100 + page); erase(); program(key, ProgramOutcome::Success);
        }
        const auto failed = issue(2, 102);
        read(0,ReadStatus::Success,"media");
        issue(3, 103);
        program(failed, ProgramOutcome::FailedUnusable);
        read(0,ReadStatus::TemporarilyRestricted,"awaiting replay");
        for (PageId page = 0; page <= 2; ++page) {
            const auto key = issue(page, 100 + page);
            read(page,ReadStatus::Success,"Pending replay");
            erase(); program(key, ProgramOutcome::Success);
            if(page<2) read(0,ReadStatus::TemporarilyRestricted,"merged endpoint pending");
        }
        std::cout << "[replay] prefix 0..2 complete; Host resubmits page 3\n";
        program(issue(3, 103), ProgramOutcome::Success);
        read(0,ReadStatus::Success,"recovery complete");
        read(5,ReadStatus::UnwrittenPage,"unwritten");
        const ChannelAddress first{0, LocalByteAddress{0}};
        const auto before = resolver.inspect(first);
        const auto refresh = issue(0, 100); erase(); program(refresh, ProgramOutcome::Success);
        const auto after = resolver.inspect(first);
        std::cout << "[refresh/new content] target stays " << before.block << " -> " << after.block
                  << "; erase_count=" << media.erase_count(block_of(after)) << '\n';
        std::cout << "No timing/performance or full AXI compliance claims.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
