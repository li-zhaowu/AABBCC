// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// End-to-end test of single window mode on the server side
// (winvnc/winvnc/vncWindowSession + vncWindowCapture) on Windows or Wine.
// A fake viewer receives the messages, rebuilds the canvas from the tiles
// and acknowledges frames. Checks: window list, canvas content, updates,
// an owned pop-up composited into the canvas, pointer input mapped from
// canvas to screen, minimize / restore, closing, ending a session.
//
// Build: tests/window_e2e/run.sh (MinGW + Wine).

#include "../../winvnc/winvnc/vncWindowSession.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <cstring>

using namespace window;

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

// --- test windows -----------------------------------------------------------

static POINT g_lastClick = { -1, -1 };
static int g_clicks = 0;

static void Paint(HWND hwnd, HDC dc)
{
	RECT r;
	GetClientRect(hwnd, &r);
	const COLORREF c = static_cast<COLORREF>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	HBRUSH b = CreateSolidBrush(c);
	FillRect(dc, &r, b);
	DeleteObject(b);
}

static LRESULT CALLBACK TestProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		Paint(hwnd, dc);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_PRINTCLIENT:
		Paint(hwnd, reinterpret_cast<HDC>(wp));
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_LBUTTONDOWN:
		g_lastClick.x = static_cast<short>(LOWORD(lp));
		g_lastClick.y = static_cast<short>(HIWORD(lp));
		++g_clicks;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND MakeWindow(const wchar_t* title, DWORD style, HWND owner, int x, int y, int w, int h, COLORREF color)
{
	HWND hwnd = CreateWindowExW(0, L"UvncWindowTest", title, style, x, y, w, h, owner, NULL,
		GetModuleHandleW(NULL), NULL);
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, color);
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	UpdateWindow(hwnd);
	return hwnd;
}

static void Pump(int ms)
{
	const DWORD end = GetTickCount() + ms;
	while (static_cast<int>(end - GetTickCount()) > 0) {
		MSG m;
		while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
		Sleep(5);
	}
}

template <class Pred>
static bool WaitFor(Pred p, int ms = 5000)
{
	for (int i = 0; i < ms / 20; ++i) {
		if (p())
			return true;
		Pump(20);
	}
	return p();
}

// --- fake viewer --------------------------------------------------------------

struct FakeViewer {
	CRITICAL_SECTION cs;
	vncWindowSession* session = nullptr;
	std::vector<Entry> list;
	bool gotList = false;
	std::vector<State> states;
	uint32_t sid = 0;
	int w = 0, h = 0;
	std::vector<uint8_t> canvas;
	int frames = 0;

	FakeViewer() { InitializeCriticalSection(&cs); }

	bool OnSend(uint8_t subtype, const std::vector<uint8_t>& p)
	{
		uint32_t ackSid = 0, ackFrame = 0;
		bool ack = false;
		EnterCriticalSection(&cs);
		if (subtype == kList) {
			gotList = DecodeList(p.data(), p.size(), list);
		} else if (subtype == kState) {
			State st;
			if (DecodeState(p.data(), p.size(), st)) {
				states.push_back(st);
				if (st.state == kStateActive)
					sid = st.sessionId;
			}
		} else if (subtype == kCanvas) {
			Canvas c;
			if (DecodeCanvas(p.data(), p.size(), c) && c.sessionId == sid) {
				w = c.width;
				h = c.height;
				canvas.assign(size_t(w) * h * 4, 0);
			}
		} else if (subtype == kFrame) {
			FrameHeader fh;
			if (DecodeFrameHeader(p.data(), p.size(), fh) && fh.sessionId == sid) {
				std::vector<uint8_t> raw;
				if (fh.canvasWidth == w && fh.canvasHeight == h &&
				    Decompress(p.data() + fh.dataOffset, p.size() - fh.dataOffset, fh.rawSize, raw) &&
				    UnpackTiles(raw.data(), raw.size(), fh.tiles, canvas.data(), w, h, w * 4))
					++frames;
				ack = true;
				ackSid = fh.sessionId;
				ackFrame = fh.frameNo;
			}
		}
		LeaveCriticalSection(&cs);
		if (ack) {
			std::vector<uint8_t> a = EncodeAck(ackSid, ackFrame);
			session->OnMessage(kAck, a);
		}
		return true;
	}

	uint32_t Pixel(int x, int y)
	{
		EnterCriticalSection(&cs);
		uint32_t v = 0;
		if (x >= 0 && y >= 0 && x < w && y < h)
			memcpy(&v, &canvas[(size_t(y) * w + x) * 4], 4);
		LeaveCriticalSection(&cs);
		return v & 0xFFFFFF; // BGR
	}
	bool HasState(uint8_t s)
	{
		EnterCriticalSection(&cs);
		bool found = false;
		for (const State& st : states)
			found = found || st.state == s;
		LeaveCriticalSection(&cs);
		return found;
	}
	void ClearStates()
	{
		EnterCriticalSection(&cs);
		states.clear();
		LeaveCriticalSection(&cs);
	}
};

