// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "stdhdrs.h"
#include "vncviewer.h"
#include "ClientConnection.h"
#include "ClientConnectionExt.h"
#include "Exception.h"
#include "common/FileClipboard.h"

using namespace extmsg;

static_assert(rfbExtMsg == extmsg::kMsgType, "rfbExtMsg mismatch");
static_assert(rfbEncodingExtCaps == extmsg::kCapsEncoding, "rfbEncodingExtCaps mismatch");
static_assert(sz_rfbExtMsgHeader == extmsg::kHeaderSize, "sz_rfbExtMsgHeader mismatch");

// Channels are chosen per connection in ResetConnection().
static const uint32_t kNoChannels = 0;

ViewerExtChannel::ViewerExtChannel(ClientConnection* cc)
	: ExtChannel(kNoChannels), m_cc(cc)
{
	// Keyboard and mouse events wait for the write lock while a batch goes
	// out; keep batches small so input stays responsive on slow uplinks.
	SetBatchBudget(64 * 1024);

	// Video mode is decoded with Windows' own decoder; the channel is always
	// offered, the user switches it on from the system menu.
	SetHandler(kChVideo, [this](uint8_t subtype, std::vector<uint8_t>& payload) {
		m_cc->OnVideoMessage(subtype, payload);
	});
	SetHandler(kChWindow, [this](uint8_t subtype, std::vector<uint8_t>& payload) {
		m_cc->OnWindowMessage(subtype, payload);
	});
}

ViewerExtChannel::~ViewerExtChannel()
{
	Shutdown();
}

void ViewerExtChannel::ResetConnection(bool allowFileClipboard)
{
	Disconnect();

	uint32_t mask = ChannelBit(kChVideo) | ChannelBit(kChWindow);
	if (allowFileClipboard) {
		if (!m_fileClip) {
			fileclip::Callbacks cb;
			cb.send = [this](ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload) {
				return Send(prio, kChFileClip, subtype, payload);
			};
			cb.log = [this](int level, const std::string& msg) { Log(level, msg); };
			m_fileClip.reset(new fileclip::FileClipboard(cb));
			if (m_fileClip->Start()) {
				SetHandler(kChFileClip, [this](uint8_t subtype, std::vector<uint8_t>& payload) {
					m_fileClip->OnMessage(subtype, payload);
				});
			} else {
				m_fileClip.reset();
			}
		}
		if (m_fileClip)
			mask |= ChannelBit(kChFileClip);
	}
	SetLocalChannelMask(mask);
}

void ViewerExtChannel::Disconnect()
{
	if (m_fileClip)
		m_fileClip->OnDisconnected();
	Reset();
}

void ViewerExtChannel::Shutdown()
{
	// The engine sends through this channel; stop it first.
	if (m_fileClip) {
		m_fileClip->OnDisconnected();
		m_fileClip->Stop();
	}
	Stop();
}

void ViewerExtChannel::ReadAndDispatch()
{
	uint8_t hb[kHeaderSize];
	hb[0] = rfbExtMsg;
	m_cc->ReadExact(reinterpret_cast<char*>(hb) + m_cc->m_nTO, kHeaderSize - m_cc->m_nTO);

	Header h = DecodeHeader(hb);
	if (!HeaderIsAcceptable(h)) {
		vnclog.Print(0, _T("rfbExtMsg: bad header (type %u, length %u)\n"),
			static_cast<unsigned>(h.type), static_cast<unsigned>(h.length));
		throw WarningException(L"Extension channel: invalid message from server");
	}

	std::vector<uint8_t> payload(h.length);
	if (h.length > 0)
		m_cc->ReadExact(reinterpret_cast<char*>(payload.data()), static_cast<int>(h.length));

	if (OnFrame(h, payload) != RxResult::Ok) {
		vnclog.Print(0, _T("rfbExtMsg: protocol error on channel %u subtype %u\n"),
			static_cast<unsigned>(h.channel), static_cast<unsigned>(h.subtype));
		throw WarningException(L"Extension channel: protocol error");
	}
}

bool ViewerExtChannel::WriteFrame(const uint8_t header[kHeaderSize],
                                  const uint8_t* payload, size_t len)
{
	if (m_cc->m_sock == INVALID_SOCKET || !m_cc->m_running)
		return false;
	// Runs on the channel's sender thread. Hold the connection's write lock
	// across header and payload so that input events sent from the UI thread
	// cannot end up in between. The Write* helpers report errors by throwing
	// or by clearing m_running; never let an exception escape this thread.
	bool ok = true;
	EnterCriticalSection(&m_cc->crit);
	try {
		if (len == 0) {
			m_cc->WriteExact(reinterpret_cast<char*>(const_cast<uint8_t*>(header)),
				static_cast<int>(kHeaderSize), rfbExtMsg);
		} else {
			m_cc->WriteExactQueue(reinterpret_cast<char*>(const_cast<uint8_t*>(header)),
				static_cast<int>(kHeaderSize), rfbExtMsg);
			m_cc->WriteExact(reinterpret_cast<char*>(const_cast<uint8_t*>(payload)),
				static_cast<int>(len));
		}
	}
	catch (...) {
		ok = false;
	}
	LeaveCriticalSection(&m_cc->crit);
	return ok && m_cc->m_running;
}

void ViewerExtChannel::OnNegotiated(uint32_t mask)
{
	vnclog.Print(1, _T("Extension channel negotiated with server, channels 0x%x\n"), mask);
	if (m_fileClip && (mask & ChannelBit(kChFileClip)))
		m_fileClip->OnConnected();
}

void ViewerExtChannel::Log(int level, const std::string& msg)
{
	vnclog.Print(level == 0 ? 0 : 2, _T("%hs\n"), msg.c_str());
}
