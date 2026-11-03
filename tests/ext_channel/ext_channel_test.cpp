// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Unit tests for common/ExtChannel. Platform independent; build with
//   tests/ext_channel/run.sh          (g++ / clang++)
// or compile ext_channel_test.cpp + ../../common/ExtChannel.cpp with MSVC.

#include "../../common/ExtChannel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace extmsg;
using namespace std::chrono_literals;

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

template <class Pred>
static bool WaitFor(Pred p, std::chrono::milliseconds timeout = 3000ms)
{
	auto end = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < end) {
		if (p())
			return true;
		std::this_thread::sleep_for(1ms);
	}
	return p();
}

// A byte pipe standing in for one direction of the TCP connection.
class Pipe {
public:
	void Write(const uint8_t* p, size_t n)
	{
		std::lock_guard<std::mutex> lk(m);
		buf.insert(buf.end(), p, p + n);
		cv.notify_all();
	}
	// Returns false when closed and drained.
	bool ReadExact(uint8_t* out, size_t n)
	{
		std::unique_lock<std::mutex> lk(m);
		cv.wait(lk, [&] { return buf.size() >= n || closed; });
		if (buf.size() < n)
			return false;
		std::copy(buf.begin(), buf.begin() + n, out);
		buf.erase(buf.begin(), buf.begin() + n);
		return true;
	}
	void Close()
	{
		std::lock_guard<std::mutex> lk(m);
		closed = true;
		cv.notify_all();
	}

private:
	std::mutex m;
	std::condition_variable cv;
	std::deque<uint8_t> buf;
	bool closed = false;
};

// Test endpoint: writes frames into `out`, reads frames from `in` on its own
// reader thread, like vncClientThread / ClientConnection::run_undetached.
class Endpoint : public ExtChannel {
public:
	Endpoint(const char* name, uint32_t mask, Pipe& in, Pipe& out)
		: ExtChannel(mask), m_name(name), m_in(in), m_out(out)
	{
	}
	~Endpoint() override
	{
		Stop();
		m_in.Close();
		if (m_reader.joinable())
			m_reader.join();
	}

	void StartReader()
	{
		m_reader = std::thread([this] {
			for (;;) {
				uint8_t hb[kHeaderSize];
				if (!m_in.ReadExact(hb, sizeof(hb)))
					return;
				Header h = DecodeHeader(hb);
				if (!HeaderIsAcceptable(h)) {
					rxErrors++;
					return;
				}
				std::vector<uint8_t> payload(h.length);
				if (h.length && !m_in.ReadExact(payload.data(), h.length))
					return;
				if (OnFrame(h, payload) != RxResult::Ok) {
					rxErrors++;
					return;
				}
			}
		});
	}

	// Test hooks.
	std::mutex gate;              // held by a test to pause the sender
	std::atomic<bool> failWrites{false};
	std::atomic<int> transportErrors{0};
	std::atomic<int> rxErrors{0};
	std::atomic<uint32_t> negotiatedMask{0};
	std::mutex logMutex;
	std::vector<std::pair<uint8_t, uint8_t>> written; // (channel, flags)

protected:
	bool WriteFrame(const uint8_t header[kHeaderSize], const uint8_t* payload,
	                size_t len) override
	{
		if (failWrites)
			return false;
		{
			std::lock_guard<std::mutex> lk(logMutex);
			written.emplace_back(header[1], header[2]);
		}
		m_out.Write(header, kHeaderSize);
		if (len)
			m_out.Write(payload, len);
		return true;
	}
	void BeginBatch() override { gate.lock(); }
	void EndBatch() override { gate.unlock(); }
	void OnTransportError() override { transportErrors++; }
	void OnNegotiated(uint32_t mask) override { negotiatedMask = mask; }
	void Log(int level, const std::string& msg) override
	{
		if (std::getenv("EXT_TEST_VERBOSE") || level == 0)
			std::fprintf(stderr, "[%s] %s\n", m_name, msg.c_str());
	}

private:
	const char* m_name;
	Pipe& m_in;
	Pipe& m_out;
	std::thread m_reader;
};

struct Pair {
	Pipe toViewer, toServer;
	std::unique_ptr<Endpoint> server, viewer;
	Pair(uint32_t serverMask, uint32_t viewerMask)
	{
		server.reset(new Endpoint("server", serverMask, toServer, toViewer));
		viewer.reset(new Endpoint("viewer", viewerMask, toViewer, toServer));
	}
	void Start()
	{
		server->StartReader();
		viewer->StartReader();
	}
	bool Negotiate()
	{
		Start();
		server->Open(); // the viewer announced rfbEncodingExtCaps
		return WaitFor([&] { return server->IsNegotiated() && viewer->IsNegotiated(); });
	}
};

