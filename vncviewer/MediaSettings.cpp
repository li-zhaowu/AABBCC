// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "MediaSettings.h"
#include "UiText.h"

#include <commctrl.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

const wchar_t kKey[] = L"Software\\UltraVNC\\Viewer\\Media";

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD def)
{
	DWORD v = 0, size = sizeof(v), type = 0;
	if (key && RegQueryValueExW(key, name, NULL, &type, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS &&
	    type == REG_DWORD)
		return v;
	return def;
}

void WriteDword(HKEY key, const wchar_t* name, DWORD v)
{
	RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

// --- in-memory dialog template (no .rc: the language DLLs would not have it) --

class DialogTemplate {
public:
	DialogTemplate(const wchar_t* title, short cx, short cy)
	{
		Align(4);
		DLGTEMPLATE t = {};
		t.style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
		t.cx = cx;
		t.cy = cy;
		Append(&t, sizeof(t));
		Word(0);           // no menu
		Word(0);           // default class
		String(title);
		Word(9);           // font size
		String(L"MS Shell Dlg");
	}
	void Add(WORD atom, const wchar_t* text, DWORD style, short x, short y, short cx, short cy, WORD id)
	{
		Item(style, x, y, cx, cy, id);
		Word(0xFFFF);
		Word(atom);
		String(text);
		Word(0);           // no creation data
	}
	// A control of a registered class (common controls).
	void Add(const wchar_t* cls, DWORD style, short x, short y, short cx, short cy, WORD id)
	{
		Item(style, x, y, cx, cy, id);
		String(cls);
		String(L"");
		Word(0);
	}
	const DLGTEMPLATE* Get() const { return reinterpret_cast<const DLGTEMPLATE*>(m_data.data()); }

private:
	void Item(DWORD style, short x, short y, short cx, short cy, WORD id)
	{
		Align(4);
		DLGITEMTEMPLATE it = {};
		it.style = WS_CHILD | WS_VISIBLE | style;
		it.x = x;
		it.y = y;
		it.cx = cx;
		it.cy = cy;
		it.id = id;
		Append(&it, sizeof(it));
		++Header()->cdit;
	}
	DLGTEMPLATE* Header() { return reinterpret_cast<DLGTEMPLATE*>(m_data.data()); }
	void Append(const void* p, size_t n)
	{
		const auto* b = static_cast<const BYTE*>(p);
		m_data.insert(m_data.end(), b, b + n);
	}
	void Word(WORD w) { Append(&w, sizeof(w)); }
	void String(const wchar_t* s) { Append(s, (wcslen(s) + 1) * sizeof(wchar_t)); }
	void Align(size_t a)
	{
		while (m_data.size() % a)
			m_data.push_back(0);
	}
	std::vector<BYTE> m_data;
};

enum : WORD { kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082, kCombo = 0x0085 };
enum : WORD {
	kIdBitrate = 100, kIdBitrateSpin, kIdFps, kIdCodec, kIdAudio, kIdMute, kIdRestore,
};

const int kFps[] = { 30, 60, 120 };
const int kAudio[] = { 96, 128, 160, 192 };

INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM)
{
	switch (msg) {
	case WM_INITDIALOG: {
		const MediaSettings s = MediaSettings::Load();
		SendDlgItemMessageW(hwnd, kIdBitrateSpin, UDM_SETRANGE32, 1, 200);
		SendDlgItemMessageW(hwnd, kIdBitrateSpin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(GetDlgItem(hwnd, kIdBitrate)), 0);
		SetDlgItemInt(hwnd, kIdBitrate, static_cast<UINT>(s.videoMaxMbps), FALSE);
		for (int f : kFps)
			SendDlgItemMessageW(hwnd, kIdFps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(TrF(L"%d fps", f).c_str()));
		SendDlgItemMessageW(hwnd, kIdFps, CB_SETCURSEL,
			static_cast<WPARAM>(std::find(std::begin(kFps), std::end(kFps), s.videoFps) - std::begin(kFps)), 0);
		const wchar_t* codecs[] = { Tr(L"Automatic (best the GPUs support)"), L"H.264", L"HEVC (H.265)", L"AV1" };
		for (const wchar_t* c : codecs)
			SendDlgItemMessageW(hwnd, kIdCodec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c));
		SendDlgItemMessageW(hwnd, kIdCodec, CB_SETCURSEL, static_cast<WPARAM>(s.videoCodec), 0);
		for (int a : kAudio)
			SendDlgItemMessageW(hwnd, kIdAudio, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(TrF(L"%d kbps", a).c_str()));
		SendDlgItemMessageW(hwnd, kIdAudio, CB_SETCURSEL,
			static_cast<WPARAM>(std::find(std::begin(kAudio), std::end(kAudio), s.audioKbps) - std::begin(kAudio)), 0);
		CheckDlgButton(hwnd, kIdMute, s.muteRemote ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hwnd, kIdRestore, s.restoreAfterReconnect ? BST_CHECKED : BST_UNCHECKED);
		return TRUE;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK) {
			MediaSettings s;
			BOOL ok = FALSE;
			s.videoMaxMbps = static_cast<int>(GetDlgItemInt(hwnd, kIdBitrate, &ok, FALSE));
			if (!ok || s.videoMaxMbps < 1 || s.videoMaxMbps > 200) {
				MessageBoxW(hwnd, Tr(L"The maximum bitrate must be between 1 and 200 Mbps."), Tr(L"Media Settings"),
					MB_OK | MB_ICONWARNING);
				SetFocus(GetDlgItem(hwnd, kIdBitrate));
				return TRUE;
			}
			const auto fps = SendDlgItemMessageW(hwnd, kIdFps, CB_GETCURSEL, 0, 0);
			s.videoFps = kFps[fps >= 0 && fps < 3 ? fps : 1];
			const auto codec = SendDlgItemMessageW(hwnd, kIdCodec, CB_GETCURSEL, 0, 0);
			s.videoCodec = codec >= 0 && codec <= 3 ? static_cast<int>(codec) : MediaSettings::kCodecAuto;
			const auto audio = SendDlgItemMessageW(hwnd, kIdAudio, CB_GETCURSEL, 0, 0);
			s.audioKbps = kAudio[audio >= 0 && audio < 4 ? audio : 1];
			s.muteRemote = IsDlgButtonChecked(hwnd, kIdMute) == BST_CHECKED;
			s.restoreAfterReconnect = IsDlgButtonChecked(hwnd, kIdRestore) == BST_CHECKED;
			s.Save();
			EndDialog(hwnd, IDOK);
			return TRUE;
		}
		if (LOWORD(wp) == IDCANCEL) {
			EndDialog(hwnd, IDCANCEL);
			return TRUE;
		}
		break;
	default:
		break;
	}
	return FALSE;
}

} // namespace

