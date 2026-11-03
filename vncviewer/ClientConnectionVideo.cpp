// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Video mode, viewer side (docs/design.md section 3). The server streams one
// monitor as H.264/HEVC on extension channel 4; we decode it with Media
// Foundation and write the pictures into the normal framebuffer, so scaling,
// full screen, snapshots etc. keep working. While video mode is on, no
// framebuffer updates are requested.

#include "stdhdrs.h"
#include "vncviewer.h"
#include "ClientConnection.h"
#include "ClientConnectionExt.h"
#include "Exception.h"
#include "VideoDecoder.h"
#include "common/VideoProto.h"

#include <libyuv.h> // vcpkg port "libyuv"

using namespace extmsg;

UINT ClientConnection::VideoModeEndedMessage()
{
	static const UINT msg = RegisterWindowMessageW(L"UltraVNC.VideoModeEnded");
	return msg;
}

static bool FramebufferIsBgra32(const rfbPixelFormat& f)
{
	return f.trueColour && f.bitsPerPixel == 32 && f.redMax == 255 && f.greenMax == 255 &&
		f.blueMax == 255 && f.redShift == 16 && f.greenShift == 8 && f.blueShift == 0;
}

static void VideoLog(int level, const std::string& msg)
{
	vnclog.Print(level, _T("video: %hs\n"), msg.c_str());
}

bool ClientConnection::SendVideoMessage(uint8_t subtype, const std::vector<uint8_t>& payload)
{
	return m_ext->Send(kPrioControl, kChVideo, subtype, payload);
}

// UI thread (system menu).
void ClientConnection::ToggleVideoMode()
{
	if (m_videoRequested) {
		StopVideoMode(true);
		return;
	}
	const wchar_t* problem = nullptr;
	if (!m_ext->ChannelUsable(kChVideo))
		problem = L"The server does not offer video mode.\n\n"
		          L"Video mode needs the new UltraVNC server on a machine with an NVIDIA GPU "
		          L"and a current NVIDIA driver (see the server log for details).";
	else if (!FramebufferIsBgra32(m_myFormat))
		problem = L"Video mode needs the \"Full colors\" (32 bit) pixel format.\n\n"
		          L"Change it in the connection options and try again.";
	else if (m_nServerScale != 1)
		problem = L"Video mode does not work with server side scaling.\n\n"
		          L"Set server scaling to 1/1 and try again.";
	if (problem) {
		MessageBoxW(m_hwndMain, problem, L"UltraVNC Viewer - Video mode", MB_OK | MB_ICONINFORMATION);
		return;
	}

	video::Start start; // H.264, 60 fps, 20 Mbps (design defaults)
	m_videoNeedKey = true;
	m_videoRequested = true;
	if (!SendVideoMessage(video::kStart, video::EncodeStart(start))) {
		m_videoRequested = false;
		return;
	}
	CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_VIDEOMODE, MF_BYCOMMAND | MF_CHECKED);
	VideoLog(1, "requested");
}

// UI thread.
void ClientConnection::StopVideoMode(bool tellServer)
{
	const bool wasActive = m_videoActive.exchange(false);
	const bool wasRequested = m_videoRequested.exchange(false);
	if (tellServer)
		SendVideoMessage(video::kStop, {});
	CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_VIDEOMODE, MF_BYCOMMAND | MF_UNCHECKED);
	// Back to classic updates: one full picture, then incremental again.
	// (When the server ended video mode, m_videoActive is already false but
	// incremental requests were suppressed, so this request is needed too.)
	if (wasActive || wasRequested)
		SendFullFramebufferUpdateRequest(true);
	VideoLog(1, "stopped");
}

// UI thread, posted by the reader thread when the server ended video mode.
void ClientConnection::OnVideoModeEnded(WPARAM statusCode)
{
	std::string reason;
	{
		omni_mutex_lock l(m_videoMutex);
		reason = m_videoEndReason;
	}
	if (!m_videoRequested)
		return; // already stopped by the user (this is the server's confirmation)
	StopVideoMode(false);
	if (statusCode != video::kStatusStopped) {
		std::wstring text = L"Video mode ended:\n\n";
		const int n = MultiByteToWideChar(CP_UTF8, 0, reason.c_str(), -1, NULL, 0);
		if (n > 1) {
			std::wstring w(n - 1, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, reason.c_str(), -1, &w[0], n);
			text += w;
		} else {
			text += L"(no reason given)";
		}
		MessageBoxW(m_hwndMain, text.c_str(), L"UltraVNC Viewer - Video mode", MB_OK | MB_ICONINFORMATION);
	}
}

