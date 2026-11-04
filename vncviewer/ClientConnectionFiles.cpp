// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// The new file transfer window and the file-only connection mode
// (docs/design.md section 6).
//
// Normal connections: "File Transfer" (system menu, toolbar) opens the new
// window when the server offers the file manager channel, else the classic
// window.
//
// File-only mode (connect dialog option, /filemode): the desktop window
// stays hidden and no screen updates are requested; the file transfer
// window opens as soon as the channel is negotiated, and closing it ends
// the connection. If the server has no file manager channel (an original
// UltraVNC server), the classic window is used; if file transfer is not
// allowed, the user is told why.

#include "stdhdrs.h"
#include "vncviewer.h"
#include "ClientConnection.h"
#include "ClientConnectionExt.h"
#include "FileManagerWindow.h"
#include "common/FileManClient.h"

using namespace extmsg;

namespace {

enum FileChannelEvent : WPARAM {
	kEventDown = 0,      // channel gone or refused
	kEventUp = 1,        // channel usable
	kEventStart = 2,     // file mode: the connection is up
	kEventTimeout = 3,   // file mode: no file channel in time
};

constexpr UINT_PTR kFileModeTimer = 0x46D0;

UINT FileMessage()
{
	static const UINT msg = RegisterWindowMessageW(L"UltraVNC.FileChannel");
	return msg;
}
constexpr UINT kFileModeWaitMs = 5000;

void CALLBACK FileModeTimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD)
{
	KillTimer(hwnd, id);
	PostMessage(hwnd, FileMessage(), kEventTimeout, 0);
}

} // namespace

UINT ClientConnection::FileChannelMessage()
{
	return FileMessage();
}

void ClientConnection::OnFileChannel(bool usable)
{
	if (m_hwndMain)
		PostMessage(m_hwndMain, FileChannelMessage(), usable ? kEventUp : kEventDown, 0);
}

void ClientConnection::OnFileChannelEvent(WPARAM event)
{
	switch (event) {
	case kEventUp:
		if (m_fileWindow)
			m_fileWindow->SetConnected(true);
		else if (m_opts->m_fileMode)
			OpenFileManager();
		break;
	case kEventDown:
		if (m_fileWindow)
			m_fileWindow->SetConnected(m_ext->ChannelUsable(kChFiles));
		else if (m_opts->m_fileMode && !m_fileModeResolved && m_ext->IsNegotiated())
			FileModeFallback(); // negotiated without the file channel
		break;
	case kEventStart:
		if (m_ext->ChannelUsable(kChFiles)) {
			if (!m_fileWindow)
				OpenFileManager();
		} else if (!m_fileWindow) {
			SetTimer(m_hwndMain, kFileModeTimer, kFileModeWaitMs, &FileModeTimerProc);
		}
		break;
	case kEventTimeout:
		if (!m_fileWindow && !m_fileModeResolved)
			FileModeFallback();
		break;
	default:
		break;
	}
}

void ClientConnection::OpenFileManager()
{
	if (m_fileWindow) {
		m_fileWindow->Activate();
		return;
	}
	m_fileModeResolved = true;
	FileManagerWindow::Options o;
	o.client = m_ext->Files();
	o.host = m_host;
	o.fileMode = m_opts->m_fileMode;
	o.icon = reinterpret_cast<HICON>(GetClassLongPtr(m_hwndMain, GCLP_HICON));
	o.onClosed = [this] {
		m_fileWindow = nullptr;
		if (m_opts->m_fileMode && !m_fileWindowClosing)
			CloseFileModeConnection();
	};
	m_fileWindow = FileManagerWindow::Open(o);
	if (m_fileWindow && m_opts->m_fileMode) {
		// The file window is the connection: make sure the desktop window
		// is out of sight and the file window is in front.
		ShowWindow(m_hwndMain, SW_HIDE);
		m_fileWindow->Activate();
	}
	if (!m_fileWindow) {
		MessageBoxW(m_hwndMain, L"The file transfer window could not be opened.", L"UltraVNC Viewer",
			MB_OK | MB_ICONERROR);
		if (m_opts->m_fileMode)
			CloseFileModeConnection();
	}
}

void ClientConnection::FileModeFallback()
{
	m_fileModeResolved = true;
	KillTimer(m_hwndMain, kFileModeTimer);
	if (m_ext->IsNegotiated()) {
		// A new server that does not allow it for us.
		MessageBoxW(NULL,
			L"The server does not allow file transfer for this connection.\n\n"
			L"File transfer may be turned off in the server settings, or this connection may not "
			L"be allowed to use keyboard and mouse.",
			L"UltraVNC Viewer - File transfer", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
		CloseFileModeConnection();
		return;
	}
	if (m_fServerKnowsFileTransfer) {
		// An older server: the classic file transfer window.
		PostMessage(m_hwndMain, WM_SYSCOMMAND, ID_FILETRANSFER, 0);
		return;
	}
	MessageBoxW(NULL, L"This server does not support file transfer.", L"UltraVNC Viewer - File transfer",
		MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
	CloseFileModeConnection();
}

// Like the user closing the (hidden) desktop window: no question, no
// automatic reconnect, and the hidden window's position is not saved.
void ClientConnection::CloseFileModeConnection()
{
	m_bClosedByUser = true;
	if (m_hwndMain)
		PostMessage(m_hwndMain, WM_CLOSE, 0, 1);
}
