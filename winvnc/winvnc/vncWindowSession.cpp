// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "vncWindowSession.h"
#include "vncWindowCapture.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _MSC_VER
#pragma comment(lib, "dwmapi.lib")
#endif

using namespace window;
using extmsg::kPrioControl;
using extmsg::kPrioVideo;

namespace {

// Office use (tiles): 30 pictures per second is plenty and keeps the CPU
// low. Video transport: up to 60, the encoder paces itself.
constexpr auto kFrameInterval = std::chrono::milliseconds(33);
constexpr auto kVideoFrameInterval = std::chrono::milliseconds(16);
// Frames sent but not yet acknowledged by the viewer.
constexpr uint32_t kMaxInFlight = 2;
// Consecutive capture errors (outside desktop switches) before giving up.
constexpr int kMaxErrors = 90;

std::string WideToUtf8(const wchar_t* w)
{
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	if (n <= 1)
		return std::string();
	std::string s(n - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
	return s;
}

// Puts the calling thread on the desktop the user sees (windows, the screen
// and SendInput are per desktop) and keeps that desktop handle open while
// the thread uses it.
class InputDesktop {
public:
	~InputDesktop()
	{
		if (m_desk)
			CloseDesktop(m_desk);
	}
	bool Attach()
	{
		HDESK d = OpenInputDesktop(0, FALSE, GENERIC_ALL);
		if (!d)
			return false;
		if (!SetThreadDesktop(d)) {
			CloseDesktop(d);
			return false;
		}
		if (m_desk)
			CloseDesktop(m_desk); // no longer the thread's desktop
		m_desk = d;
		return true;
	}

private:
	HDESK m_desk = NULL;
};

bool InputDesktopIsDefault()
{
	HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
	if (!d)
		return false; // Winlogon (UAC, lock screen) cannot be opened
	wchar_t name[64] = {};
	DWORD needed = 0;
	GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &needed);
	CloseDesktop(d);
	return _wcsicmp(name, L"Default") == 0;
}

std::string ProgramName(DWORD pid)
{
	HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!p)
		return std::string();
	wchar_t path[MAX_PATH * 2];
	DWORD n = ARRAYSIZE(path);
	std::string result;
	if (QueryFullProcessImageNameW(p, 0, path, &n)) {
		const wchar_t* slash = wcsrchr(path, L'\\');
		result = WideToUtf8(slash ? slash + 1 : path);
	}
	CloseHandle(p);
	return result;
}

// Title without sending a message to the window (GetWindowText does that
// for windows of our own process, which can block on a busy thread).
std::wstring TitleOf(HWND hwnd)
{
	typedef int(WINAPI* PFN)(HWND, LPWSTR, int);
	static PFN internalGetWindowText = reinterpret_cast<PFN>(reinterpret_cast<void*>(
		GetProcAddress(GetModuleHandleW(L"user32.dll"), "InternalGetWindowText")));
	wchar_t title[512] = {};
	if (internalGetWindowText)
		internalGetWindowText(hwnd, title, ARRAYSIZE(title));
	return title;
}

struct ListContext {
	std::vector<Entry>* out;
	bool allowOwnProcess;
};

// Windows a user would call "a window": visible, top level, with a title,
// not a tool window, not cloaked (hidden store apps), not the shell, not
// UltraVNC itself.
bool IsPickable(HWND hwnd, bool allowOwnProcess)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && !allowOwnProcess)
		return false;
	if (!IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd)
		return false;
	const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW))
		return false;
	if (GetWindow(hwnd, GW_OWNER) && !(ex & WS_EX_APPWINDOW))
		return false;
	DWORD cloaked = 0;
	if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
		return false;
	if (TitleOf(hwnd).empty())
		return false;
	wchar_t cls[64] = {};
	GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
	return wcscmp(cls, L"Progman") && wcscmp(cls, L"Shell_TrayWnd") && wcscmp(cls, L"WorkerW");
}

BOOL CALLBACK ListProc(HWND hwnd, LPARAM lp)
{
	auto* ctx = reinterpret_cast<ListContext*>(lp);
	std::vector<Entry>* out = ctx->out;
	if (!IsPickable(hwnd, ctx->allowOwnProcess) || out->size() >= kMaxListEntries)
		return TRUE;
	Entry e;
	e.handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
	GetWindowThreadProcessId(hwnd, reinterpret_cast<DWORD*>(&e.pid));
	e.minimized = IsIconic(hwnd) != FALSE;
	e.title = WideToUtf8(TitleOf(hwnd).c_str());
	e.program = ProgramName(e.pid);
	out->push_back(std::move(e));
	return TRUE;
}

} // namespace