// Connection (re)start.
void ClientConnection::ResetVideoState()
{
	m_videoActive = false;
	m_videoRequested = false;
	if (m_hwndMain)
		CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_VIDEOMODE, MF_BYCOMMAND | MF_UNCHECKED);
}

// Reader thread.
void ClientConnection::OnVideoMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	const uint8_t* p = payload.data();
	const size_t n = payload.size();
	switch (subtype) {
	case video::kConfig: {
		video::Config cfg;
		if (!video::DecodeConfig(p, n, cfg) || !m_videoRequested)
			return;
		if (!m_videoDecoder)
			m_videoDecoder.reset(new VideoDecoder);
		std::string error;
		if (!m_videoDecoder->Open(cfg.codec, cfg.width, cfg.height, cfg.fps, error)) {
			VideoLog(0, "decoder: " + error);
			SendVideoMessage(video::kStop, {});
			{
				omni_mutex_lock l(m_videoMutex);
				m_videoEndReason = "The video decoder could not be started: " + error;
			}
			PostMessage(m_hwndMain, VideoModeEndedMessage(), video::kStatusUnsupported, 0);
			return;
		}
		m_videoConfig = cfg;
		m_videoNeedKey = true;
		m_videoActive = true;
		VideoLog(1, "stream " + std::to_string(cfg.streamId) + ": " + std::to_string(cfg.width) + "x" +
			std::to_string(cfg.height) + " at " + std::to_string(cfg.x) + "," + std::to_string(cfg.y));
		break;
	}
	case video::kFrame: {
		video::FrameHeader h;
		if (!m_videoActive || !video::DecodeFrameHeader(p, n, h) || h.streamId != m_videoConfig.streamId)
			return; // stale stream or video mode already off
		const bool key = (h.flags & video::kFrameKey) != 0;
		if (m_videoNeedKey && !key)
			return; // cannot decode P frames before the next IDR
		std::string error;
		const bool ok = m_videoDecoder->Decode(p + video::kFrameHeaderSize, n - video::kFrameHeaderSize,
			static_cast<int64_t>(h.captureTimeUs) * 10,
			[this](const VideoDecoder::Picture& pic) { BlitVideoPicture(pic); }, error);
		if (ok) {
			m_videoNeedKey = false;
		} else {
			VideoLog(0, "decode error, asking for a keyframe: " + error);
			m_videoNeedKey = true;
			video::Feedback fb;
			fb.streamId = h.streamId;
			fb.lastFrameNo = h.frameNo;
			fb.flags = video::kFeedbackKeyframe;
			SendVideoMessage(video::kFeedback, video::EncodeFeedback(fb));
		}
		break;
	}
	case video::kStatus: {
		video::Status st;
		if (!video::DecodeStatus(p, n, st))
			return;
		VideoLog(st.code == video::kStatusStopped ? 1 : 0,
			"server ended video mode (" + std::to_string(st.code) + "): " + st.message);
		m_videoActive = false;
		{
			omni_mutex_lock l(m_videoMutex);
			m_videoEndReason = st.message;
		}
		PostMessage(m_hwndMain, VideoModeEndedMessage(), st.code, 0);
		break;
	}
	default:
		break;
	}
}

// Reader thread: NV12 picture -> BGRA framebuffer.
void ClientConnection::BlitVideoPicture(const VideoDecoder::Picture& pic)
{
	video::Config cfg = m_videoConfig;
	cfg.width = static_cast<uint16_t>(pic.width);
	cfg.height = static_cast<uint16_t>(pic.height);
	video::ClipResult r;
	RECT rc;
	{
		omni_mutex_lock l(m_bitmapdcMutex);
		if (!m_DIBbits || !FramebufferIsBgra32(m_myFormat))
			return;
		if (!video::ClipToFramebuffer(cfg, m_si.framebufferWidth, m_si.framebufferHeight, r))
			return;
		const int srcX = r.srcX & ~1; // chroma is sampled in pairs
		const int dstStride = m_si.framebufferWidth * 4;
		uint8_t* dst = static_cast<uint8_t*>(m_DIBbits) + static_cast<size_t>(r.dstY) * dstStride + r.dstX * 4;
		libyuv::NV12ToARGB(pic.y + static_cast<size_t>(r.srcY) * pic.pitch + srcX, pic.pitch,
			pic.uv + static_cast<size_t>(r.srcY / 2) * pic.pitch + srcX, pic.pitch,
			dst, dstStride, r.width, r.height);
		rc.left = r.dstX;
		rc.top = r.dstY;
		rc.right = r.dstX + r.width;
		rc.bottom = r.dstY + r.height;
	}
	InvalidateScreenRect(&rc);
}