static std::vector<uint8_t> Pattern(size_t n, uint32_t seed)
{
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; ++i) {
		seed = seed * 1103515245u + 12345u;
		v[i] = static_cast<uint8_t>(seed >> 16);
	}
	return v;
}

static void TestHeaderAndCaps()
{
	Header h;
	h.channel = 3;
	h.flags = kFragMore;
	h.subtype = 7;
	h.length = 0x01020304;
	uint8_t b[kHeaderSize];
	EncodeHeader(h, b);
	CHECK(b[0] == 101 && b[4] == 1 && b[7] == 4);
	Header d = DecodeHeader(b);
	CHECK(d.channel == 3 && d.flags == kFragMore && d.subtype == 7 && d.length == 0x01020304);
	CHECK(!HeaderIsAcceptable(d)); // > kMaxFrameLength
	d.length = 10;
	CHECK(HeaderIsAcceptable(d));

	Caps c;
	c.channelMask = 0x1f;
	c.maxFragment = 4096;
	auto enc = EncodeCaps(c);
	enc.push_back(0xAA); // trailing data from a newer peer must be ignored
	Caps out;
	CHECK(DecodeCaps(enc.data(), enc.size(), out));
	CHECK(out.version == kProtocolVersion && out.channelMask == 0x1f && out.maxFragment == 4096);
	CHECK(!DecodeCaps(enc.data(), 5, out));
}

static void TestNegotiation()
{
	const uint32_t s = ChannelBit(kChFileClip) | ChannelBit(kChVideo) | ChannelBit(kChAudio);
	const uint32_t v = ChannelBit(kChFileClip) | ChannelBit(kChAudio) | ChannelBit(kChWindow);
	Pair p(s, v);

	// Nobody may send before Open / negotiation.
	CHECK(!p.viewer->Send(kPrioControl, kChControl, 9, nullptr, 0));
	CHECK(!p.server->Send(kPrioBulk, kChFileClip, 1, nullptr, 0));

	CHECK(p.Negotiate());
	const uint32_t expect = ChannelBit(kChControl) | ChannelBit(kChFileClip) | ChannelBit(kChAudio);
	CHECK(p.server->NegotiatedMask() == expect);
	CHECK(p.viewer->NegotiatedMask() == expect);
	CHECK(WaitFor([&] { return p.server->negotiatedMask == expect; }));
	CHECK(p.viewer->ChannelUsable(kChAudio));
	CHECK(!p.viewer->ChannelUsable(kChVideo));
	CHECK(!p.server->Send(kPrioVideo, kChVideo, 1, nullptr, 0)); // not negotiated

	// Open is idempotent: a second SetEncodings must not break anything.
	p.server->Open();
	CHECK(p.server->IsNegotiated());
}

static void TestFragmentationRoundTrip()
{
	const uint32_t mask = ChannelBit(kChFileClip) | ChannelBit(kChAudio);
	Pair p(mask, mask);

	std::mutex m;
	std::vector<std::pair<uint8_t, std::vector<uint8_t>>> got;
	p.viewer->SetHandler(kChFileClip, [&](uint8_t sub, std::vector<uint8_t>& data) {
		std::lock_guard<std::mutex> lk(m);
		got.emplace_back(sub, std::move(data));
	});
	CHECK(p.Negotiate());

	auto big = Pattern(1000 * 1000 + 17, 1);  // ~16 fragments
	auto exact = Pattern(kDefaultFragment, 2); // exactly one fragment
	auto small = Pattern(5, 3);
	CHECK(p.server->Send(kPrioBulk, kChFileClip, 1, big));
	CHECK(p.server->Send(kPrioBulk, kChFileClip, 2, exact));
	CHECK(p.server->Send(kPrioBulk, kChFileClip, 3, nullptr, 0));
	CHECK(p.server->Send(kPrioBulk, kChFileClip, 1, small));

	CHECK(WaitFor([&] { std::lock_guard<std::mutex> lk(m); return got.size() == 4; }));
	std::lock_guard<std::mutex> lk(m);
	if (got.size() == 4) {
		CHECK(got[0].first == 1 && got[0].second == big);
		CHECK(got[1].first == 2 && got[1].second == exact);
		CHECK(got[2].first == 3 && got[2].second.empty());
		CHECK(got[3].first == 1 && got[3].second == small);
	}
	CHECK(p.viewer->rxErrors == 0);
}

