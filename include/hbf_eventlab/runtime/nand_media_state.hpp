#pragma once
#include "hbf_eventlab/architecture/device_description.hpp"
#include "hbf_eventlab/interfaces/assembly_storage.hpp"
#include <map>

namespace hbf_eventlab {
// Functional media state only: no command timing, ECC, analog failure model or resources.
// Initially erased test medium. REFERENCE PEC accounting: +1 on successful erase completion.
// Logical invalidation/remapping never erases physical data or resets its wear history.
class NandMediaState {
public:
    explicit NandMediaState(Geometry g) : geometry_(g) { g.validate(); }
    void erase(PhysicalBlockKey);
    void program(PhysicalNandAddress, const PageData&);
    const PageData* read(PhysicalNandAddress) const;
    void mark_unusable(PhysicalBlockKey);
    bool unusable(PhysicalBlockKey) const;
    std::uint64_t erase_count(PhysicalBlockKey) const;
private:
    struct Block { std::uint64_t erases = 0; PageId next_page = 0; bool unusable = false; std::map<PageId, PageData> pages; };
    void validate(PhysicalBlockKey) const;
    Geometry geometry_;
    std::map<PhysicalBlockKey, Block> blocks_;
};
} // namespace hbf_eventlab
