// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Single window remote control, viewer side (docs/design.md section 4).
// The user picks a remote window; the local framebuffer becomes that
// window's canvas (window plus its pop-ups) and is filled with lossless
// tiles from extension channel 2. Leaving the mode restores the desktop
// framebuffer and asks for a full update.

#include "stdhdrs.h"
#include "vncviewer.h"
#include "ClientConnection.h"
#include "UiText.h"
#include "ClientConnectionExt.h"
#include "common/WindowProto.h"

using namespace extmsg;

namespace {

enum WindowModeEvent : WPARAM {
	kEventList = 1,      // the window list arrived
	kEventEnded = 2,     // the server ended the session (lParam = state)
	kEventMinimized = 3, // the remote window was minimized
};

bool FramebufferIsBgra32(const rfbPixelFormat& f)
{
	return f.trueColour && f.bitsPerPixel == 32 && f.redMax == 255 && f.greenMax == 255 &&
		f.blueMax == 255 && f.redShift == 16 && f.greenShift == 8 && f.blueShift == 0;
}

void WindowLog(int level, const std::string& msg)
{
	vnclog.Print(level, _T("window: %hs\n"), msg.c_str());
}

std::wstring Utf8ToWide(const std::string& s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
	if (n <= 1)
		return std::wstring();
	std::wstring w(n - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	return w;
}

// --- window picker: a small dialog built in memory -------------------------

const int kIdList = 100;

struct PickerData {
	const std::vector<window::Entry>* list;
	int choice;
};

void AlignDword(std::vector<WORD>& t)
{
	if (t.size() % 2)
		t.push_back(0);
}

void AddString(std::vector<WORD>& t, const wchar_t* s)
{
	do {
		t.push_back(static_cast<WORD>(*s));
	} while (*s++);
}

void AddItem(std::vector<WORD>& t, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls,
             const wchar_t* text)
{
	AlignDword(t);
	DLGITEMTEMPLATE it = {};
	it.style = style | WS_CHILD | WS_VISIBLE;
	it.x = x;
	it.y = y;
	it.cx = cx;
	it.cy = cy;
	it.id = id;
	const WORD* p = reinterpret_cast<const WORD*>(&it);
	t.insert(t.end(), p, p + sizeof(it) / sizeof(WORD));
	t.push_back(0xFFFF);
	t.push_back(cls); // predefined class atom
	AddString(t, text);
	t.push_back(0); // no creation data
}

INT_PTR CALLBACK PickerProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_INITDIALOG: {
		SetWindowLongPtrW(dlg, DWLP_USER, lp);
		PickerData* d = reinterpret_cast<PickerData*>(lp);
		HWND list = GetDlgItem(dlg, kIdList);
		for (size_t i = 0; i < d->list->size(); ++i) {
			const window::Entry& e = (*d->list)[i];
			std::wstring line = Utf8ToWide(e.title);
			if (!e.program.empty())
				line += L"    [" + Utf8ToWide(e.program) + L"]";
			if (e.minimized)
				line += std::wstring(L"    ") + Tr(L"(minimized)");
			const LRESULT idx = SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
			SendMessageW(list, LB_SETITEMDATA, idx, static_cast<LPARAM>(i));
		}
		SendMessageW(list, LB_SETCURSEL, 0, 0);
		return TRUE;
	}
	case WM_COMMAND: {
		PickerData* d = reinterpret_cast<PickerData*>(GetWindowLongPtrW(dlg, DWLP_USER));
		const WORD id = LOWORD(wp);
		if (id == IDOK || (id == kIdList && HIWORD(wp) == LBN_DBLCLK)) {
			HWND list = GetDlgItem(dlg, kIdList);
			const LRESULT sel = SendMessageW(list, LB_GETCURSEL, 0, 0);
			if (sel != LB_ERR)
				d->choice = static_cast<int>(SendMessageW(list, LB_GETITEMDATA, sel, 0));
			EndDialog(dlg, IDOK);
			return TRUE;
		}
		if (id == IDCANCEL) {
			EndDialog(dlg, IDCANCEL);
			return TRUE;
		}
		break;
	}
	}
	return FALSE;
}

// Returns the index of the chosen window or -1.
int PickWindow(HWND parent, const std::vector<window::Entry>& list)
{
	std::vector<WORD> t;
	DLGTEMPLATE dt = {};
	dt.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
	dt.cdit = 3;
	dt.cx = 320;
	dt.cy = 200;
	const WORD* p = reinterpret_cast<const WORD*>(&dt);
	t.insert(t.end(), p, p + sizeof(dt) / sizeof(WORD));
	t.push_back(0); // no menu
	t.push_back(0); // default class
	AddString(t, Tr(L"Show only one remote window"));
	t.push_back(9); // font size
	AddString(t, L"Segoe UI");
	AddItem(t, LBS_NOTIFY | WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT, 7, 7, 306, 162, kIdList,
		0x0083, L"");
	AddItem(t, BS_DEFPUSHBUTTON | WS_TABSTOP, 206, 177, 50, 16, IDOK, 0x0080, Tr(L"Show"));
	AddItem(t, BS_PUSHBUTTON | WS_TABSTOP, 263, 177, 50, 16, IDCANCEL, 0x0080, Tr(L"Cancel"));

	PickerData d = { &list, -1 };
	const INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), reinterpret_cast<LPCDLGTEMPLATEW>(t.data()),
		parent, &PickerProc, reinterpret_cast<LPARAM>(&d));
	return r == IDOK ? d.choice : -1;
}

} // namespace

