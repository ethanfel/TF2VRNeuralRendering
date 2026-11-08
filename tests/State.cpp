#include "State.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace tf2vrnr;
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}
int main() {
    ImageQueue q;
    Check(!q.Writable(), "unacquired image must not be read");
    q.Acquired(2); q.Acquired(0);
    Check(!q.Writable(), "acquire alone does not give writable ownership");
    q.Waited(); Check(q.Writable() == 2, "wait grants oldest acquired image");
    q.Released(); Check(!q.Writable(), "release removes ownership, next image needs a wait");
    q.Waited(); Check(q.Writable() == 0, "second acquisition preserves its own index");
    q.Released(); Check(!q.Writable(), "no ownership after final release");
    q.Acquired(1); q.Acquired(2); q.Waited(); q.Waited(); q.Released();
    Check(q.Writable() == 2, "multiple successful waits retain FIFO release order");
    ViewLayout l{10, 0, 0, 0, 2476, 2594}, r{10, 1, 0, 0, 2476, 2594};
    ViewHistory left, right;
    Check(left.Observe(l, 1000000000), "left first frame resets history");
    Check(right.Observe(r, 1000000000), "right first frame has independent history");
    Check(!left.Observe(l, 1011000000), "stable left view keeps history");
    Check(!right.Observe(r, 1011000000), "stable right view keeps history");
    l.width = 2000;
    Check(left.Observe(l, 1022000000), "left resize resets only left history");
    Check(!right.Observe(r, 1022000000), "right history is not reset by left resize");
    Check(right.Observe(r, 1600000000), "long frame gap resets temporal data");
    Check(right.Observe(r, 1500000000), "time moving backwards resets temporal data");
    right.Invalidate(); Check(right.Observe(r, 1511000000), "failure invalidates next view history");
    Check(Fits(2476, 2594, 2, r), "right texture array slice accepted");
    r.slice = 2; Check(!Fits(2476, 2594, 2, r), "out of range slice rejected");
    r.slice = 0; r.x = -1; Check(!Fits(2476, 2594, 2, r), "negative crop rejected");
    r.x = std::numeric_limits<int32_t>::max();
    Check(!Fits(2476, 2594, 2, r), "crop overflow cannot wrap into valid region");
    r = {10, 0, 2476, 0, 2476, 2594};
    Check(Fits(4952, 2594, 1, r), "right eye in a shared atlas accepted");
    std::printf("PASS: %u frame ownership and per-eye history checks\n", checks);
}
