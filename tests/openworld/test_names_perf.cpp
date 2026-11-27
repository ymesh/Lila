// Naming at world scale: one NameBook handing out hundreds of names of one program stays fast and unique.

#include "names/Names.h"

#include <doctest/doctest.h>

#include <chrono>
#include <set>

using namespace lila::names;

TEST_CASE("names: 2000 unnamed shops from one book are unique, fictional and fast")
{
    for (const char* culture : {"mexican", "oldEnglish"})
    {
        NameBook book(culture);
        std::set<std::string> seen;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t k = 0; k < 2000; ++k)
        {
            const std::string n = book.place(Kind::Business, "shop", 0x1000 + k);
            CHECK_FALSE(n.empty());
            CHECK(seen.insert(n).second);
            CHECK_FALSE(book.real(n));
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE(culture << ": 2000 shop names in " << ms << " ms");
        CHECK(ms < 3000.0);
        // Asking again gives the same names without new work.
        CHECK(book.place(Kind::Business, "shop", 0x1000) == book.place(Kind::Business, "shop", 0x1000));
    }
}

TEST_CASE("names: a city whose real streets use every word of the culture still gets fictional street names")
{
    // Real data from a large Mexican city names streets with every root the mexican culture lists.
    auto real = std::make_shared<RealNames>();
    for (uint64_t k = 0; k < 4000; ++k)
        for (int cls = 0; cls < 5; ++cls)
            real->add(streetName("mexican", cls, k, 0), "osm");
    NameBook book("mexican", real);
    REQUIRE(book.real(streetName("mexican", 2, 1, 0)));
    std::set<std::string> seen;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t k = 0; k < 3000; ++k)
    {
        const std::string n = book.street(int(k % 5), 0x5000 + k);
        CHECK_FALSE(n.empty());
        CHECK(seen.insert(n).second);
        CHECK_FALSE(book.real(n));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("3000 street names with every root real in " << ms << " ms, " << book.coined() << " coined");
    CHECK(book.coined() > 0);
    CHECK(ms < 5000.0);
}

TEST_CASE("names: coinName splices distinctive words and keeps the generic ones")
{
    CHECK(coinName("Calle Las Palmas", "Avenida Los Gaviotas") == "Calle Las Palotas");
    CHECK(coinName("Calle del Mar", "Calle Los Pinos") == "Calle del Mar");
    // A word the donor shares is spliced with the next donor word, so a fixed word of a pattern still changes.
    CHECK(coinName("Cuartel Morado", "Cuartel Morado Calle Las Palmas") == "Cuaado Mormas");
    CHECK(coinName("Calle Los Guam\xc3\xba" "chiles", "Calle Los Cedros") == "Calle Los Guam\xc3\xba" "ros");
}
