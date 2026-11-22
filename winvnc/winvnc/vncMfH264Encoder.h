// This file is part of UltraVNC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class vncMfH264Encoder {
public:
	vncMfH264Encoder();
	~vncMfH264Encoder();

	static bool IsAvailable();
	bool Open(ID3D11Device* device, int width, int height, int fps, uint32_t kbps, std::string& error);
	void Close();
	bool Encode(ID3D11Texture2D* source, bool forceIdr, uint64_t timestamp,
	            std::vector<uint8_t>& out, bool& isKeyframe, std::string& error);
	bool SetBitrate(uint32_t kbps);

private:
	struct Session;
	std::unique_ptr<Session> m_session;
};
