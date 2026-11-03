// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "stdhdrs.h"
#include "vncExtChannel.h"
#include "vncclient.h"
#include "vsocket.h"
#include "SettingsManager.h"
#include "credentials.h"
#include "common/FileClipboard.h"
#include "vncVideoStreamer.h"
#include "vncWindowSession.h"

#include <mutex>

using namespace extmsg;

static_assert(rfbExtMsg == extmsg::kMsgType, "rfbExtMsg mismatch");
static_assert(rfbEncodingExtCaps == extmsg::kCapsEncoding, "rfbEncodingExtCaps mismatch");
static_assert(sz_rfbExtMsgHeader == extmsg::kHeaderSize, "sz_rfbExtMsgHeader mismatch");

// Channels are enabled per client in OpenForClient().
static const uint32_t kNoChannels = 0;

vncServerExtChannel::vncServerExtChannel(vncClient* client)
	: ExtChannel(kNoChannels), m_client(client)
{
	// Bulk data is sent while holding the update lock. Keep each batch small
	// so that screen updates are not held back on slow links (64 KB is about
	// 60 ms at 1 MB/s).
	SetBatchBudget(64 * 1024);
	// Video mode adapts its bitrate to the round-trip time; measure it often.
	SetPingInterval(std::chrono::milliseconds(500));
}

vncServerExtChannel::~vncServerExtChannel()
{
	StopEngines();
	Stop();
}

// Token of the user logged on to the console, for file access while the
// server runs as LocalSystem. The engine closes the returned duplicate.
static bool AcquireDesktopUserToken(HANDLE* token)
{
	*token = NULL;
	if (!settings->RunningFromExternalService())
		return true; // we already run as the desktop user
	// No lock-screen check here: IsWSLocked() looks for LogonUI.exe, which
	// on some systems stays in the session after unlocking and made every
	// remote-to-local copy fail. A logged-on user's token is what matters.
	// DesktopUsersToken is not thread safe; serialise our calls and keep a
	// private duplicate so that a later refresh cannot close it under us.
	static std::mutex tokenMutex;
	std::lock_guard<std::mutex> lk(tokenMutex);
	HANDLE shared = DesktopUsersToken::getInstance()->getDesktopUsersToken();
	if (!shared) {
		vnclog.Print(LL_INTERR, VNCLOG("file clipboard: no desktop user token\n"));
		return false;
	}
	return DuplicateHandle(GetCurrentProcess(), shared, GetCurrentProcess(), token, 0, FALSE,
		DUPLICATE_SAME_ACCESS) != FALSE;
}

void vncServerExtChannel::OpenForClient()
{
	if (m_openedForClient) {
		Open(); // no-op once open
		return;
	}
	m_openedForClient = true;

	uint32_t mask = 0;

	// File copy and paste follows the File Transfer permission, and needs a
	// client that may use keyboard and mouse.
	if (settings->getEnableFileTransfer() && m_client->m_keyboardenabled && m_client->m_pointerenabled) {
		fileclip::Callbacks cb;
		cb.send = [this](ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload) {
			return Send(prio, kChFileClip, subtype, payload);
		};
		cb.log = [this](int level, const std::string& msg) { Log(level, msg); };
		cb.acquireUserToken = &AcquireDesktopUserToken;
		m_fileClip.reset(new fileclip::FileClipboard(cb));
		if (m_fileClip->Start()) {
			SetHandler(kChFileClip, [this](uint8_t subtype, std::vector<uint8_t>& payload) {
				m_fileClip->OnMessage(subtype, payload);
			});
			mask |= ChannelBit(kChFileClip);
		} else {
			m_fileClip.reset();
		}
	}

	// Video mode needs NVENC on an NVIDIA GPU; without it the viewer keeps
	// the classic mode only (its menu entry reports why).
	std::string why;
	if (vncVideoStreamer::IsSupported(&why)) {
		vncVideoStreamer::Callbacks vc;
		vc.send = [this](ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload) {
			return Send(prio, kChVideo, subtype, payload);
		};
		vc.log = [this](int level, const std::string& msg) { Log(level, msg); };
		vc.queuedVideoBytes = [this] { return QueuedBytes(kPrioVideo); };
		vc.rttMicros = [this] { return RttMicros(); };
		vc.framebufferArea = [this](RECT* area, std::string* reason) -> uint32_t {
			if (m_client->m_nScale != 1) {
				*reason = "video mode does not work with server side scaling";
				return video::kStatusScaling;
			}
			// Same mapping as the cursor position: framebuffer (0,0) is at
			// screen (m_ScreenOffset + monitor_Offset).
			const int x = m_client->m_ScreenOffsetx + m_client->monitor_Offsetx;
			const int y = m_client->m_ScreenOffsety + m_client->monitor_Offsety;
			const rfb::Rect fb = m_client->m_ScaledScreen;
			area->left = x;
			area->top = y;
			area->right = x + (fb.br.x - fb.tl.x);
			area->bottom = y + (fb.br.y - fb.tl.y);
			return 0;
		};
		m_video.reset(new vncVideoStreamer(vc));
		SetHandler(kChVideo, [this](uint8_t subtype, std::vector<uint8_t>& payload) {
			m_video->OnMessage(subtype, payload);
		});
		mask |= ChannelBit(kChVideo);
	} else {
		Log(1, "video mode unavailable: " + why);
	}

	// Single window mode works with any GPU (lossless tiles).
	{
		vncWindowSession::Callbacks wc;
		wc.send = [this](ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload) {
			return Send(prio, kChWindow, subtype, payload);
		};
		wc.log = [this](int level, const std::string& msg) { Log(level, msg); };
		wc.onSessionStart = [this] {
			// An update being sent right now finishes first (it holds the
			// update lock); the viewer requests a full update when it leaves
			// window mode.
			omni_mutex_lock l(m_client->GetUpdateLock(), 261);
			m_client->m_incr_rgn.clear();
		};
		m_window.reset(new vncWindowSession(wc));
		SetHandler(kChWindow, [this](uint8_t subtype, std::vector<uint8_t>& payload) {
			m_window->OnMessage(subtype, payload);
		});
		mask |= ChannelBit(kChWindow);
	}

	SetLocalChannelMask(mask);
	Open();
}