MediaSettings MediaSettings::Load()
{
	MediaSettings s;
	HKEY key = NULL;
	RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key);
	s.videoMaxMbps = static_cast<int>(ReadDword(key, L"VideoMaxMbps", static_cast<DWORD>(s.videoMaxMbps)));
	s.videoFps = static_cast<int>(ReadDword(key, L"VideoFps", static_cast<DWORD>(s.videoFps)));
	s.videoCodec = static_cast<int>(ReadDword(key, L"VideoCodec", static_cast<DWORD>(s.videoCodec)));
	s.audioKbps = static_cast<int>(ReadDword(key, L"AudioKbps", static_cast<DWORD>(s.audioKbps)));
	s.muteRemote = ReadDword(key, L"MuteRemote", s.muteRemote ? 1 : 0) != 0;
	s.restoreAfterReconnect = ReadDword(key, L"RestoreAfterReconnect", s.restoreAfterReconnect ? 1 : 0) != 0;
	if (key)
		RegCloseKey(key);
	s.Clamp();
	return s;
}

void MediaSettings::Clamp()
{
	videoMaxMbps = (std::max)(1, (std::min)(200, videoMaxMbps));
	if (videoFps != 30 && videoFps != 60 && videoFps != 120)
		videoFps = 60;
	if (videoCodec < kCodecAuto || videoCodec > kCodecAv1)
		videoCodec = kCodecAuto;
	if (audioKbps != 96 && audioKbps != 128 && audioKbps != 160 && audioKbps != 192)
		audioKbps = 128;
}