UINT ClientConnection::WindowModeMessage()
{
	static const UINT msg = RegisterWindowMessageW(L"UltraVNC.WindowMode");
	return msg;
}

bool ClientConnection::SendWindowMessage(uint8_t subtype, const std::vector<uint8_t>& payload)
{
	return m_ext->Send(kPrioControl, kChWindow, subtype, payload);
}

// UI thread (system menu).
void ClientConnection::ToggleWindowMode()
{
	if (m_windowRequested) {
		LeaveWindowMode(true);
		return;
	}
	const wchar_t* problem = nullptr;
	if (!m_ext->ChannelUsable(kChWindow))
		problem = Tr(L"The server does not offer single window mode (it needs the new UltraVNC server).");
	else if (!FramebufferIsBgra32(m_myFormat))
		problem = Tr(L"Single window mode needs the \"Full colors\" (32 bit) pixel format.\n\n"
		             L"Change it in the connection options and try again.");
	if (problem) {
		MessageBoxW(m_hwndMain, problem, Tr(L"UltraVNC Viewer - Single window"), MB_OK | MB_ICONINFORMATION);
		return;
	}
	m_windowPicking = true;
	SendWindowMessage(window::kListReq, {});
}

// UI thread: tiles, or video when video mode is on (phase 3b). Sending it
// again for the same window switches the transport.
void ClientConnection::SelectWindowTransport()
{
	SendWindowMessage(window::kSelect, window::EncodeSelect(m_windowHandle,
		m_videoRequested ? window::kTransportVideo : window::kTransportTiles));
}

// UI thread.
void ClientConnection::LeaveWindowMode(bool tellServer)
{
	m_windowRequested = false;
	m_windowHandle = 0;
	if (tellServer)
		SendWindowMessage(window::kSelect, window::EncodeSelect(0));
	if (m_windowActive) {
		// Back to the desktop framebuffer, then a full desktop picture.
		ResizeLocalFramebuffer(m_windowSavedWidth, m_windowSavedHeight);
		m_windowActive = false;
		SendFullFramebufferUpdateRequest(true);
	}
	if (m_videoRequested) {
		// Video mode is still on: back to the monitor stream.
		m_videoActive = false;
		m_videoNeedKey = true;
		SendVideoMessage(video::kStart, video::EncodeStart(MakeVideoStart()));
	}
	CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_WINDOWMODE, MF_BYCOMMAND | MF_UNCHECKED);
	WindowLog(1, "back to the desktop");
}

// UI thread, posted by the reader thread.
void ClientConnection::OnWindowModeEvent(WPARAM event, LPARAM param)
{
	if (event == kEventList) {
		std::vector<window::Entry> list;
		{
			omni_mutex_lock l(m_windowMutex);
			list.swap(m_windowList);
		}
		if (!m_windowPicking)
			return;
		m_windowPicking = false;
		if (list.empty()) {
			MessageBoxW(m_hwndMain, Tr(L"No windows to show on the remote computer."), Tr(L"UltraVNC Viewer - Single window"),
				MB_OK | MB_ICONINFORMATION);
			return;
		}
		const int i = PickWindow(m_hwndMain, list);
		if (i < 0 || i >= static_cast<int>(list.size()))
			return;
		m_windowRequested = true;
		m_windowSession = 0; // set by the server's first State
		m_windowHandle = list[i].handle;
		CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_WINDOWMODE, MF_BYCOMMAND | MF_CHECKED);
		SelectWindowTransport();
		WindowLog(1, "selected \"" + list[i].title + "\"" + (m_videoRequested ? " as video" : ""));
		return;
	}
	if (event == kEventMinimized) {
		if (m_windowRequested &&
		    MessageBoxW(m_hwndMain, Tr(L"The window was minimized on the remote computer.\n\nRestore it?"),
			    Tr(L"UltraVNC Viewer - Single window"), MB_YESNO | MB_ICONQUESTION) == IDYES)
			SendWindowMessage(window::kRestore, {});
		return;
	}
	if (event == kEventEnded) {
		if (!m_windowRequested)
			return; // we asked for it
		std::string reason;
		{
			omni_mutex_lock l(m_windowMutex);
			reason = m_windowEndReason;
		}
		LeaveWindowMode(false);
		if (param != window::kStateEnded) {
			std::wstring text = Tr(L"Single window mode ended");
			const std::wstring w = Utf8ToWide(reason);
			text += w.empty() ? L"." : L":\n\n" + w;
			MessageBoxW(m_hwndMain, text.c_str(), Tr(L"UltraVNC Viewer - Single window"), MB_OK | MB_ICONINFORMATION);
		}
	}
}

