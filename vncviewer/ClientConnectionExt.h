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

class ClientConnection;

class ViewerExtChannel : public extmsg::ExtChannel {
public:
	explicit ViewerExtChannel(ClientConnection* cc);
	~ViewerExtChannel() override;

	// Called by the reading thread after it read a message type of rfbExtMsg.
	// Throws (like the other Read* functions) if the connection must be
	// dropped.
	void ReadAndDispatch();

protected:
	bool WriteFrame(const uint8_t header[extmsg::kHeaderSize],
	                const uint8_t* payload, size_t len) override;
	void OnNegotiated(uint32_t mask) override;
	void Log(int level, const std::string& msg) override;

private:
	ClientConnection* m_cc;
};
