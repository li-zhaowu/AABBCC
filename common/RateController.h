// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Bitrate adaptation for video mode (docs/design.md 3.7), shared by the
// monitor and the single window video streams. Platform independent; unit
// tested in tests/rate_controller.
//
// AIMD, evaluated every kInterval: congestion (frames skipped because the
// send queue was full, or a round-trip time kRttMargin above the lowest
// seen) cuts the rate to 70 %; after kGrowDelay without congestion it grows
// by 5 % of the maximum. It starts at half the maximum.
//
// The reference RTT is the lowest RTT of the last 15-30 s (two windows of
// kRttWindow): old samples expire, so a route that became slower for good
// stops counting as congestion after at most 30 s.

#pragma once

#include <chrono>
#include <cstdint>

class RateController {
public:
	using Clock = std::chrono::steady_clock;

	static constexpr std::chrono::milliseconds kInterval{500};
	static constexpr std::chrono::seconds kGrowDelay{2};
	static constexpr std::chrono::seconds kRttWindow{15};
	static constexpr uint32_t kMinKbps = 1000;
	static constexpr uint32_t kRttMarginUs = 150000;

	RateController(uint32_t maxKbps, Clock::time_point now);

	// Count a frame that was skipped because the send queue was full.
	void FrameSkipped() { ++m_skipped; }

	// Call every frame period. Returns true when it is time to change the
	// rate; the new rate is in Target(). Call Accept() once the encoder took
	// it (or not, then the controller keeps the old one).
	bool Update(Clock::time_point now, uint32_t rttUs);
	void Accept() { m_current = m_target; }

	uint32_t Current() const { return m_current; }
	uint32_t Target() const { return m_target; }
	uint32_t Max() const { return m_max; }
	bool LastWasCongestion() const { return m_lastCongested; }

private:
	uint32_t m_max;
	uint32_t m_current;
	uint32_t m_target;
	uint32_t m_minRttCurrent = 0;  // lowest RTT in the current window (0 = none)
	uint32_t m_minRttPrevious = 0; // lowest RTT in the previous window
	uint32_t m_skipped = 0;
	bool m_lastCongested = false;
	Clock::time_point m_nextCheck;
	Clock::time_point m_lastDecrease;
	Clock::time_point m_windowEnd;
};
