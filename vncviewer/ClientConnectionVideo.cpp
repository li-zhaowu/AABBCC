// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Video mode, viewer side (docs/design.md section 3). The server streams one
// monitor as H.264/HEVC/AV1 on extension channel 4; we decode it with Media
// Foundation and write the pictures into the normal framebuffer, so scaling,
// full screen, snapshots etc. keep working. While video mode is on, no
// framebuffer updates are requested.

#include "stdhdrs.h"
#include "vncviewer.h"
#include "ClientConnection.h"
#include "ClientConnectionExt.h"
#include "Exception.h"
#include "VideoDecoder.h"
#include "VideoCursor.h"
#include "MediaSettings.h"
#include "UiText.h"
#include "common/VideoProto.h"

#include <libyuv.h> // vcpkg port "libyuv"

using namespace extmsg;

UINT ClientConnection::VideoModeEndedMessage()
{
	static const UINT msg = RegisterWindowMessageW(L"UltraVNC.VideoModeEnded");
	return msg;
}

UINT ClientConnection::VideoCursorChangedMessage()
{
	static const UINT msg = RegisterWindowMessageW(L"UltraVNC.VideoCursorChanged");
	return msg;
}

// Reader thread: turn the server's pointer shape into a cursor and make the
// UI thread show it.
void ClientConnection::SetVideoCursor(const video::Cursor& c)
{
	HCURSOR cursor = CreateCursorFromShape(c);
	// kCursorHidden: NULL cursor.
	if (c.type != video::kCursorHidden && !cursor)
		return; // keep the previous one rather than showing nothing

	{
		omni_mutex_lock l(m_videoMutex);
		// The UI thread may still be using the current cursor; destroy the
		// one before it.
		if (m_videoCursorOld)
			DestroyCursor(m_videoCursorOld);
		m_videoCursorOld = m_videoCursor;
		m_videoCursor = cursor;
	}
	m_videoCursorValid = true;
	PostMessage(m_hwndcn, VideoCursorChangedMessage(), 0, 0);
}

