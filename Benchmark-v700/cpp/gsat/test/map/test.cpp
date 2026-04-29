#include <catch.hpp>

#include <memory>

#include <validate.h>

#include "map.h"

using MAP = Map<int, int>;
static constexpr int kNoValue = -1;

TEST_CASE("map_unit_test") {
    MAP map(kNoValue);

    auto root = map.GetRoot();
    auto node_handler = std::unique_ptr<MAP::NodeHandler>(map.GetNodeHandler());

    auto num_keys = [&]() {
        return GetNumKeys(node_handler.get(), root);
    };
    auto sum_keys = [&]() {
        return GetSumKeys(node_handler.get(), root);
    };

    SECTION("inserts_deletes") {
        map.Insert(1, 2);
        REQUIRE(map.Contains(1));

        map.Delete(1);
        REQUIRE(!map.Contains(1));

        map.Insert(2, 3);
        map.Insert(3, 4);
        REQUIRE(map.Find(2) == 3);
        REQUIRE(map.Find(1) == kNoValue);

        map.Insert(4, 5);
        REQUIRE(map.Find(3) == 4);

        REQUIRE(num_keys() == 3);
        REQUIRE(sum_keys() == 9);
    }

    SECTION("deletes_non_existed") {
        map.Delete(1);
        REQUIRE(!map.Contains(1));
        REQUIRE(sum_keys() == 0);
        REQUIRE(num_keys() == 0);

        map.Insert(100, 100);
        REQUIRE(sum_keys() == 100);

        map.Delete(100);
        map.Delete(100);

        REQUIRE(sum_keys() == 0);
        REQUIRE(num_keys() == 0);
    }
}
