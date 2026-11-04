// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Viewer side of the extension message channel (rfbExtMsg).
// See common/ExtChannel.h for the protocol and threading rules.

#pragma once

#include "common/ExtChannel.h"

#include <memory>

class ClientConnection;
namespace fileclip { class FileClipboard; }
namespace fileman { class FileManClient; }

class ViewerExtChannel : public extmsg::ExtChannel {
public:
	explicit ViewerExtChannel(ClientConnection* cc);
	~ViewerExtChannel() override;

	// Called by the reading thread after it read a message type of rfbExtMsg.
	// Throws (like the other Read* functions) if the connection must be
	// dropped.
	void ReadAndDispatch();

	// Before each (re)connection: forget the previous connection and choose
	// the channels to offer. Starts the file clipboard engine on first use.
	void ResetConnection(bool allowFileClipboard);
	// The connection is gone (auto-reconnect pending).
	void Disconnect();
	// The viewer window closes: stop everything.
	void Shutdown();
	// File manager requests (channel kChFiles); never null.
	fileman::FileManClient* Files() { return m_files.get(); }

protected:
	bool WriteFrame(const uint8_t header[extmsg::kHeaderSize],
	                const uint8_t* payload, size_t len) override;
	void OnNegotiated(uint32_t mask) override;
	void Log(int level, const std::string& msg) override;

private:
	ClientConnection* m_cc;
	// File copy and paste (channel kChFileClip); created on first use.
	std::unique_ptr<fileclip::FileClipboard> m_fileClip;
	// File manager client (channel kChFiles), used by the file transfer window.
	std::unique_ptr<fileman::FileManClient> m_files;
};