vncWindowSession::vncWindowSession(Callbacks cb) : m_cb(std::move(cb)) {}

vncWindowSession::~vncWindowSession()
{
	Shutdown();
}

void vncWindowSession::Log(int level, const std::string& msg)
{
	if (m_cb.log)
		m_cb.log(level, "window: " + msg);
}

void vncWindowSession::SendState(uint32_t sessionId, uint8_t state, const std::string& message)
{
	State st;
	st.sessionId = sessionId;
	st.state = state;
	st.message = message;
	m_cb.send(kPrioControl, kState, EncodeState(st));
	if (state != kStateActive)
		Log(1, "session " + std::to_string(sessionId) + " state " + std::to_string(state) +
			(message.empty() ? "" : ": " + message));
}

void vncWindowSession::SendList()
{
	// Enumerate on a helper thread attached to the input desktop: the
	// reader thread may be elsewhere.
	std::vector<Entry> list;
	ListContext ctx = { &list, m_cb.allowOwnProcess };
	std::thread t([&] {
		InputDesktop desk;
		desk.Attach();
		EnumWindows(&ListProc, reinterpret_cast<LPARAM>(&ctx));
	});
	t.join();
	m_cb.send(kPrioControl, kList, EncodeList(list));
	Log(2, "listed " + std::to_string(list.size()) + " windows");
}

void vncWindowSession::OnMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	const uint8_t* p = payload.data();
	const size_t n = payload.size();
	switch (subtype) {
	case kListReq:
		SendList();
		break;
	case kSelect: {
		uint64_t handle = 0;
		uint8_t transport = kTransportTiles;
		if (DecodeSelect(p, n, handle, transport))
			Select(handle, transport);
		break;
	}
	case kAck: {
		uint32_t sid = 0, frame = 0;
		if (DecodeAck(p, n, sid, frame) && sid == m_sessionId.load() && frame > m_lastAck.load())
			m_lastAck = frame;
		m_cond.notify_all();
		break;
	}
	case kRestore: {
		HWND target;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			target = m_target;
		}
		if (target)
			ShowWindowAsync(target, SW_RESTORE);
		break;
	}
	default:
		break;
	}
}

void vncWindowSession::Select(uint64_t handle, uint8_t transport)
{
	const uint32_t old = m_sessionId.load();
	const bool wasRunning = m_thread.joinable();
	StopThread();
	if (handle == 0) {
		if (wasRunning)
			SendState(old, kStateEnded, "");
		return;
	}
	HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(handle));
	const uint32_t sid = m_nextSessionId++;
	m_sessionId = sid;
	m_lastAck = 0;
	DWORD pid = 0;
	if (IsWindow(hwnd))
		GetWindowThreadProcessId(hwnd, &pid);
	if (!IsWindow(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd ||
	    (pid == GetCurrentProcessId() && !m_cb.allowOwnProcess)) {
		SendState(sid, kStateRefused, "this window cannot be shown (it was closed?)");
		return;
	}
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_stop = false;
	}
	m_videoFailed = false;
	m_thread = std::thread(&vncWindowSession::ThreadMain, this, hwnd, sid, transport == kTransportVideo);
}

void vncWindowSession::StopThread()
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_stop = true;
	}
	m_cond.notify_all();
	if (m_thread.joinable())
		m_thread.join();
	m_active = false;
	std::lock_guard<std::mutex> lk(m_mutex);
	m_target = NULL;
	m_parts.clear();
}

void vncWindowSession::Shutdown()
{
	StopThread();
}

