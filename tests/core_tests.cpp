// psvrecore::core: Result/Exception handling and log categories.

#include <cstring>

#include "nxtest.h"
#include "psvrecore/error.hpp"
#include "psvrecore/log.hpp"

namespace {

psvrecore::Result<int> parse_positive(int value) {
    if (value <= 0) return psvrecore::failure("not positive");
    return value;
}

}  // namespace

TEST_CASE("core: Result carries a value or an Error") {
    const auto ok = parse_positive(5);
    REQUIRE(ok.has_value());
    CHECK(*ok == 5);

    const auto bad = parse_positive(-1);
    REQUIRE_FALSE(bad.has_value());
    CHECK(nx::string_view(bad.error().message) == "not positive");
}

TEST_CASE("core: value_or_throw turns an Error into an Exception") {
    CHECK(psvrecore::value_or_throw(parse_positive(7)) == 7);
    CHECK_THROWS_AS(psvrecore::value_or_throw(parse_positive(0)), psvrecore::Exception);
}

TEST_CASE("core: log categories are registered under their own names") {
    CHECK(std::strcmp(psvrecore::logs::loader.name(), "loader") == 0);
    CHECK(std::strcmp(psvrecore::logs::gxm.name(), "gxm") == 0);
    CHECK(std::strcmp(psvrecore::logs::tool.name(), "tool") == 0);
}