void ClientConnection::ResetWindowState()
{
	m_windowPicking = false;
	m_windowRequested = false;
	// A reconnect starts with the server's desktop size anyway.
	m_windowActive = false;
	if (m_hwndMain)
		CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_WINDOWMODE, MF_BYCOMMAND | MF_UNCHECKED);
}

// Same steps as for a server side resize (rfbResizeFrameBuffer).
void ClientConnection::ResizeLocalFramebuffer(int width, int height)
{
	if (width <= 0 || height <= 0)
		return;
	{
		omni_mutex_lock l(m_bitmapdcMutex);
		if (m_si.framebufferWidth == width && m_si.framebufferHeight == height)
			return;
		ClearCache();
		m_si.framebufferWidth = static_cast<CARD16>(width);
		m_si.framebufferHeight = static_cast<CARD16>(height);
		checkParemeters();
		Createdib();
		m_pendingScaleChange = true;
	}
	SizeWindow();
	InvalidateRect(m_hwndcn, NULL, TRUE);
	RealiseFullScreenMode();
}

// Reader thread.
void ClientConnection::OnWindowMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	const uint8_t* p = payload.data();
	const size_t n = payload.size();
	switch (subtype) {
	case window::kList: {
		std::vector<window::Entry> list;
		if (!window::DecodeList(p, n, list))
			return;
		{
			omni_mutex_lock l(m_windowMutex);
			m_windowList.swap(list);
		}
		PostMessage(m_hwndMain, WindowModeMessage(), kEventList, 0);
		break;
	}
	case window::kState: {
		window::State st;
		if (!window::DecodeState(p, n, st) || !m_windowRequested)
			return;
		if (st.state == window::kStateActive) {
			m_windowSession = st.sessionId;
			WindowLog(1, "session " + std::to_string(st.sessionId) + " active");
		} else if (st.sessionId != m_windowSession && m_windowSession != 0) {
			return; // an older session
		}
		if (st.state == window::kStateMinimized) {
			PostMessage(m_hwndMain, WindowModeMessage(), kEventMinimized, 0);
		} else if (window::IsFinal(st.state)) {
			{
				omni_mutex_lock l(m_windowMutex);
				m_windowEndReason = st.message;
			}
			WindowLog(1, "session ended (" + std::to_string(st.state) + "): " + st.message);
			PostMessage(m_hwndMain, WindowModeMessage(), kEventEnded, st.state);
		}
		break;
	}
	case window::kCanvas: {
		window::Canvas c;
		if (!window::DecodeCanvas(p, n, c) || !m_windowRequested || c.sessionId != m_windowSession)
			return;
		if (!m_windowActive) {
			m_windowSavedWidth = m_si.framebufferWidth;
			m_windowSavedHeight = m_si.framebufferHeight;
			m_windowActive = true;
		}
		ResizeLocalFramebuffer(c.width, c.height);
		WindowLog(2, "canvas " + std::to_string(c.width) + "x" + std::to_string(c.height));
		break;
	}
	case window::kFrame: {
		window::FrameHeader fh;
		if (!window::DecodeFrameHeader(p, n, fh) || !m_windowActive || fh.sessionId != m_windowSession)
			return;
		// Acknowledge every frame of the session, even one we drop: the
		// server sends nothing while two frames are unacknowledged.
		SendWindowMessage(window::kAck, window::EncodeAck(fh.sessionId, fh.frameNo));
		std::vector<uint8_t> raw;
		if (!window::Decompress(p + fh.dataOffset, n - fh.dataOffset, fh.rawSize, raw)) {
			WindowLog(0, "corrupt frame dropped");
			return;
		}
		RECT dirty = {};
		{
			omni_mutex_lock l(m_bitmapdcMutex);
			// A frame for another canvas size is from before a resize; the
			// server sends the whole new canvas after it.
			if (!m_DIBbits || !FramebufferIsBgra32(m_myFormat) || fh.canvasWidth != m_si.framebufferWidth ||
			    fh.canvasHeight != m_si.framebufferHeight)
				return;
			if (!window::UnpackTiles(raw.data(), raw.size(), fh.tiles, static_cast<uint8_t*>(m_DIBbits),
					m_si.framebufferWidth, m_si.framebufferHeight, m_si.framebufferWidth * 4))
				return;
		}
		for (const window::Rect& t : fh.tiles) {
			RECT r = { t.x, t.y, t.x + t.w, t.y + t.h };
			UnionRect(&dirty, &dirty, &r);
		}
		InvalidateScreenRect(&dirty);
		break;
	}
	default:
		break;
	}
}