static void TestPriority()
{
	const uint32_t mask = ChannelBit(kChFileClip) | ChannelBit(kChAudio);
	Pair p(mask, mask);

	std::mutex m;
	std::vector<uint8_t> order; // channel of each completed message
	auto rec = [&](uint8_t ch) {
		return [&, ch](uint8_t, std::vector<uint8_t>&) {
			std::lock_guard<std::mutex> lk(m);
			order.push_back(ch);
		};
	};
	p.viewer->SetHandler(kChFileClip, rec(kChFileClip));
	p.viewer->SetHandler(kChAudio, rec(kChAudio));
	CHECK(p.Negotiate());

	// Pause the server's sender, queue a 3 MB bulk message, then audio.
	{
		std::lock_guard<std::mutex> gate(p.server->gate);
		CHECK(p.server->Send(kPrioBulk, kChFileClip, 1, Pattern(3 * 1024 * 1024, 4)));
		std::this_thread::sleep_for(20ms);
		CHECK(p.server->Send(kPrioAudio, kChAudio, 1, Pattern(300, 5)));
	}
	CHECK(WaitFor([&] { std::lock_guard<std::mutex> lk(m); return order.size() == 2; }));
	{
		std::lock_guard<std::mutex> lk(m);
		CHECK(order.size() == 2 && order[0] == kChAudio && order[1] == kChFileClip);
	}

	// While bulk data streams, audio queued later must overtake the remaining
	// fragments: it has to be written before the bulk message's last fragment.
	{
		std::lock_guard<std::mutex> lk(p.server->logMutex);
		p.server->written.clear();
	}
	CHECK(p.server->Send(kPrioBulk, kChFileClip, 1, Pattern(8 * 1024 * 1024, 6)));
	std::this_thread::sleep_for(2ms);
	CHECK(p.server->Send(kPrioAudio, kChAudio, 1, Pattern(300, 7)));
	CHECK(WaitFor([&] { std::lock_guard<std::mutex> lk(m); return order.size() == 4; }));
	{
		std::lock_guard<std::mutex> lk(p.server->logMutex);
		size_t audioPos = SIZE_MAX, lastBulk = 0;
		for (size_t i = 0; i < p.server->written.size(); ++i) {
			if (p.server->written[i].first == kChAudio && audioPos == SIZE_MAX)
				audioPos = i;
			if (p.server->written[i].first == kChFileClip)
				lastBulk = i;
		}
		CHECK(audioPos != SIZE_MAX && audioPos < lastBulk);
	}
	CHECK(p.viewer->rxErrors == 0);
}

static void TestLocalMaskChange()
{
	Pair p(ChannelBit(kChFileClip), ChannelBit(kChFileClip));
	// The viewer disables the file clipboard before (re)connecting.
	p.viewer->SetLocalChannelMask(0);
	CHECK(p.viewer->LocalChannelMask() == ChannelBit(kChControl));
	CHECK(p.Negotiate());
	CHECK(!p.server->ChannelUsable(kChFileClip));
	CHECK(!p.viewer->ChannelUsable(kChFileClip));
}

static void TestPriorityConsistency()
{
	const uint32_t mask = ChannelBit(kChFileClip);
	Pair p(mask, mask);
	CHECK(p.Negotiate());
	CHECK(p.server->Send(kPrioBulk, kChFileClip, 4, nullptr, 0));
	CHECK(!p.server->Send(kPrioAudio, kChFileClip, 4, nullptr, 0));
	CHECK(p.server->Send(kPrioAudio, kChFileClip, 5, nullptr, 0)); // other subtype is fine
}

class RxOnly : public ExtChannel {
public:
	RxOnly() : ExtChannel(ChannelBit(kChFileClip)) {}
	~RxOnly() override { Stop(); }
	using ExtChannel::OnFrame;

protected:
	bool WriteFrame(const uint8_t*, const uint8_t*, size_t) override { return true; }
};

