// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// File manager, server side (docs/design.md section 6): answers the
// viewer's requests on extension channel 5 (common/FileManProto.h).
//
// All file system work happens on one worker thread, in request order, as
// the logged-on desktop user when a token is supplied (service mode).
// Uploads are written to a hidden temporary file next to the target and
// renamed into place on CloseWrite, so an interrupted upload never leaves a
// half-written file under the real name. Recursive delete does not follow
// junctions or symbolic links.

#pragma once

#ifdef _WIN32

#include <winsock2.h>
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ExtChannel.h"
#include "FileManProto.h"

namespace fileman {

struct ServerCallbacks {
	// Queue a message on channel kChFiles. Returns false if not usable.
	std::function<bool(extmsg::ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload)> send;
	// level 0 = error, 1 = info, 2 = debug
	std::function<void(int level, const std::string& msg)> log;
	// Optional, as in fileclip::Callbacks: return false to refuse file
	// access; set *token to a token to impersonate (closed by the engine).
	std::function<bool(HANDLE* token)> acquireUserToken;
	// Told to the viewer when acquireUserToken refuses.
	std::string refusedMessage = "no user is logged on";
};

class FileManServer {
public:
	explicit FileManServer(ServerCallbacks cb);
	~FileManServer();

	FileManServer(const FileManServer&) = delete;
	FileManServer& operator=(const FileManServer&) = delete;

	void Start();
	// Stop the worker, close all handles, delete unfinished uploads.
	void Stop();

	// A complete message on channel kChFiles (transport reader thread).
	void OnMessage(uint8_t subtype, std::vector<uint8_t>& payload);

	// Windows error -> protocol status (exposed for tests).
	static uint32_t StatusFromWin32(DWORD error);

private:
	struct Job {
		uint8_t subtype = 0;
		std::vector<uint8_t> payload;
	};
	struct Open {
		HANDLE handle = INVALID_HANDLE_VALUE;
		bool write = false;
		std::wstring target;     // write: final path (long form)
		std::wstring temp;       // write: temporary file
		uint64_t written = 0;
		uint32_t failed = kOk;   // write: first error
		bool overwrite = false;
	};

	void WorkerMain();
	void Handle(Job& job);
	void Reply(uint32_t reqId, uint32_t status, const std::vector<uint8_t>& body = {},
	           const std::string& message = std::string(), uint8_t flags = 0);
	void ReplyError(uint32_t reqId, DWORD win32Error);

	void DoRoots(uint32_t reqId);
	void DoList(uint32_t reqId, const std::string& path);
	void DoStat(uint32_t reqId, const std::string& path);
	void DoMkDir(uint32_t reqId, const std::string& path);
	void DoRename(const RenameRequest& r);
	void DoDelete(uint32_t reqId, const std::string& path);
	void DoOpenRead(uint32_t reqId, const std::string& path);
	void DoRead(const ReadRequest& r);
	void DoClose(uint32_t reqId, uint32_t handle);
	void DoOpenWrite(const OpenWriteRequest& r);
	void DoWrite(const WriteHeader& h, const uint8_t* bytes, size_t n);
	void DoCloseWrite(const CloseWriteRequest& r);
	void CloseAll();

	void Log(int level, const std::string& msg);

	ServerCallbacks m_cb;
	std::thread m_worker;
	std::mutex m_mutex;
	std::condition_variable m_cond;
	std::deque<Job> m_jobs;
	bool m_stop = false;
	bool m_started = false;

	// Worker thread only.
	std::map<uint32_t, Open> m_open;
	uint32_t m_nextHandle = 1;
	uint32_t m_tempCounter = 0;
};

// Helpers shared with the viewer's local side (Windows only).
// UTF-8 absolute path -> "\\?\" long form for the wide Win32 APIs.
std::wstring LongPath(const std::string& utf8);
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
// System message for a Win32 error, UTF-8, without the trailing newline.
std::string Win32Message(DWORD error);
uint64_t FileTimeToU64(const FILETIME& ft);
FILETIME U64ToFileTime(uint64_t v);
uint32_t AttributesToWire(DWORD attributes);

} // namespace fileman

#endif // _WIN32
