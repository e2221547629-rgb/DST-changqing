#include "../src/frame_capture.hpp"
#include <cstdio>
#include <sstream>
#include <string>
#include <fstream>
using namespace dst_probe;
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (false)
int main(int argc, char **argv) {
    CaptureBuffer buffer;
    int owner = 0, device = 0, other = 0;
    CHECK(!buffer.enabled() && buffer.ticket(&device) == 0);
    CHECK(buffer.arm(nullptr, &device, 9) == 0);
    const auto first = buffer.arm(&owner, &device, 9);
    CHECK(first && buffer.enabled() && buffer.ticket(&device) == first);
    CHECK(buffer.arm(&other, &other, 9) == 0 && buffer.ticket(&other) == 0);
    DrawInfo row;
    row.ordinal = 7; row.indexed = true; row.count = 18; row.instances = 1;
    row.base_vertex = -2;
    row.textures[0] = {0xabcd, 32, 16, 1, 21, 3};
    row.textures[2] = {0x1234, 64, 64, 1, 21, 3};
    buffer.record(first, row);
    CHECK(!buffer.finish(&other));
    auto capture = buffer.finish(&owner);
    CHECK(capture && capture->seen == 1 && capture->rows.size() == 1 && !buffer.enabled());
    std::ostringstream csv;
    write_capture_csv(csv, *capture);
    const auto text = csv.str();
    CHECK(text.find("texture_id,texture_width,texture_height") != std::string::npos);
    CHECK(text.find("1,10,7,indexed,18,1,0,-2,0,0,") != std::string::npos);
    CHECK(text.find(",0,0xabcd,32,16,1,21,3,0,0\n") != std::string::npos);
    CHECK(text.find(",2,0x1234,64,64,1,21,3,0,0\n") != std::string::npos);
    unsigned newlines = 0;
    for (char ch : text) if (ch == '\n') ++newlines;
    CHECK(newlines == 3); // Header plus one row per bound texture, not extra empty slots.
    if (argc == 2) {
        std::ofstream fixture(argv[1], std::ios::binary);
        fixture << text;
        CHECK(fixture.good());
    }

    const auto second = buffer.arm(&owner, &device, 100);
    CHECK(second != first);
    buffer.record(first, row); // A late callback from an old capture must be ignored.
    row.textures = {};
    for (size_t i = 0; i < CaptureBuffer::limit + 5; ++i) buffer.record(second, row);
    capture = buffer.finish(&owner);
    CHECK(capture->seen == CaptureBuffer::limit + 5 && capture->rows.size() == CaptureBuffer::limit);
    std::ostringstream empty;
    CapturedFrame one;
    one.rows.push_back(row);
    write_capture_csv(empty, one);
    CHECK(empty.str().find(",-1,0x0,0,0,0,0,0,0,0\n") != std::string::npos);

    buffer.arm(&owner, &device, 200);
    CHECK(!buffer.cancel(&other));
    CHECK(buffer.cancel(&owner) && !buffer.enabled() && !buffer.finish(&owner));
    std::puts("PASS: one-frame/device ownership, stale tickets, metadata CSV, no-texture rows, 4096-draw limit, cancellation.");
    return 0;
}
