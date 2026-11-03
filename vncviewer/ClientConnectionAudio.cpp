// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Remote audio, viewer side (docs/design.md section 5). Switched on from the
// system menu; the server then sends what it plays as AAC on extension
// channel 3 and mutes its own speakers while it does.

#include "stdhdrs.h"
#include "vncviewer.h"
#include "ClientConnection.h"
#include "ClientConnectionExt.h"
#include "AudioPlayer.h"
#include "common/AudioProto.h"

using namespace extmsg;

UINT ClientConnection::AudioEventMessage()
{
	static const UINT msg = RegisterWindowMessageW(L"UltraVNC.RemoteAudioEnded");
	return msg;
}

static void AudioLog(int level, const std::string& msg)
{
	vnclog.Print(level, _T("%hs\n"), msg.c_str());
}

bool ClientConnection::SendAudioMessage(uint8_t subtype, const std::vector<uint8_t>& payload)
{
	return m_ext->Send(kPrioControl, kChAudio, subtype, payload);
}

// UI thread (system menu).
void ClientConnection::ToggleRemoteAudio()
{
	if (m_audioRequested) {
		StopRemoteAudio(true);
		return;
	}
	if (!m_ext->ChannelUsable(kChAudio)) {
		MessageBoxW(m_hwndMain,
			L"The server does not offer remote audio.\n\n"
			L"Remote audio needs the new UltraVNC server.",
			L"UltraVNC Viewer - Remote audio", MB_OK | MB_ICONINFORMATION);
		return;
	}
	if (!m_audioPlayer)
		m_audioPlayer.reset(new AudioPlayer(&AudioLog));

	audio::Start start; // AAC, 128 kbps, remote speakers muted
	m_audioRequested = true;
	if (!SendAudioMessage(audio::kStart, audio::EncodeStart(start))) {
		m_audioRequested = false;
		return;
	}
	CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_REMOTEAUDIO, MF_BYCOMMAND | MF_CHECKED);
	AudioLog(1, "audio: requested");
}

// UI thread.
void ClientConnection::StopRemoteAudio(bool tellServer)
{
	m_audioRequested = false;
	if (tellServer)
		SendAudioMessage(audio::kStop, {});
	if (m_audioPlayer)
		m_audioPlayer->Close();
	if (m_hwndMain)
		CheckMenuItem(GetSystemMenu(m_hwndMain, FALSE), ID_REMOTEAUDIO, MF_BYCOMMAND | MF_UNCHECKED);
}

// UI thread, posted by the reader thread when audio ended on its own.
void ClientConnection::OnAudioEvent(WPARAM statusCode)
{
	std::string reason;
	{
		omni_mutex_lock l(m_audioMutex);
		reason = m_audioEndReason;
	}
	if (!m_audioRequested)
		return; // the user switched it off in the meantime
	// Tell the server too: when the decoder failed here, it is still sending.
	StopRemoteAudio(true);
	std::wstring text = L"Remote audio ended:\n\n";
	const int n = MultiByteToWideChar(CP_UTF8, 0, reason.c_str(), -1, NULL, 0);
	if (n > 1) {
		std::wstring w(n - 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, reason.c_str(), -1, &w[0], n);
		text += w;
	} else {
		text += L"(no reason given)";
	}
	MessageBoxW(m_hwndMain, text.c_str(), L"UltraVNC Viewer - Remote audio", MB_OK | MB_ICONINFORMATION);
}

// Connection (re)start and suspend. Audio stays off until switched on again.
void ClientConnection::ResetAudioState()
{
	StopRemoteAudio(false);
}

// Reader thread.
void ClientConnection::OnAudioMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	const uint8_t* p = payload.data();
	const size_t n = payload.size();
	switch (subtype) {
	case audio::kFormat: {
		audio::Format fmt;
		if (!audio::DecodeFormat(p, n, fmt) || !m_audioRequested || !m_audioPlayer)
			return;
		std::string error;
		if (!m_audioPlayer->Open(fmt, error)) {
			AudioLog(0, "audio: decoder: " + error);
			{
				omni_mutex_lock l(m_audioMutex);
				m_audioEndReason = "The audio decoder could not be started: " + error;
			}
			PostMessage(m_hwndMain, AudioEventMessage(), audio::kStatusUnsupported, 0);
			return;
		}
		// Switched off while opening: do not keep playing.
		if (!m_audioRequested)
			m_audioPlayer->Close();
		AudioLog(1, "audio: stream " + std::to_string(fmt.streamId) + ", " + std::to_string(fmt.sampleRate) +
			" Hz, " + std::to_string(fmt.channels) + " channel(s), " + std::to_string(fmt.bitrateKbps) + " kbps");
		break;
	}
	case audio::kData: {
		audio::DataHeader h;
		if (!m_audioRequested || !m_audioPlayer || !audio::DecodeDataHeader(p, n, h))
			return;
		m_audioPlayer->Push(h.streamId, p + audio::kDataHeaderSize, n - audio::kDataHeaderSize);
		break;
	}
	case audio::kStatus: {
		audio::Status st;
		if (!audio::DecodeStatus(p, n, st))
			return;
		if (st.code == audio::kStatusStopped)
			return; // confirms our Stop
		{
			omni_mutex_lock l(m_audioMutex);
			m_audioEndReason = st.message;
		}
		PostMessage(m_hwndMain, AudioEventMessage(), st.code, 0);
		break;
	}
	default:
		break;
	}
}
