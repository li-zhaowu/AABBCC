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

using namespace extmsg;

static_assert(rfbExtMsg == extmsg::kMsgType, "rfbExtMsg mismatch");
static_assert(rfbEncodingExtCaps == extmsg::kCapsEncoding, "rfbEncodingExtCaps mismatch");
static_assert(sz_rfbExtMsgHeader == extmsg::kHeaderSize, "sz_rfbExtMsgHeader mismatch");

// Channels this viewer implements. Bits are added as the features land.
static const uint32_t kViewerChannels = 0;

ViewerExtChannel::ViewerExtChannel(ClientConnection* cc)
	: ExtChannel(kViewerChannels), m_cc(cc)
{
}

ViewerExtChannel::~ViewerExtChannel()
{
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
}

void ViewerExtChannel::Log(int level, const std::string& msg)
{
	vnclog.Print(level == 0 ? 0 : 2, _T("%hs\n"), msg.c_str());
}
