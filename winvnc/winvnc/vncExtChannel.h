// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Server side of the extension message channel (rfbExtMsg).
// See common/ExtChannel.h for the protocol and threading rules.

#pragma once

#include "common/ExtChannel.h"

#include <atomic>
#include <memory>

namespace fileclip { class FileClipboard; }
class vncVideoStreamer;

class vncClient;
class VSocket;

class vncServerExtChannel : public extmsg::ExtChannel {
public:
	explicit vncServerExtChannel(vncClient* client);
	~vncServerExtChannel() override;

	// Called by vncClientThread after it read a message type of rfbExtMsg.
	// nTO is the number of type bytes already consumed (1 normally, 0 when a
	// non-streaming DSM plugin re-sends the whole message). Returns false if
	// the connection must be dropped.
	bool ReadAndDispatch(VSocket* socket, int nTO);

	// The viewer announced rfbEncodingExtCaps: decide which channels this
	// client may use, start their engines and send our Caps. Idempotent.
	void OpenForClient();

	// Called by vncClientThread when the connection ends, before the update
	// thread is stopped. Unblocks a pending send and joins the sender thread.
	// Must not be called while holding the client's update lock.
	void Shutdown();

protected:
	bool WriteFrame(const uint8_t header[extmsg::kHeaderSize],
	                const uint8_t* payload, size_t len) override;
	void BeginBatch() override;
	void EndBatch() override;
	void OnTransportError() override;
	void OnNegotiated(uint32_t mask) override;
	void Log(int level, const std::string& msg) override;

private:
	void StopEngines();

	vncClient* m_client;
	std::atomic<bool> m_shuttingDown{false};
	bool m_openedForClient = false;
	// File copy and paste (channel kChFileClip); null if not allowed.
	std::unique_ptr<fileclip::FileClipboard> m_fileClip;
	// Video mode (channel kChVideo); null without an NVIDIA encoder.
	std::unique_ptr<vncVideoStreamer> m_video;
};

// When the server runs as a service (LocalSystem in the user's session), the
// user's Explorer must be allowed to call into our clipboard objects. Call
// once at startup, before COM is used for anything else.
void vncExtChannelInitServiceComSecurity();
