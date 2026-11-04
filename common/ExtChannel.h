// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Extension message channel (see docs/design.md, section 1).
//
// All custom features (file clipboard, window capture, audio, video) travel
// inside a single bidirectional RFB message type, rfbExtMsg. Each frame is
//
//   CARD8  type      = rfbExtMsg (101)
//   CARD8  channel   (ExtChannelId)
//   CARD8  flags     (ExtFrameFlags)
//   CARD8  subtype   (meaning depends on channel)
//   CARD32 length    (big endian, payload bytes that follow)
//   CARD8  payload[length]
//
// Messages larger than the negotiated fragment size are split into several
// frames: the first has kFragMore set, the following ones kFragCont (and
// kFragMore except for the last one). Fragments of one message are never
// interleaved with another fragmented message of the same (channel, subtype),
// but frames of other channels may be sent in between, so small high-priority
// frames (audio) can overtake large low-priority messages (files).
//
// Negotiation: the viewer adds the pseudo-encoding rfbEncodingExtCaps to
// SetEncodings. Only then does the server send its Caps (channel 0). The
// viewer never sends rfbExtMsg before it has received the server's Caps, and
// then answers with its own Caps. This keeps both directions safe with stock
// UltraVNC peers, which drop the connection on unknown message types.
//
// This file is deliberately free of Windows and RFB headers so that it can be
// unit tested on any platform (see tests/ext_channel).

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace extmsg {

// Must match rfbExtMsg / rfbEncodingExtCaps in rfb/rfbproto.h.
constexpr uint8_t  kMsgType      = 101;
constexpr uint32_t kCapsEncoding = 0xFFFF8100u;

constexpr uint16_t kProtocolVersion = 1;
constexpr size_t   kHeaderSize      = 8;
// Fragment size we send and accept by default.
constexpr uint32_t kDefaultFragment = 64 * 1024;
// Hard limit on a single frame's payload; larger frames are a protocol error.
constexpr uint32_t kMaxFrameLength  = 1024 * 1024;
// Hard limit on a reassembled message.
constexpr uint32_t kMaxMessageSize  = 32 * 1024 * 1024;

enum ExtChannelId : uint8_t {
	kChControl  = 0,
	kChFileClip = 1,
	kChWindow   = 2,
	kChAudio    = 3,
	kChVideo    = 4,
	kChFiles    = 5, // file manager (file transfer window, file-only mode)
	kChannelCount
};

constexpr uint32_t ChannelBit(uint8_t ch) { return 1u << ch; }

enum ExtFrameFlags : uint8_t {
	kFragMore = 0x01, // more fragments of this message follow
	kFragCont = 0x02, // this frame continues a message started earlier
};

enum ExtControlSubtype : uint8_t {
	kCtlCaps = 1,
	kCtlPing = 2,
	kCtlPong = 3,
};

// Lower value = sent first.
enum ExtPriority : uint8_t {
	kPrioAudio   = 0,
	kPrioControl = 1,
	kPrioVideo   = 2,
	kPrioBulk    = 3,
	kPriorityCount
};

struct Header {
	uint8_t  type    = kMsgType;
	uint8_t  channel = 0;
	uint8_t  flags   = 0;
	uint8_t  subtype = 0;
	uint32_t length  = 0;
};

void EncodeHeader(const Header& h, uint8_t out[kHeaderSize]);
Header DecodeHeader(const uint8_t in[kHeaderSize]);
// True if a received header may be accepted (payload size within limits).
bool HeaderIsAcceptable(const Header& h);

struct Caps {
	uint16_t version     = kProtocolVersion;
	uint32_t channelMask = 0;
	uint32_t maxFragment = kDefaultFragment;
};
std::vector<uint8_t> EncodeCaps(const Caps& c);
// Unknown trailing bytes are ignored for forward compatibility.
bool DecodeCaps(const uint8_t* p, size_t len, Caps& out);

// Big-endian helpers for channel payloads.
void PutU16(std::vector<uint8_t>& v, uint16_t x);
void PutU32(std::vector<uint8_t>& v, uint32_t x);
void PutU64(std::vector<uint8_t>& v, uint64_t x);
uint16_t GetU16(const uint8_t* p);
uint32_t GetU32(const uint8_t* p);
uint64_t GetU64(const uint8_t* p);

// One end of the extension channel. A transport (server: vncClient,
// viewer: ClientConnection) derives from it, implements WriteFrame() and
// feeds received frames into OnFrame().
//
// Threads:
//  - Send() may be called from any thread. It never touches the socket; it
//    only queues frames.
//  - A private sender thread drains the queues in priority order and calls
//    BeginBatch() / WriteFrame() / EndBatch().
//  - OnFrame() is called by the transport's reader thread; handlers run on
//    that thread.
class ExtChannel {
public:
	using Handler = std::function<void(uint8_t subtype, std::vector<uint8_t>& payload)>;

	explicit ExtChannel(uint32_t localChannelMask);
	virtual ~ExtChannel();

	ExtChannel(const ExtChannel&) = delete;
	ExtChannel& operator=(const ExtChannel&) = delete;

	// Channels this end supports, announced in Caps. Takes effect at the next
	// Open(); call it before Open() / after Reset().
	void SetLocalChannelMask(uint32_t mask) { m_localMask = mask | ChannelBit(kChControl); }
	uint32_t LocalChannelMask() const { return m_localMask.load(); }

	// Register the handler for complete messages on a channel (not Control).
	// Call before Open() / before frames can arrive.
	void SetHandler(uint8_t channel, Handler h);

	// Start the sender thread, allow sending and queue our Caps.
	// Server: call when the viewer announced rfbEncodingExtCaps.
	// Viewer: called automatically when the server's Caps arrive.
	// Idempotent.
	void Open();

	// Stop the sender thread and drop queued frames. Must not be called while
	// holding the lock taken in BeginBatch(). Idempotent. Derived classes must
	// call it in their destructor.
	void Stop();

	// Stop() and return to the initial state so that the object can serve a
	// new connection (viewer auto-reconnect). Handlers are kept. Must not be
	// called while the reader thread is inside OnFrame().
	void Reset();

	// Queue a message. Returns false if the channel is closed, the channel was
	// not negotiated with the peer (Control only needs Open()), or the payload
	// is too large. For a given (channel, subtype), always use the same
	// priority, otherwise fragments could be reordered.
	bool Send(ExtPriority prio, uint8_t channel, uint8_t subtype,
	          const void* data, size_t len);
	bool Send(ExtPriority prio, uint8_t channel, uint8_t subtype,
	          const std::vector<uint8_t>& payload) {
		return Send(prio, channel, subtype, payload.data(), payload.size());
	}

	enum class RxResult { Ok, ProtocolError };
	// Feed one received frame (header already validated by HeaderIsAcceptable).
	RxResult OnFrame(const Header& h, std::vector<uint8_t>& payload);

	bool IsOpen() const { return m_open.load(); }
	// True once both sides have exchanged Caps.
	bool IsNegotiated() const { return m_negotiated.load(); }
	// Channels both sides support (valid once negotiated).
	uint32_t NegotiatedMask() const { return m_negotiatedMask.load(); }
	bool ChannelUsable(uint8_t ch) const {
		return IsNegotiated() && (NegotiatedMask() & ChannelBit(ch)) != 0;
	}
	// Smoothed round-trip time in microseconds, 0 if not measured yet.
	uint32_t RttMicros() const { return m_srttUs.load(); }
	// Bytes queued but not yet handed to the transport, per priority.
	size_t QueuedBytes(ExtPriority prio) const;

	// Tunables (set before Open()).
	void SetPingInterval(std::chrono::milliseconds ms) { m_pingInterval = ms; }
	void SetBatchBudget(size_t bytes) { m_batchBudget = bytes; }

protected:
	// Send one complete frame (header + payload) atomically with respect to
	// other writers on the socket. Return false on a fatal socket error.
	virtual bool WriteFrame(const uint8_t header[kHeaderSize],
	                        const uint8_t* payload, size_t len) = 0;
	// Bracket a series of WriteFrame() calls, e.g. to hold the server's
	// update lock so that frames go out between framebuffer updates.
	virtual void BeginBatch() {}
	virtual void EndBatch() {}
	virtual void OnTransportError() {}
	virtual void OnNegotiated(uint32_t /*mask*/) {}
	virtual void Log(int /*level*/, const std::string& /*msg*/) {}

private:
	struct Frame {
		uint8_t prio = kPrioBulk;
		Header hdr;
		std::vector<uint8_t> payload;
	};
	struct Reassembly {
		bool active = false;
		std::vector<uint8_t> data;
	};

	void SenderLoop();
	bool PopFrame(bool allowLowPriority, Frame& out);
	void HandleControl(uint8_t subtype, std::vector<uint8_t>& payload);
	bool QueueControl(uint8_t subtype, const std::vector<uint8_t>& payload);
	static uint64_t NowMicros();

	std::atomic<uint32_t> m_localMask;
	std::array<Handler, kChannelCount> m_handlers;

	// Sending side.
	mutable std::mutex m_qMutex;
	std::condition_variable m_qCond;
	std::array<std::deque<Frame>, kPriorityCount> m_queues;
	std::array<size_t, kPriorityCount> m_queuedBytes{};
	std::array<std::array<int8_t, 256>, kChannelCount> m_prioOf{}; // -1 = unset
	std::thread m_thread;
	bool m_stop = false;
	bool m_failed = false;
	std::atomic<bool> m_open{false};
	std::atomic<uint32_t> m_peerFragment{kDefaultFragment};
	std::chrono::milliseconds m_pingInterval{2000};
	size_t m_batchBudget = 256 * 1024;
	uint32_t m_pingSeq = 0;

	// Receiving side (reader thread only).
	std::array<std::array<Reassembly, 256>, kChannelCount> m_rx;

	// Negotiation state.
	std::atomic<bool> m_negotiated{false};
	std::atomic<uint32_t> m_negotiatedMask{0};
	std::atomic<uint32_t> m_srttUs{0};
};

} // namespace extmsg