void MediaSettings::Save() const
{
	HKEY key = NULL;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS)
		return;
	WriteDword(key, L"VideoMaxMbps", static_cast<DWORD>(videoMaxMbps));
	WriteDword(key, L"VideoFps", static_cast<DWORD>(videoFps));
	WriteDword(key, L"VideoCodec", static_cast<DWORD>(videoCodec));
	WriteDword(key, L"AudioKbps", static_cast<DWORD>(audioKbps));
	WriteDword(key, L"MuteRemote", muteRemote ? 1 : 0);
	WriteDword(key, L"RestoreAfterReconnect", restoreAfterReconnect ? 1 : 0);
	RegCloseKey(key);
}

bool ShowMediaSettingsDialog(HWND owner)
{
	// Layout in dialog units.
	const short W = 260, L = 12, LW = 110, C = 128, CW = 120;
	DialogTemplate t(Tr(L"Media Settings"), W, 214);
	t.Add(kButton, Tr(L"Video mode (GPU)"), BS_GROUPBOX, 6, 6, W - 12, 72, 0xFFFF);
	t.Add(kStatic, Tr(L"Maximum bitrate (Mbps):"), SS_LEFT, L, 22, LW, 10, 0xFFFF);
	t.Add(kEdit, L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_RIGHT, C, 20, 50, 13, kIdBitrate);
	t.Add(UPDOWN_CLASSW, UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, kIdBitrateSpin);
	t.Add(kStatic, Tr(L"Frame rate:"), SS_LEFT, L, 40, LW, 10, 0xFFFF);
	t.Add(kCombo, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, C, 38, CW, 80, kIdFps);
	t.Add(kStatic, Tr(L"Codec:"), SS_LEFT, L, 58, LW, 10, 0xFFFF);
	t.Add(kCombo, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, C, 56, CW, 80, kIdCodec);
	t.Add(kButton, Tr(L"Remote audio"), BS_GROUPBOX, 6, 84, W - 12, 52, 0xFFFF);
	t.Add(kStatic, Tr(L"Bitrate:"), SS_LEFT, L, 100, LW, 10, 0xFFFF);
	t.Add(kCombo, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, C, 98, CW, 80, kIdAudio);
	t.Add(kButton, Tr(L"Mute the remote computer's speakers while listening"), BS_AUTOCHECKBOX | WS_TABSTOP, L, 118,
		W - 24, 10, kIdMute);
	t.Add(kButton, Tr(L"Turn video mode and remote audio back on after a reconnect"), BS_AUTOCHECKBOX | WS_TABSTOP, L,
		144, W - 18, 10, kIdRestore);
	t.Add(kStatic, Tr(L"Changes apply the next time video mode or remote audio is turned on."), SS_LEFT, 6, 162, W - 12,
		20, 0xFFFF);
	t.Add(kButton, Tr(L"OK"), BS_DEFPUSHBUTTON | WS_TABSTOP, W - 114, 192, 50, 14, IDOK);
	t.Add(kButton, Tr(L"Cancel"), BS_PUSHBUTTON | WS_TABSTOP, W - 58, 192, 50, 14, IDCANCEL);
	return DialogBoxIndirectParamW(GetModuleHandleW(NULL), t.Get(), owner, DialogProc, 0) == IDOK;
}
