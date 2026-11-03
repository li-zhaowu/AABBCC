// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "RateController.h"

#include <algorithm>

constexpr std::chrono::milliseconds RateController::kInterval;
constexpr std::chrono::seconds RateController::kGrowDelay;
constexpr std::chrono::seconds RateController::kRttWindow;
constexpr uint32_t RateController::kMinKbps;
constexpr uint32_t RateController::kRttMarginUs;

RateController::RateController(uint32_t maxKbps, Clock::time_point now)
	: m_max((std::max)(maxKbps, kMinKbps)),
	  m_current((std::max)(kMinKbps, m_max / 2)),
	  m_target(m_current),
	  m_nextCheck(now + kInterval),
	  m_lastDecrease(now),
	  m_windowEnd(now + kRttWindow)
{
}

bool RateController::Update(Clock::time_point now, uint32_t rttUs)
{
	if (now < m_nextCheck)
		return false;
	m_nextCheck = now + kInterval;

	if (now >= m_windowEnd) {
		m_windowEnd = now + kRttWindow;
		m_minRttPrevious = m_minRttCurrent;
		m_minRttCurrent = 0;
	}
	if (rttUs != 0 && (m_minRttCurrent == 0 || rttUs < m_minRttCurrent))
		m_minRttCurrent = rttUs;
	uint32_t baseRtt = m_minRttCurrent;
	if (m_minRttPrevious != 0 && (baseRtt == 0 || m_minRttPrevious < baseRtt))
		baseRtt = m_minRttPrevious;
	const bool rttHigh = rttUs != 0 && baseRtt != 0 && rttUs > baseRtt + kRttMarginUs;
	const bool congested = m_skipped > 0 || rttHigh;
	m_skipped = 0;
	m_lastCongested = congested;

	m_target = m_current;
	if (congested) {
		m_target = (std::max)(kMinKbps, m_current * 7 / 10);
		m_lastDecrease = now;
	} else if (now - m_lastDecrease >= kGrowDelay) {
		m_target = (std::min)(m_max, m_current + (std::max)(250u, m_max / 20));
	}
	return m_target != m_current;
}
