// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Viewer settings for video mode and remote audio (docs/design.md section
// 9), kept per user in HKCU\Software\UltraVNC\Viewer\Media, and the dialog
// that edits them (system menu "Media Settings...").

#pragma once

#include <windows.h>

#include <cstdint>

struct MediaSettings {
	enum Codec : int { kCodecAuto = 0, kCodecH264 = 1, kCodecHevc = 2, kCodecAv1 = 3 };

	int videoMaxMbps = 20;        // 1 .. 200
	int videoFps = 60;            // 30, 60 or 120
	int videoCodec = kCodecAuto;  // Auto: the best both GPUs handle
	int audioKbps = 128;          // 96, 128, 160 or 192
	bool muteRemote = true;       // silence the remote speakers while listening
	bool restoreAfterReconnect = true; // video mode / remote audio come back after a reconnect

	static MediaSettings Load();
	void Save() const;
	void Clamp();
};

// Modal dialog; true if the user pressed OK (the settings were saved).
bool ShowMediaSettingsDialog(HWND owner);
