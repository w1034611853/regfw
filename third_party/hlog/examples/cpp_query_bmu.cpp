#include "hlog.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

class Bmu {
public:
    explicit Bmu(const char *instance) : instance_(instance)
    {
        map_table_query_ = hlog::register_query(
            "bmu", instance_.c_str(), "map-table",
            "Show VF l_pool to vl_pool mappings",
            {
                hlog::required_u32("vf", "VF index, valid range 0..7"),
                hlog::optional_u32("l_pool", "Optional logical pool filter"),
            },
            [this](const hlog::QueryArgs &args, hlog::QueryOutput &output) {
                uint32_t vf = args.u32("vf");
                auto l_pool = args.optional_u32("l_pool");
                if (vf >= map_table_.size()) {
                    throw std::invalid_argument("vf is out of range");
                }
                if (l_pool && *l_pool >= map_table_[vf].size()) {
                    throw std::invalid_argument("l_pool is out of range");
                }
                output.print(
                    "schema=hlog-query-v1 target=bmu/{}/map-table "
                    "status=ok\n",
                    instance_);
                if (l_pool) {
                    output.print(
                        "record=map-entry vf={} l_pool={} vl_pool={}\n",
                        vf, *l_pool, map_table_[vf][*l_pool]);
                    output.print(
                        "summary record_count=1 invalid_count=0\n");
                    return;
                }
                for (uint32_t index = 0;
                     index < map_table_[vf].size(); ++index) {
                    output.print(
                        "record=map-entry vf={} l_pool={} vl_pool={}\n",
                        vf, index, map_table_[vf][index]);
                }
                output.print(
                    "summary record_count={} invalid_count=0\n",
                    map_table_[vf].size());
            });
        if (!map_table_query_) {
            throw std::runtime_error(hlog::last_error());
        }
    }

private:
    std::string instance_;
    std::array<std::array<uint32_t, 4>, 8> map_table_{};
    hlog::QueryRegistration map_table_query_;
};
