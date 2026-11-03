// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "ExtChannel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace extmsg {

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

void PutU16(std::vector<uint8_t>& v, uint16_t x)
{
	v.push_back(static_cast<uint8_t>(x >> 8));
	v.push_back(static_cast<uint8_t>(x));
}

void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
	PutU16(v, static_cast<uint16_t>(x >> 16));
	PutU16(v, static_cast<uint16_t>(x));
}

void PutU64(std::vector<uint8_t>& v, uint64_t x)
{
	PutU32(v, static_cast<uint32_t>(x >> 32));
	PutU32(v, static_cast<uint32_t>(x));
}

uint16_t GetU16(const uint8_t* p)
{
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t GetU32(const uint8_t* p)
{
	return (static_cast<uint32_t>(GetU16(p)) << 16) | GetU16(p + 2);
}

uint64_t GetU64(const uint8_t* p)
{
	return (static_cast<uint64_t>(GetU32(p)) << 32) | GetU32(p + 4);
}

void EncodeHeader(const Header& h, uint8_t out[kHeaderSize])
{
	out[0] = h.type;
	out[1] = h.channel;
	out[2] = h.flags;
	out[3] = h.subtype;
	out[4] = static_cast<uint8_t>(h.length >> 24);
	out[5] = static_cast<uint8_t>(h.length >> 16);
	out[6] = static_cast<uint8_t>(h.length >> 8);
	out[7] = static_cast<uint8_t>(h.length);
}

Header DecodeHeader(const uint8_t in[kHeaderSize])
{
	Header h;
	h.type = in[0];
	h.channel = in[1];
	h.flags = in[2];
	h.subtype = in[3];
	h.length = GetU32(in + 4);
	return h;
}

bool HeaderIsAcceptable(const Header& h)
{
	return h.type == kMsgType && h.length <= kMaxFrameLength;
}

std::vector<uint8_t> EncodeCaps(const Caps& c)
{
	std::vector<uint8_t> v;
	PutU16(v, c.version);
	PutU16(v, 0); // reserved
	PutU32(v, c.channelMask);
	PutU32(v, c.maxFragment);
	return v;
}

bool DecodeCaps(const uint8_t* p, size_t len, Caps& out)
{
	if (len < 12)
		return false;
	out.version = GetU16(p);
	out.channelMask = GetU32(p + 4);
	out.maxFragment = GetU32(p + 8);
	return out.version >= 1;
}

static std::string Hex(uint32_t v)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "0x%x", v);
	return buf;
}

// ---------------------------------------------------------------------------
// ExtChannel
// ---------------------------------------------------------------------------

ExtChannel::ExtChannel(uint32_t localChannelMask)
	: m_localMask(localChannelMask | ChannelBit(kChControl))
{
	for (auto& perChannel : m_prioOf)
		perChannel.fill(-1);
}

ExtChannel::~ExtChannel()
{
	Stop();
}

void ExtChannel::SetHandler(uint8_t channel, Handler h)
{
	if (channel < kChannelCount && channel != kChControl)
		m_handlers[channel] = std::move(h);
}