// Deliver a message like the reader thread would: on another thread, while
// this thread keeps pumping messages (the test windows live on it, and
// PrintWindow needs them to answer).
static void Send(vncWindowSession& s, uint8_t subtype, std::vector<uint8_t> p)
{
	std::atomic<bool> done{false};
	std::thread t([&] {
		s.OnMessage(subtype, p);
		done = true;
	});
	while (!done)
		Pump(10);
	t.join();
}

int main(int argc, char** argv)
{
	const bool verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = &TestProc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"UvncWindowTest";
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	RegisterClassExW(&wc);

	HWND main = MakeWindow(L"uvnc window test", WS_OVERLAPPEDWINDOW, NULL, 100, 100, 400, 300, RGB(255, 0, 0));
	Pump(300);

	FakeViewer viewer;
	vncWindowSession::Callbacks cb;
	cb.send = [&](extmsg::ExtPriority, uint8_t subtype, const std::vector<uint8_t>& p) {
		return viewer.OnSend(subtype, p);
	};
	cb.log = [&](int level, const std::string& m) {
		if (verbose || level == 0)
			std::fprintf(stderr, "[server] %s\n", m.c_str());
	};
	bool sessionStartCalled = false;
	cb.onSessionStart = [&] { sessionStartCalled = true; };
	cb.allowOwnProcess = true; // the test windows live in this process
	// Video transport doubles (phase 3b).
	bool videoAvailable = true;
	int videoBegins = 0, videoEnds = 0;
	std::atomic<int> videoPushes{0};
	std::atomic<int> lastPushW{0};
	cb.beginVideo = [&] {
		if (videoAvailable)
			++videoBegins;
		return videoAvailable;
	};
	cb.pushVideo = [&](const uint8_t*, int w, int, int) {
		lastPushW = w;
		++videoPushes;
	};
	cb.endVideo = [&] { ++videoEnds; };
	vncWindowSession session(cb);
	viewer.session = &session;

	// 1. The list contains our window.
	Send(session, kListReq, {});
	CHECK(viewer.gotList);
	bool listed = false;
	for (const Entry& e : viewer.list)
		listed = listed || (e.handle == reinterpret_cast<uintptr_t>(main) && e.title == "uvnc window test");
	CHECK(listed);

	// 2. Invalid handle is refused.
	Send(session, kSelect, EncodeSelect(0x12345678));
	CHECK(WaitFor([&] { return viewer.HasState(kStateRefused); }, 1000));
	CHECK(!session.Active());

	// 3. Select the window: canvas and content.
	viewer.ClearStates();
	Send(session, kSelect, EncodeSelect(reinterpret_cast<uintptr_t>(main)));
	CHECK(WaitFor([&] { return viewer.HasState(kStateActive) && viewer.frames > 0; }));
	CHECK(session.Active() && sessionStartCalled);
	RECT client;
	GetClientRect(main, &client);
	POINT c0 = { 0, 0 };
	ClientToScreen(main, &c0);
	RECT frame;
	GetWindowRect(main, &frame);
	if (verbose)
		std::printf("canvas %dx%d, window %ldx%ld\n", viewer.w, viewer.h, frame.right - frame.left,
			frame.bottom - frame.top);
	CHECK(viewer.w > 300 && viewer.h > 200);
	// Middle of the client area, in canvas coordinates. Canvas origin is the
	// window's visible frame; find it from the window rect (no DWM in Wine).
	const int cx = (c0.x - frame.left) + 200, cy = (c0.y - frame.top) + 150;
	CHECK(WaitFor([&] { return viewer.Pixel(cx, cy) == 0xFF0000; }));
	std::printf("centre pixel after select: %06x\n", viewer.Pixel(cx, cy));

	// 4. Content change reaches the viewer.
	SetWindowLongPtrW(main, GWLP_USERDATA, RGB(0, 255, 0));
	InvalidateRect(main, NULL, TRUE);
	CHECK(WaitFor([&] { return viewer.Pixel(cx, cy) == 0x00FF00; }));

	// 5. An owned pop-up partly outside the window grows the canvas.
	const int oldW = viewer.w;
	HWND popup = MakeWindow(L"", WS_POPUP | WS_BORDER, main, frame.right - 50, frame.top + 40, 120, 80, RGB(0, 0, 255));
	Pump(100);
	CHECK(WaitFor([&] { return viewer.w > oldW; }));
	const int px = (frame.right - 50 - frame.left) + 60, py = 40 + 40;
	CHECK(WaitFor([&] { return viewer.Pixel(px, py) == 0x0000FF; }));
	std::printf("canvas with pop-up %dx%d, pop-up pixel %06x\n", viewer.w, viewer.h, viewer.Pixel(px, py));
	DestroyWindow(popup);
	CHECK(WaitFor([&] { return viewer.w == oldW; }));

	// 6. Pointer input lands in the window at the right place.
	g_clicks = 0;
	session.HandlePointer(cx, cy, 1, 0, 0);
	session.HandlePointer(cx, cy, 0, 0, 0);
	CHECK(WaitFor([&] { return g_clicks > 0; }, 2000));
	std::printf("click at canvas %d,%d arrived at client %ld,%ld\n", cx, cy, g_lastClick.x, g_lastClick.y);
	CHECK(g_lastClick.x >= 198 && g_lastClick.x <= 202 && g_lastClick.y >= 148 && g_lastClick.y <= 152);
	// A press outside the window and its pop-ups is dropped.
	g_clicks = 0;
	session.HandlePointer(viewer.w + 500, 5, 1, 0, 0);
	session.HandlePointer(viewer.w + 500, 5, 0, 0, 0);
	Pump(300);
	CHECK(g_clicks == 0);

	// 7. Minimize, then restore on request.
	viewer.ClearStates();
	ShowWindow(main, SW_MINIMIZE);
	CHECK(WaitFor([&] { return viewer.HasState(kStateMinimized); }));
	Send(session, kRestore, {});
	CHECK(WaitFor([&] { return viewer.HasState(kStateActive); }));
	CHECK(!IsIconic(main));

	// 8. Ending on request.
	viewer.ClearStates();
	Send(session, kSelect, EncodeSelect(0));
	CHECK(viewer.HasState(kStateEnded));
	CHECK(!session.Active());

	// 9. Closing the window ends the session.
	viewer.ClearStates();
	Send(session, kSelect, EncodeSelect(reinterpret_cast<uintptr_t>(main)));
	CHECK(WaitFor([&] { return viewer.HasState(kStateActive); }));
	DestroyWindow(main);
	CHECK(WaitFor([&] { return viewer.HasState(kStateClosed); }));
	CHECK(!session.Active());

	// 10. Video transport: the canvas goes to the encoder, only on changes.
	HWND vid = MakeWindow(L"uvnc video window", WS_OVERLAPPEDWINDOW, NULL, 150, 120, 320, 240, RGB(10, 20, 30));
	Pump(200);
	viewer.ClearStates();
	const int tileFramesBefore = viewer.frames;
	Send(session, kSelect, EncodeSelect(reinterpret_cast<uintptr_t>(vid), kTransportVideo));
	CHECK(WaitFor([&] { return viewer.HasState(kStateActive) && videoPushes > 0; }));
	CHECK(videoBegins == 1 && lastPushW == viewer.w);
	Pump(500);
	const int pushesStatic = videoPushes;
	Pump(500);
	CHECK(videoPushes == pushesStatic); // static window: no new pictures
	SetWindowLongPtrW(vid, GWLP_USERDATA, RGB(200, 100, 0));
	InvalidateRect(vid, NULL, TRUE);
	CHECK(WaitFor([&] { return videoPushes > pushesStatic; }));
	CHECK(viewer.frames == tileFramesBefore); // no tiles while video works
	std::printf("video transport: %d pictures pushed\n", videoPushes.load());

	// The encoder fails: continue with tiles, starting with the whole canvas.
	session.VideoFailed();
	CHECK(WaitFor([&] { return viewer.frames > tileFramesBefore; }));
	RECT vr;
	GetWindowRect(vid, &vr);
	POINT vc = { 0, 0 };
	ClientToScreen(vid, &vc);
	CHECK(WaitFor([&] { return viewer.Pixel(vc.x - vr.left + 100, vc.y - vr.top + 100) == 0xC86400; }));
	Send(session, kSelect, EncodeSelect(0));
	CHECK(videoEnds == 0); // fell back: the encoder was already gone

	// 11. Video requested but not available: tiles from the start.
	videoAvailable = false;
	viewer.ClearStates();
	const int before = viewer.frames;
	Send(session, kSelect, EncodeSelect(reinterpret_cast<uintptr_t>(vid), kTransportVideo));
	CHECK(WaitFor([&] { return viewer.HasState(kStateActive) && viewer.frames > before; }));
	// And a video session that ends normally stops the encoder.
	videoAvailable = true;
	Send(session, kSelect, EncodeSelect(reinterpret_cast<uintptr_t>(vid), kTransportVideo));
	CHECK(WaitFor([&] { return videoBegins == 2; }));
	Send(session, kSelect, EncodeSelect(0));
	CHECK(videoEnds == 1);
	DestroyWindow(vid);

	session.Shutdown();
	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all window_e2e tests passed\n");
	return 0;
}