bool vncServerExtChannel::WindowSessionActive() const
{
	return m_window && m_window->Active();
}

void vncServerExtChannel::HandleWindowPointer(int x, int y, uint8_t buttonMask, int wheelAxis, int wheelDelta)
{
	if (m_window)
		m_window->HandlePointer(x, y, buttonMask, wheelAxis, wheelDelta);
}

void vncServerExtChannel::BeforeWindowKey()
{
	if (m_window && m_window->Active())
		m_window->BeforeKey();
}

void vncServerExtChannel::OnNegotiated(uint32_t mask)
{
	vnclog.Print(LL_INTINFO, VNCLOG("extension channels negotiated: 0x%x\n"), mask);
	if (m_fileClip && (mask & ChannelBit(kChFileClip)))
		m_fileClip->OnConnected();
}

void vncServerExtChannel::StopEngines()
{
	if (m_window)
		m_window->Shutdown();
	if (m_video)
		m_video->Shutdown();
	if (m_fileClip) {
		m_fileClip->OnDisconnected();
		m_fileClip->Stop();
	}
}

bool vncServerExtChannel::ReadAndDispatch(VSocket* socket, int nTO)
{
	uint8_t hb[kHeaderSize];
	hb[0] = rfbExtMsg;
	if (!socket->ReadExact(reinterpret_cast<char*>(hb) + nTO, kHeaderSize - nTO))
		return false;

	Header h = DecodeHeader(hb);
	if (!HeaderIsAcceptable(h)) {
		vnclog.Print(LL_CONNERR, VNCLOG("rfbExtMsg: bad header (type %u, length %u)\n"),
			static_cast<unsigned>(h.type), static_cast<unsigned>(h.length));
		return false;
	}

	std::vector<uint8_t> payload(h.length);
	if (h.length > 0 && !socket->ReadExact(reinterpret_cast<char*>(payload.data()), h.length))
		return false;

	if (OnFrame(h, payload) != RxResult::Ok) {
		vnclog.Print(LL_CONNERR, VNCLOG("rfbExtMsg: protocol error on channel %u subtype %u\n"),
			static_cast<unsigned>(h.channel), static_cast<unsigned>(h.subtype));
		return false;
	}
	return true;
}

void vncServerExtChannel::Shutdown()
{
	m_shuttingDown = true;
	// A send blocked on a peer that stopped reading would keep the sender
	// thread from exiting; shutting the socket down makes it fail at once.
	if (IsOpen() && m_client->m_socket)
		m_client->m_socket->Shutdown();
	// Engines send through this channel; stop them first.
	StopEngines();
	Stop();
}

bool vncServerExtChannel::WriteFrame(const uint8_t header[kHeaderSize],
                                     const uint8_t* payload, size_t len)
{
	VSocket* socket = m_client->m_socket;
	if (!socket || !m_client->cl_connected)
		return false;
	// Same pattern as the extended clipboard: the header carries the message
	// type (needed by non-streaming DSM plugins), the payload follows.
	if (len == 0)
		return socket->SendExact(reinterpret_cast<const char*>(header), kHeaderSize, rfbExtMsg) != 0;
	if (!socket->SendExactQueue(reinterpret_cast<const char*>(header), kHeaderSize, rfbExtMsg))
		return false;
	return socket->SendExact(reinterpret_cast<const char*>(payload), static_cast<VCard>(len)) != 0;
}

// Frames go out between framebuffer updates: vncClient sends each update
// while holding the update lock.
void vncServerExtChannel::BeginBatch()
{
	m_client->GetUpdateLock().lock();
}

void vncServerExtChannel::EndBatch()
{
	m_client->GetUpdateLock().unlock();
}

void vncServerExtChannel::OnTransportError()
{
	if (m_shuttingDown)
		return;
	// Same reaction as vncClient::SendRFBMsg on a failed send.
	m_client->Kill();
}

void vncServerExtChannel::Log(int level, const std::string& msg)
{
	vnclog.Print(level == 0 ? LL_INTERR : LL_INTINFO, VNCLOG("%s\n"), msg.c_str());
}

void vncExtChannelInitServiceComSecurity()
{
	// Keep COM initialised on this thread for the lifetime of the process;
	// CoInitializeSecurity needs it and applies to the whole process.
	HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		vnclog.Print(LL_INTERR, VNCLOG("CoInitializeEx failed 0x%lx\n"), static_cast<unsigned long>(hr));
		return;
	}
	std::string error;
	if (fileclip::FileClipboard::InitComSecurityForService(&error))
		vnclog.Print(LL_INTINFO, VNCLOG("COM security set for file copy and paste\n"));
	else
		vnclog.Print(LL_INTERR, VNCLOG("%s; pasting files on this machine may fail\n"), error.c_str());
}