uint64_t ExtChannel::NowMicros()
{
	using namespace std::chrono;
	return static_cast<uint64_t>(
		duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void ExtChannel::Open()
{
	{
		std::lock_guard<std::mutex> lk(m_qMutex);
		if (m_open.load() || m_stop)
			return;
		m_open = true;
		m_thread = std::thread(&ExtChannel::SenderLoop, this);
	}
	Caps caps;
	caps.channelMask = m_localMask.load();
	caps.maxFragment = kDefaultFragment;
	QueueControl(kCtlCaps, EncodeCaps(caps));
	Log(1, "extension channel opened");
}

void ExtChannel::Stop()
{
	{
		std::lock_guard<std::mutex> lk(m_qMutex);
		m_stop = true;
		m_open = false;
		for (size_t i = 0; i < m_queues.size(); ++i) {
			m_queues[i].clear();
			m_queuedBytes[i] = 0;
		}
	}
	m_qCond.notify_all();
	if (m_thread.joinable()) {
		if (m_thread.get_id() == std::this_thread::get_id())
			m_thread.detach();
		else
			m_thread.join();
	}
}

void ExtChannel::Reset()
{
	Stop();
	std::lock_guard<std::mutex> lk(m_qMutex);
	m_stop = false;
	m_failed = false;
	m_open = false;
	m_negotiated = false;
	m_negotiatedMask = 0;
	m_srttUs = 0;
	m_peerFragment = kDefaultFragment;
	m_pingSeq = 0;
	for (auto& perChannel : m_prioOf)
		perChannel.fill(-1);
	for (auto& perChannel : m_rx)
		for (auto& r : perChannel) {
			r.active = false;
			std::vector<uint8_t>().swap(r.data);
		}
}

size_t ExtChannel::QueuedBytes(ExtPriority prio) const
{
	std::lock_guard<std::mutex> lk(m_qMutex);
	return prio < kPriorityCount ? m_queuedBytes[prio] : 0;
}

bool ExtChannel::Send(ExtPriority prio, uint8_t channel, uint8_t subtype,
                      const void* data, size_t len)
{
	if (!m_open.load() || prio >= kPriorityCount || channel >= kChannelCount)
		return false;
	if (channel != kChControl && !ChannelUsable(channel))
		return false;
	if (len > kMaxMessageSize || (len > 0 && data == nullptr))
		return false;

	const size_t fragSize = std::max<size_t>(1024,
		std::min<uint32_t>(kDefaultFragment, m_peerFragment.load()));
	const uint8_t* src = static_cast<const uint8_t*>(data);

	{
		std::lock_guard<std::mutex> lk(m_qMutex);
		if (m_stop || m_failed)
			return false;

		int8_t& fixedPrio = m_prioOf[channel][subtype];
		if (fixedPrio < 0) {
			fixedPrio = static_cast<int8_t>(prio);
		} else if (fixedPrio != static_cast<int8_t>(prio)) {
			// Mixing priorities for one (channel, subtype) could reorder
			// fragments on the wire; this is a programming error.
			Log(0, "ExtChannel::Send: inconsistent priority for channel " +
				std::to_string(channel) + " subtype " + std::to_string(subtype));
			return false;
		}

		size_t offset = 0;
		bool first = true;
		do {
			const size_t n = (std::min)(fragSize, len - offset);
			Frame f;
			f.prio = static_cast<uint8_t>(prio);
			f.hdr.channel = channel;
			f.hdr.subtype = subtype;
			f.hdr.length = static_cast<uint32_t>(n);
			f.hdr.flags = static_cast<uint8_t>((first ? 0 : kFragCont) |
				(offset + n < len ? kFragMore : 0));
			if (n > 0)
				f.payload.assign(src + offset, src + offset + n);
			m_queues[prio].push_back(std::move(f));
			m_queuedBytes[prio] += n + kHeaderSize;
			offset += n;
			first = false;
		} while (offset < len);
	}
	m_qCond.notify_one();
	return true;
}

bool ExtChannel::QueueControl(uint8_t subtype, const std::vector<uint8_t>& payload)
{
	return Send(kPrioControl, kChControl, subtype, payload);
}

// Called with m_qMutex NOT held.
bool ExtChannel::PopFrame(bool allowLowPriority, Frame& out)
{
	std::lock_guard<std::mutex> lk(m_qMutex);
	if (m_stop)
		return false;
	const size_t last = allowLowPriority ? kPriorityCount : kPrioVideo;
	for (size_t p = 0; p < last; ++p) {
		if (!m_queues[p].empty()) {
			out = std::move(m_queues[p].front());
			m_queues[p].pop_front();
			m_queuedBytes[p] -= out.payload.size() + kHeaderSize;
			out.prio = static_cast<uint8_t>(p);
			return true;
		}
	}
	return false;
}

void ExtChannel::SenderLoop()
{
	using clock = std::chrono::steady_clock;
	auto nextPing = clock::now() + m_pingInterval;

	std::unique_lock<std::mutex> lk(m_qMutex);
	auto anyQueued = [this] {
		for (const auto& q : m_queues)
			if (!q.empty())
				return true;
		return false;
	};

	for (;;) {
		if (!m_stop && !anyQueued())
			m_qCond.wait_until(lk, nextPing, [&] { return m_stop || anyQueued(); });
		if (m_stop)
			break;

		if (clock::now() >= nextPing) {
			nextPing = clock::now() + m_pingInterval;
			if (m_negotiated.load()) {
				Frame f;
				f.prio = kPrioControl;
				f.hdr.channel = kChControl;
				f.hdr.subtype = kCtlPing;
				PutU32(f.payload, ++m_pingSeq);
				PutU64(f.payload, NowMicros());
				f.hdr.length = static_cast<uint32_t>(f.payload.size());
				m_queuedBytes[kPrioControl] += f.payload.size() + kHeaderSize;
				m_queues[kPrioControl].push_back(std::move(f));
			}
		}
		if (!anyQueued())
			continue;

		lk.unlock();

		// Send one batch. High-priority frames are always drained; video and
		// bulk frames stop once the byte budget is spent, so the transport
		// lock is released regularly and framebuffer updates get through.
		bool ok = true;
		size_t budget = m_batchBudget;
		BeginBatch();
		Frame f;
		while (PopFrame(budget > 0, f)) {
			uint8_t hdr[kHeaderSize];
			EncodeHeader(f.hdr, hdr);
			ok = WriteFrame(hdr, f.payload.empty() ? nullptr : f.payload.data(),
			                f.payload.size());
			if (!ok)
				break;
			if (f.prio >= kPrioVideo) {
				const size_t sz = f.payload.size() + kHeaderSize;
				budget = budget > sz ? budget - sz : 0;
			}
		}
		EndBatch();

		if (!ok) {
			lk.lock();
			m_failed = true;
			m_open = false;
			for (size_t i = 0; i < m_queues.size(); ++i) {
				m_queues[i].clear();
				m_queuedBytes[i] = 0;
			}
			lk.unlock();
			Log(0, "extension channel: transport write failed");
			OnTransportError();
			return;
		}

		// Give other writers (framebuffer updates) a chance at the lock.
		std::this_thread::yield();
		lk.lock();
	}
}

ExtChannel::RxResult ExtChannel::OnFrame(const Header& h, std::vector<uint8_t>& payload)
{
	if (h.channel >= kChannelCount)
		return RxResult::Ok; // channel from a newer peer: ignore

	Reassembly& r = m_rx[h.channel][h.subtype];
	const bool cont = (h.flags & kFragCont) != 0;
	const bool more = (h.flags & kFragMore) != 0;

	std::vector<uint8_t> message;
	if (!cont) {
		if (r.active)
			return RxResult::ProtocolError; // previous message never finished
		if (more) {
			r.active = true;
			r.data = std::move(payload);
			return RxResult::Ok;
		}
		message = std::move(payload);
	} else {
		if (!r.active)
			return RxResult::ProtocolError; // continuation without a start
		if (r.data.size() + payload.size() > kMaxMessageSize)
			return RxResult::ProtocolError;
		r.data.insert(r.data.end(), payload.begin(), payload.end());
		if (more)
			return RxResult::Ok;
		r.active = false;
		message.swap(r.data);
	}

	if (h.channel == kChControl) {
		HandleControl(h.subtype, message);
	} else if ((m_localMask.load() & ChannelBit(h.channel)) && m_handlers[h.channel]) {
		m_handlers[h.channel](h.subtype, message);
	}
	return RxResult::Ok;
}

void ExtChannel::HandleControl(uint8_t subtype, std::vector<uint8_t>& payload)
{
	switch (subtype) {
	case kCtlCaps: {
		Caps peer;
		if (!DecodeCaps(payload.data(), payload.size(), peer)) {
			Log(0, "extension channel: malformed Caps ignored");
			return;
		}
		m_peerFragment = std::max<uint32_t>(1024, peer.maxFragment);
		m_negotiatedMask = (m_localMask.load() & peer.channelMask) | ChannelBit(kChControl);
		// Viewer side: the server's Caps are what allow us to start sending.
		Open();
		const bool first = !m_negotiated.exchange(true);
		Log(1, "extension channel negotiated, peer version " +
			std::to_string(peer.version) + ", channels " + Hex(m_negotiatedMask.load()));
		if (first)
			OnNegotiated(m_negotiatedMask.load());
		break;
	}
	case kCtlPing:
		QueueControl(kCtlPong, payload);
		break;
	case kCtlPong:
		if (payload.size() >= 12) {
			const uint64_t sent = GetU64(payload.data() + 4);
			const uint64_t now = NowMicros();
			if (now >= sent) {
				const uint64_t rtt64 = std::min<uint64_t>(now - sent, UINT32_MAX);
				const uint32_t rtt = static_cast<uint32_t>(rtt64);
				const uint32_t srtt = m_srttUs.load();
				m_srttUs = srtt == 0 ? rtt : static_cast<uint32_t>((7ull * srtt + rtt) / 8);
			}
		}
		break;
	default:
		break; // unknown control message from a newer peer
	}
}

} // namespace extmsg