static void TestReceiverValidation()
{
	RxOnly rx;
	std::vector<uint8_t> data(10);
	Header h;

	h.channel = 200; // unknown channel from a newer peer
	CHECK(rx.OnFrame(h, data) == ExtChannel::RxResult::Ok);

	h.channel = kChFileClip;
	h.flags = kFragCont; // continuation without a start
	CHECK(rx.OnFrame(h, data) == ExtChannel::RxResult::ProtocolError);

	RxOnly rx2;
	h.flags = kFragMore;
	CHECK(rx2.OnFrame(h, data) == ExtChannel::RxResult::Ok);
	h.flags = 0; // new message while the previous one is unfinished
	CHECK(rx2.OnFrame(h, data) == ExtChannel::RxResult::ProtocolError);

	// Reassembly limit.
	RxOnly rx3;
	std::vector<uint8_t> chunk(kMaxFrameLength);
	h.flags = kFragMore;
	CHECK(rx3.OnFrame(h, chunk) == ExtChannel::RxResult::Ok);
	ExtChannel::RxResult r = ExtChannel::RxResult::Ok;
	h.flags = kFragCont | kFragMore;
	for (uint32_t i = 0; i < kMaxMessageSize / kMaxFrameLength + 1 && r == ExtChannel::RxResult::Ok; ++i) {
		chunk.assign(kMaxFrameLength, 0);
		r = rx3.OnFrame(h, chunk);
	}
	CHECK(r == ExtChannel::RxResult::ProtocolError);
}

static void TestPing()
{
	Pair p(0, 0);
	p.server->SetPingInterval(20ms);
	p.viewer->SetPingInterval(20ms);
	CHECK(p.Negotiate());
	CHECK(WaitFor([&] { return p.server->RttMicros() > 0 && p.viewer->RttMicros() > 0; }));
}

static void TestTransportFailure()
{
	const uint32_t mask = ChannelBit(kChFileClip);
	Pair p(mask, mask);
	CHECK(p.Negotiate());
	p.server->failWrites = true;
	CHECK(p.server->Send(kPrioBulk, kChFileClip, 1, Pattern(10, 8)));
	CHECK(WaitFor([&] { return p.server->transportErrors == 1; }));
	CHECK(!p.server->Send(kPrioBulk, kChFileClip, 1, Pattern(10, 9)));
	CHECK(!p.server->IsOpen());
}

static void TestStopWhileQueued()
{
	const uint32_t mask = ChannelBit(kChFileClip);
	Pair p(mask, mask);
	CHECK(p.Negotiate());
	for (int i = 0; i < 20; ++i)
		p.server->Send(kPrioBulk, kChFileClip, 1, Pattern(1024 * 1024, i));
	p.server->Stop(); // must return promptly and not deadlock
	CHECK(!p.server->IsOpen());
	CHECK(!p.server->Send(kPrioBulk, kChFileClip, 1, nullptr, 0));
	p.server->Open(); // a stopped channel stays stopped
	CHECK(!p.server->IsOpen());
}

static void TestResetForReconnect()
{
	const uint32_t mask = ChannelBit(kChFileClip);
	Pipe a1, b1;
	Endpoint server("server", mask, a1, b1);
	Endpoint viewer("viewer", mask, b1, a1);
	std::atomic<int> received{0};
	viewer.SetHandler(kChFileClip, [&](uint8_t, std::vector<uint8_t>&) { received++; });
	server.StartReader();
	viewer.StartReader();
	server.Open();
	CHECK(WaitFor([&] { return viewer.IsNegotiated() && server.IsNegotiated(); }));

	// Viewer side of a reconnect: a half-received message must be forgotten.
	viewer.Reset();
	CHECK(!viewer.IsOpen() && !viewer.IsNegotiated() && viewer.NegotiatedMask() == 0);
	CHECK(!viewer.Send(kPrioControl, kChControl, 9, nullptr, 0));

	// The server end restarts as well; Caps flow again and the viewer reopens.
	server.Reset();
	server.Open();
	CHECK(WaitFor([&] { return viewer.IsNegotiated() && server.IsNegotiated(); }));
	CHECK(server.Send(kPrioBulk, kChFileClip, 1, Pattern(200 * 1024, 10)));
	CHECK(WaitFor([&] { return received == 1; }));
}

int main()
{
	TestHeaderAndCaps();
	TestNegotiation();
	TestFragmentationRoundTrip();
	TestPriority();
	TestPriorityConsistency();
	TestLocalMaskChange();
	TestReceiverValidation();
	TestPing();
	TestTransportFailure();
	TestStopWhileQueued();
	TestResetForReconnect();

	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all ext_channel tests passed\n");
	return 0;
}