void vncWindowSession::ThreadMain(HWND target, uint32_t sessionId, bool wantVideo)
{
	InputDesktop desk;
	desk.Attach();
	vncWindowCapture capture;
	std::string note;
	if (!capture.Start(target, note)) {
		SendState(sessionId, kStateRefused, note);
		return;
	}
	if (!note.empty())
		Log(1, note);
	Log(1, std::string("session ") + std::to_string(sessionId) + " started, capture by " +
		(capture.UsesWgc() ? "Windows.Graphics.Capture" : "PrintWindow"));
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_target = target;
	}
	m_active = true;
	if (m_cb.onSessionStart)
		m_cb.onSessionStart();
	SendState(sessionId, kStateActive, "");

	// Video transport: the canvas goes to the encoder instead of tiles.
	bool video = wantVideo && m_cb.beginVideo && m_cb.beginVideo();
	if (wantVideo)
		Log(1, video ? "sending the window as video" : "video not available, sending lossless tiles");

	std::vector<uint8_t> prev; // last picture sent
	int prevW = 0, prevH = 0;
	uint32_t frameNo = 0;
	bool minimized = false;
	int errors = 0;
	const char* endReason = nullptr;
	uint8_t endState = 0;

	for (;;) {
		{
			std::unique_lock<std::mutex> lk(m_mutex);
			m_cond.wait_for(lk, video ? kVideoFrameInterval : kFrameInterval, [&] { return m_stop; });
			if (m_stop)
				break;
		}
		const vncWindowCapture::Result r = capture.Capture();
		if (r == vncWindowCapture::Result::Closed) {
			endState = kStateClosed;
			endReason = "the window was closed";
			break;
		}
		if (r == vncWindowCapture::Result::Minimized) {
			if (!minimized) {
				minimized = true;
				SendState(sessionId, kStateMinimized, "");
			}
			continue;
		}
		if (r == vncWindowCapture::Result::Error) {
			// During UAC prompts and the lock screen the window is not
			// visible; wait for the user's desktop to come back.
			if (!InputDesktopIsDefault()) {
				errors = 0;
				continue;
			}
			desk.Attach();
			if (++errors > kMaxErrors) {
				endState = kStateUnavailable;
				endReason = "the window could not be captured";
				break;
			}
			continue;
		}
		errors = 0;
		if (minimized) {
			minimized = false;
			SendState(sessionId, kStateActive, "");
		}

		const int w = capture.Width(), h = capture.Height();
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_origin = capture.Origin();
			m_parts = capture.Parts();
		}
		if (w != prevW || h != prevH) {
			Canvas c;
			c.sessionId = sessionId;
			c.width = static_cast<uint16_t>(w);
			c.height = static_cast<uint16_t>(h);
			m_cb.send(kPrioControl, kCanvas, EncodeCanvas(c));
			prev.clear(); // send everything for the new size
			prevW = w;
			prevH = h;
		}
		if (video && m_videoFailed.exchange(false)) {
			video = false;
			prev.clear(); // the viewer has video pictures: send everything
			Log(1, "video failed, continuing with lossless tiles");
		}
		if (video) {
			// Push only changes; a static window costs nothing.
			const size_t bytes = size_t(w) * h * 4;
			if (prev.size() != bytes || memcmp(prev.data(), capture.Pixels(), bytes) != 0) {
				m_cb.pushVideo(capture.Pixels(), w, h, capture.Stride());
				prev.assign(capture.Pixels(), capture.Pixels() + bytes);
			}
			continue;
		}

		// Flow control: wait for the viewer, the next frame then carries
		// everything that changed meanwhile.
		if (frameNo - m_lastAck.load() >= kMaxInFlight)
			continue;

		FrameHeader fh;
		fh.tiles = ChangedTiles(prev.empty() ? nullptr : prev.data(), capture.Pixels(), w, h, capture.Stride());
		if (fh.tiles.empty())
			continue;
		const std::vector<uint8_t> packed = PackTiles(capture.Pixels(), capture.Stride(), fh.tiles);
		const std::vector<uint8_t> compressed = Compress(packed, 1);
		if (compressed.empty())
			continue;
		fh.sessionId = sessionId;
		fh.frameNo = ++frameNo;
		fh.canvasWidth = static_cast<uint16_t>(w);
		fh.canvasHeight = static_cast<uint16_t>(h);
		fh.rawSize = static_cast<uint32_t>(packed.size());
		if (!m_cb.send(kPrioVideo, kFrame, EncodeFrame(fh, compressed)))
			break; // connection closed
		prev.assign(capture.Pixels(), capture.Pixels() + size_t(w) * h * 4);
	}

	if (video && m_cb.endVideo)
		m_cb.endVideo();
	m_active = false;
	if (endReason)
		SendState(sessionId, endState, endReason);
	std::lock_guard<std::mutex> lk(m_mutex);
	m_target = NULL;
	m_parts.clear();
}