void ClientConnection::DestroyVideoCursors()
{
	omni_mutex_lock l(m_videoMutex);
	m_videoCursorValid = false;
	if (m_videoCursorOld)
		DestroyCursor(m_videoCursorOld);
	if (m_videoCursor)
		DestroyCursor(m_videoCursor);
	m_videoCursorOld = m_videoCursor = NULL;
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

UINT ClientConnection::MediaRestoreMessage()
{
	static const UINT msg = RegisterWindowMessageW(L"UltraVNC.MediaRestore");
	return msg;
}

// Reader thread, when the extension channel is (re)negotiated.
void ClientConnection::OnExtReady()
{
	if (m_hwndMain && (m_restoreVideo || m_restoreAudio))
		PostMessage(m_hwndMain, MediaRestoreMessage(), 0, 0);
}

// UI thread: after a reconnect, turn video mode and remote audio back on if
// they were on when the connection dropped (media setting, default on).
void ClientConnection::RestoreMediaAfterReconnect()
{
	const bool video = m_restoreVideo.exchange(false);
	const bool audio = m_restoreAudio.exchange(false);
	if (m_opts->m_fileMode || !MediaSettings::Load().restoreAfterReconnect)
		return;
	if (video && !m_videoRequested && m_ext->ChannelUsable(kChVideo)) {
		VideoLog(1, "turning video mode back on after the reconnect");
		ToggleVideoMode();
	}
	if (audio && !m_audioRequested && m_ext->ChannelUsable(kChAudio)) {
		VideoLog(1, "turning remote audio back on after the reconnect");
		ToggleRemoteAudio();
	}
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
		problem = Tr(L"The server does not offer video mode.\n\n"
		             L"Video mode needs the new UltraVNC server on a machine with an NVIDIA GPU "
		             L"and a current NVIDIA driver (see the server log for details).");
	else if (!FramebufferIsBgra32(m_myFormat))
		problem = Tr(L"Video mode needs the \"Full colors\" (32 bit) pixel format.\n\n"
		             L"Change it in the connection options and try again.");
	else if (m_nServerScale != 1)
		problem = Tr(L"Video mode does not work with server side scaling.\n\n"
		             L"Set server scaling to 1/1 and try again.");
	if (problem) {
		MessageBoxW(m_hwndMain, problem, Tr(L"UltraVNC Viewer - Video mode"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	video::Start start = MakeVideoStart();
	m_videoNeedKey = true;
	m_videoRequested = true;
	// Showing one window: the server only takes the parameters, then the
	// window is selected again to be sent as video.
	if (m_windowRequested)
		start.flags |= video::kStartParamsOnly;
	if (!SendVideoMessage(video::kStart, video::EncodeStart(start))) {
		m_videoRequested = false;
		return;
	}
	if (m_windowRequested)
		SelectWindowTransport();
	CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_VIDEOMODE, MF_BYCOMMAND | MF_CHECKED);
	std::string offered;
	for (uint8_t c : video::CodecCandidates(start))
		offered += std::string(offered.empty() ? "" : ", ") + video::CodecName(c);
	VideoLog(1, "requested, codecs " + offered);
}

// Start parameters: frame rate, bitrate and codec from the media settings
// (defaults 60 fps, up to 20 Mbps, automatic codec).
video::Start ClientConnection::MakeVideoStart()
{
	const MediaSettings settings = MediaSettings::Load();
	video::Start start;
	start.maxFps = static_cast<uint16_t>(settings.videoFps);
	start.maxBitrateKbps = static_cast<uint32_t>(settings.videoMaxMbps) * 1000;
	if (settings.videoCodec != MediaSettings::kCodecAuto) {
		// The user's choice; H.264 stays as the fallback if the server's GPU
		// cannot encode it.
		start.codec = settings.videoCodec == MediaSettings::kCodecAv1 ? video::kCodecAv1
			: settings.videoCodec == MediaSettings::kCodecHevc ? video::kCodecHevc : video::kCodecH264;
		start.codecMask = static_cast<uint8_t>(video::CodecBit(start.codec) | video::CodecBit(video::kCodecH264));
		return start;
	}
	// AV1 and HEVC give better pictures per bit, but decoding them in
	// software is heavy: offer them only if this machine decodes them on the
	// GPU. The server takes the best one its GPU can encode (AV1: RTX 40 and
	// newer), else H.264.
	start.codecMask = video::CodecBit(video::kCodecH264);
	if (!m_videoForceSoftware) {
		for (uint8_t c : { uint8_t(video::kCodecHevc), uint8_t(video::kCodecAv1) })
			if (VideoDecoder::HardwareAvailable(c))
				start.codecMask |= video::CodecBit(c);
	}
	start.codec = (start.codecMask & video::CodecBit(video::kCodecAv1)) ? video::kCodecAv1
		: (start.codecMask & video::CodecBit(video::kCodecHevc)) ? video::kCodecHevc : video::kCodecH264;
	return start;
}

// UI thread.
void ClientConnection::StopVideoMode(bool tellServer)
{
	const bool wasActive = m_videoActive.exchange(false);
	const bool wasRequested = m_videoRequested.exchange(false);
	// Back to the local cursor.
	m_videoCursorValid = false;
	PostMessage(m_hwndcn, VideoCursorChangedMessage(), 0, 0);
	if (tellServer)
		SendVideoMessage(video::kStop, {});
	// Showing one window as video: switch it back to lossless tiles. (When
	// the server ended the video, it switched to tiles on its own.)
	if (tellServer && m_windowRequested)
		SelectWindowTransport();
	CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_VIDEOMODE, MF_BYCOMMAND | MF_UNCHECKED);
	// Back to classic updates: one full picture, then incremental again.
	// (When the server ended video mode, m_videoActive is already false but
	// incremental requests were suppressed, so this request is needed too.)
	if ((wasActive || wasRequested) && !m_windowRequested)
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
		std::wstring text = Tr(L"Video mode ended:\n\n");
		const int n = MultiByteToWideChar(CP_UTF8, 0, reason.c_str(), -1, NULL, 0);
		if (n > 1) {
			std::wstring w(n - 1, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, reason.c_str(), -1, &w[0], n);
			text += w;
		} else {
			text += Tr(L"(no reason given)");
		}
		MessageBoxW(m_hwndMain, text.c_str(), Tr(L"UltraVNC Viewer - Video mode"), MB_OK | MB_ICONINFORMATION);
	}
}

// Connection (re)start.
void ClientConnection::ResetVideoState()
{
	m_videoActive = false;
	m_videoRequested = false;
	m_videoCursorValid = false;
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
		if (!m_videoDecoder->Open(cfg.codec, cfg.width, cfg.height, cfg.fps, !m_videoForceSoftware, error)) {
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
			std::to_string(cfg.height) + " at " + std::to_string(cfg.x) + "," + std::to_string(cfg.y) + ", " +
			video::CodecName(cfg.codec) + ", " +
			(m_videoDecoder->IsHardware() ? "GPU" : "software") + " decoding");
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
			if (m_videoDecoder->IsHardware()) {
				// Continue in software for the rest of this session.
				m_videoForceSoftware = true;
				std::string reopenError;
				if (m_videoDecoder->Open(m_videoConfig.codec, m_videoConfig.width, m_videoConfig.height,
						m_videoConfig.fps, false, reopenError))
					VideoLog(0, "GPU decoding failed, switched to software decoding");
				else
					VideoLog(0, "software decoder: " + reopenError);
			}
			video::Feedback fb;
			fb.streamId = h.streamId;
			fb.lastFrameNo = h.frameNo;
			fb.flags = video::kFeedbackKeyframe;
			SendVideoMessage(video::kFeedback, video::EncodeFeedback(fb));
		}
		break;
	}
	case video::kCursor: {
		video::Cursor c;
		if (m_videoRequested && video::DecodeCursor(p, n, c))
			SetVideoCursor(c);
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
