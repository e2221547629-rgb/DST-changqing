#include "../src/probe_state.hpp"
#include <cstdio>
using namespace dst_probe;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)
int main() {
    State state;
    CHECK(!state.draw(100) && !state.draw(100));
    auto frame = state.finish_frame();
    CHECK(frame.draws == 2 && frame.hits == 1 && frame.requests == 0);
    CHECK(state.update(100, {true, F8}, 1000));
    CHECK(state.single());
    CHECK(!state.update(200, {true, F8}, 1000));
    CHECK(state.single()); // Holding F8 must not toggle again.
    CHECK(state.draw(200) && !state.draw(200));
    frame = state.finish_frame();
    CHECK(frame.draws == 2 && frame.hits == 1 && frame.requests == 1);
    state.update(201, {true, 0}, 1000);
    state.update(202, {true, F10, true}, 1000);
    CHECK(state.target() == 101);
    state.update(601, {true, F10, true}, 1000);
    CHECK(state.target() == 101);
    state.update(602, {true, F10, true}, 1000);
    CHECK(state.target() == 201);
    state.update(662, {true, F10, true}, 250);
    CHECK(state.target() == 250);
    state.update(663, {true, 0}, 250);
    state.update(664, {true, F9, true}, 250);
    CHECK(state.target() == 150);
    state.update(665, {true, 0}, 250);
    state.update(666, {true, F11}, 250);
    CHECK(!state.single() && state.target() == 1);
    state.finish_frame();
    CHECK(!state.draw(667));
    state.finish_frame();
    state.update(1000, {true, F7}, 1000);
    CHECK(!state.single() && state.burst_until() == 1500);
    for (unsigned i = 0; i < 200; ++i) CHECK(state.draw(1001));
    state.update(1100, {true, F7}, 1000);
    CHECK(state.burst_until() == 1500); // Holding F7 cannot extend its deadline.
    CHECK(state.draw(1499));
    CHECK(!state.draw(1500)); // Recovery even before another present/update.
    frame = state.finish_frame();
    CHECK(frame.requests == 201 && frame.hits == 1 && state.burst_requests() == 201);
    CHECK(state.update(1500, {true, F7}, 1000));
    CHECK(state.burst_until() == 0 && !state.single());
    state.update(1501, {true, 0}, 1000);
    state.update(1502, {true, F7}, 1000);
    state.update(1503, {false, F7}, 1000);
    CHECK(state.burst_until() == 0 && !state.draw(1504));
    state.update(1505, {false, 0}, 1000);
    state.update(1506, {false, F8}, 1000);
    CHECK(!state.single());
    state.update(1507, {true, F8}, 1000);
    CHECK(!state.single()); // A held background key cannot toggle on focus regain.
    state.update(1508, {true, 0}, 1000);
    state.update(1509, {true, F7}, 1000);
    state.update(1510, {true, F11}, 1000);
    CHECK(state.burst_until() == 0 && state.target() == 1 && !state.single());
    std::puts("PASS: hit/request counts, single suppression, held keys, repeat/Shift navigation, "
              "500 ms burst, expiry without present, focus loss, reset.");
    return 0;
}
