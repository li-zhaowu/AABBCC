// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Unit tests for common/RateController. Build with tests/rate_controller/run.sh or:
//   cl /std:c++17 /EHsc rate_controller_test.cpp ..\..\common\RateController.cpp

#include "../../common/RateController.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

using Clock = RateController::Clock;
using ms = std::chrono::milliseconds;

// Advance in steps of one controller interval; apply every proposed change.
static void Run(RateController& rc, Clock::time_point& t, int steps, uint32_t rtt, bool skip = false)
{
	for (int i = 0; i < steps; ++i) {
		t += RateController::kInterval;
		if (skip)
			rc.FrameSkipped();
		if (rc.Update(t, rtt))
			rc.Accept();
	}
}

int main()
{
	Clock::time_point t{};
	RateController rc(20000, t);
	CHECK(rc.Current() == 10000); // half the maximum
	CHECK(!rc.Update(t + ms(100), 10000)); // not yet time

	// Calm link: grows by 1000 kbps (5 % of 20000) per interval after 2 s.
	Run(rc, t, 3, 10000);
	CHECK(rc.Current() == 10000); // still inside the grow delay
	Run(rc, t, 2, 10000);
	CHECK(rc.Current() > 10000);
	Run(rc, t, 40, 10000);
	CHECK(rc.Current() == 20000); // capped at the maximum

	// Skipped frames: -30 % at once.
	Run(rc, t, 1, 10000, true);
	CHECK(rc.Current() == 14000 && rc.LastWasCongestion());
	// No growth for 2 s after a decrease.
	Run(rc, t, 3, 10000);
	CHECK(rc.Current() == 14000);

	// RTT 150 ms above the lowest seen also counts as congestion.
	Run(rc, t, 1, 10000 + 150000 + 1);
	CHECK(rc.Current() == 9800);
	Run(rc, t, 1, 10000 + 149000); // within the margin: no decrease
	CHECK(rc.Current() == 9800);

	// Never below the minimum.
	Run(rc, t, 50, 10000, true);
	CHECK(rc.Current() == RateController::kMinKbps);

	// A route that became slower for good: congestion at first, but old RTT
	// samples expire, so after at most 30 s the new level is the reference
	// and the rate grows again.
	RateController r2(10000, t);
	Run(r2, t, 70, 20000);          // 35 s at 20 ms
	Run(r2, t, 1, 200000);          // jump to 200 ms
	CHECK(r2.LastWasCongestion());
	Run(r2, t, 60, 200000);         // 30 s at 200 ms
	const uint32_t settled = r2.Current();
	CHECK(!r2.LastWasCongestion());
	Run(r2, t, 20, 200000);         // 10 more calm seconds
	CHECK(r2.Current() > settled);  // growing again
	// A real spike on the new level is still detected.
	Run(r2, t, 1, 200000 + 200000);
	CHECK(r2.LastWasCongestion());

	// A maximum below the minimum is raised to the minimum.
	RateController low(300, t);
	CHECK(low.Current() == RateController::kMinKbps && low.Max() == RateController::kMinKbps);

	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all rate_controller tests passed\n");
	return 0;
}
