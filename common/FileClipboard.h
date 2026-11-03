// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Copy and paste of files between viewer and server (docs/design.md, section
// 2). Shared by both sides; each side plays both roles:
//
//  Source: when the local clipboard gets files (CF_HDROP, e.g. Ctrl+C in
//    Explorer) the file list is sent to the peer as an Offer. File contents
//    are read and streamed only when the peer asks for them.
//
//  Target: an Offer from the peer is put on the local clipboard as virtual
//    files (CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS). When the user pastes,
//    Explorer reads each file through an IStream that pulls the bytes from
//    the peer on demand, with read-ahead.
//
// Threads:
//  - an STA thread owns the clipboard listener window and the OLE clipboard
//    object; COM calls from Explorer (GetData, IStream::Read) run here;
//  - a worker thread serves the peer's read requests (source role);
//  - OnMessage() is called on the transport's reader thread.

#pragma once

#ifdef _WIN32

#include <winsock2.h>
#include <windows.h>
#include <objidl.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ExtChannel.h"
#include "FileClipProto.h"

namespace fileclip {

struct Callbacks {
	// Queue a message on the extension channel (channel kChFileClip).
	// Returns false if the channel is not usable.
	std::function<bool(extmsg::ExtPriority prio, uint8_t subtype,
	                   const std::vector<uint8_t>& payload)> send;
	// level 0 = error, 1 = info, 2 = debug
	std::function<void(int level, const std::string& msg)> log;
	// Optional. Called before touching local files (listing, opening). Return
	// false to refuse file access. Set *token to a token to impersonate
	// while doing so; the engine closes it. Leave it NULL to use the
	// process's own identity.
	std::function<bool(HANDLE* token)> acquireUserToken;
	// Register for clipboard change notifications. The sequence number is
	// polled as well, so this is only turned off by tests of the polling.
	bool useClipboardListener = true;
};

class RemoteDataObject;
class RemoteStream;
struct RemoteOffer;

class FileClipboard {
public:
	explicit FileClipboard(Callbacks cb);
	~FileClipboard();

	FileClipboard(const FileClipboard&) = delete;
	FileClipboard& operator=(const FileClipboard&) = delete;

	// Start the threads. Returns false if the clipboard thread failed.
	bool Start();
	// Remove our data from the clipboard, fail pending reads, join threads.
	void Stop();

	// The peer negotiated the file clipboard channel. Offers the current
	// clipboard if it holds files.
	void OnConnected();
	// The connection is gone: fail pending reads, take our virtual files off
	// the clipboard, close open files.
	void OnDisconnected();

	// A complete message on channel kChFileClip (transport reader thread).
	void OnMessage(uint8_t subtype, std::vector<uint8_t>& payload);

	// Allow COM calls into this process from other users' processes in the
	// same session (needed when the server runs as LocalSystem and the
	// user's Explorer pastes from our clipboard object). Call once, early,
	// on a thread with COM initialised. Returns false if it was too late.
	static bool InitComSecurityForService(std::string* error);

private:
	friend class RemoteDataObject;
	friend class RemoteStream;

	struct LocalFile {
		std::wstring path;  // absolute
		bool directory = false;
	};
	struct ReadJob {
		bool cancel = false;
		Read read;          // valid if !cancel
		uint32_t streamId = 0;
	};
	struct OpenFile {
		HANDLE handle = INVALID_HANDLE_VALUE;
	};

	// Clipboard (STA) thread.
	void ClipThreadMain();
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	void OnLocalClipboardChanged(bool force);
	void ApplyRemoteOffer(RemoteOffer* offer);
	void ApplyRemoteRevoke(uint32_t seq);
	void RemoveRemoteObject();

	// Source role.
	bool ReadClipboardFiles(std::vector<std::wstring>& paths);
	bool BuildOffer(const std::vector<std::wstring>& paths, Offer& offer,
	                std::vector<LocalFile>& files);
	void RevokeLocalOffer();
	void WorkerMain();
	void ServeRead(const Read& rd);
	void CloseStream(uint32_t streamId);
	void CloseAllStreams();

	// Target role (called by RemoteStream).
	bool Send(extmsg::ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload);
	uint32_t NewStreamId();
	void RegisterStream(uint32_t id, RemoteStream* s);
	void UnregisterStream(uint32_t id);
	bool IsConnected() const { return m_connected.load(); }
	bool IsStopping() const { return m_stopping.load(); }
	void FailAllStreams();

	void Log(int level, const std::string& msg);

	Callbacks m_cb;
	std::atomic<bool> m_connected{false};
	std::atomic<bool> m_stopping{false};
	bool m_started = false;

	// Clipboard thread state.
	std::thread m_clipThread;
	HWND m_hwnd = NULL;
	HANDLE m_readyEvent = NULL;
	bool m_comOk = false;
	DWORD m_lastClipSeq = 0; // clipboard sequence number last looked at
	RemoteDataObject* m_remoteObject = nullptr; // our object on the clipboard
	uint32_t m_remoteSeq = 0;

	// Source state (clipboard thread writes, worker reads).
	std::mutex m_srcMutex;
	uint32_t m_localSeq = 0;      // 0 = nothing offered
	uint32_t m_nextLocalSeq = 1;
	std::vector<LocalFile> m_localFiles;

	// Worker.
	std::thread m_worker;
	std::mutex m_jobMutex;
	std::condition_variable m_jobCond;
	std::deque<ReadJob> m_jobs;
	std::set<uint32_t> m_cancelled; // streams cancelled while queued/served
	bool m_workerStop = false;
	bool m_dropOpenFiles = false;
	std::map<uint32_t, OpenFile> m_open; // worker thread only

	// Target streams.
	std::mutex m_streamMutex;
	std::map<uint32_t, RemoteStream*> m_streams;
	std::atomic<uint32_t> m_nextStreamId{1};
};

} // namespace fileclip

#endif // _WIN32