void vncWindowSession::HandlePointer(int x, int y, uint8_t buttonMask, int wheelAxis, int wheelDelta)
{
	POINT origin;
	std::vector<RECT> parts;
	HWND target;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		origin = m_origin;
		parts = m_parts;
		target = m_target;
	}
	if (!target || !m_active)
		return;
	// A precise wheel message carries no button state: keep ours.
	if (wheelDelta != 0)
		buttonMask = m_buttons;
	const POINT pt = { origin.x + x, origin.y + y };
	bool inside = false;
	for (const RECT& r : parts)
		inside = inside || PtInRect(&r, pt);
	// Outside the window only a drag that started inside may continue.
	const uint8_t held = m_buttons & 0x07;
	if (!inside && !held)
		return;

	INPUT in[10] = {}; // move, 3 buttons, 4 wheel clicks, precise wheel
	int count = 0;
	const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
	const int vw = (std::max)(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = (std::max)(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
	auto add = [&](DWORD flags, DWORD data) {
		INPUT& i = in[count++];
		i.type = INPUT_MOUSE;
		i.mi.dx = static_cast<LONG>((static_cast<LONGLONG>(pt.x - vx) * 65535) / (vw - 1));
		i.mi.dy = static_cast<LONG>((static_cast<LONGLONG>(pt.y - vy) * 65535) / (vh - 1));
		i.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
		i.mi.mouseData = data;
	};
	add(MOUSEEVENTF_MOVE, 0);

	// Buttons 1-3: left, middle, right (swapped like the normal input path
	// when the user swapped the mouse buttons).
	const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != FALSE;
	const DWORD leftDown = MOUSEEVENTF_LEFTDOWN, leftUp = MOUSEEVENTF_LEFTUP;
	const DWORD rightDown = MOUSEEVENTF_RIGHTDOWN, rightUp = MOUSEEVENTF_RIGHTUP;
	const DWORD downFlag[3] = { swap ? rightDown : leftDown, MOUSEEVENTF_MIDDLEDOWN, swap ? leftDown : rightDown };
	const DWORD upFlag[3] = { swap ? rightUp : leftUp, MOUSEEVENTF_MIDDLEUP, swap ? leftUp : rightUp };
	for (int b = 0; b < 3; ++b) {
		const uint8_t bit = static_cast<uint8_t>(1 << b);
		if ((buttonMask & bit) && !(m_buttons & bit)) {
			if (inside) // never start a press outside the window
				add(downFlag[b], 0);
			else
				buttonMask &= ~bit;
		} else if (!(buttonMask & bit) && (m_buttons & bit)) {
			add(upFlag[b], 0);
		}
	}
	// Wheel: RFB buttons 4/5 (up/down) and 6/7 (left/right) are clicks.
	if (inside) {
		const struct { uint8_t bit; DWORD flag; int delta; } wheels[4] = {
			{ 0x08, MOUSEEVENTF_WHEEL, WHEEL_DELTA }, { 0x10, MOUSEEVENTF_WHEEL, -WHEEL_DELTA },
			{ 0x20, MOUSEEVENTF_HWHEEL, -WHEEL_DELTA }, { 0x40, MOUSEEVENTF_HWHEEL, WHEEL_DELTA } };
		for (const auto& w : wheels)
			if ((buttonMask & w.bit) && !(m_buttons & w.bit))
				add(w.flag, static_cast<DWORD>(w.delta));
		if (wheelDelta != 0)
			add(wheelAxis == 0 ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL, static_cast<DWORD>(wheelDelta));
	}
	m_buttons = buttonMask;
	SendInput(count, in, sizeof(INPUT));
}

void vncWindowSession::BeforeKey()
{
	HWND target;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		target = m_target;
	}
	if (!target || !m_active)
		return;
	HWND fg = GetForegroundWindow();
	if (fg && vncWindowCapture::BelongsTo(fg, target))
		return;
	// Keys go to the foreground window; bring ours to the front first. The
	// foreground lock is lifted by attaching to the current foreground
	// thread's input for a moment.
	const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
	const DWORD self = GetCurrentThreadId();
	const bool attached = fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE);
	BringWindowToTop(target);
	SetForegroundWindow(target);
	if (attached)
		AttachThreadInput(self, fgThread, FALSE);
}
