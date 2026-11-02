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

using namespace extmsg;

static_assert(rfbExtMsg == extmsg::kMsgType, "rfbExtMsg mismatch");
static_assert(rfbEncodingExtCaps == extmsg::kCapsEncoding, "rfbEncodingExtCaps mismatch");
static_assert(sz_rfbExtMsgHeader == extmsg::kHeaderSize, "sz_rfbExtMsgHeader mismatch");

// Channels this server implements. Bits are added as the features land.
static const uint32_t kServerChannels = 0;

vncServerExtChannel::vncServerExtChannel(vncClient* client)
	: ExtChannel(kServerChannels), m_client(client)
{
}

vncServerExtChannel::~vncServerExtChannel()
{
	Stop();
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
