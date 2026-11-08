// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// File manager, viewer side (docs/design.md section 6): requests to the
// server on extension channel 5 (common/FileManProto.h), and whole-file
// downloads and uploads with a window of data in flight.
//
// Threads: OnMessage() runs on the transport's reader thread and only
// queues or hands over data; asynchronous callbacks run there too, so they
// must be quick (the window posts them to its UI thread). The *Sync calls
// and Download/Upload block and are meant for worker threads.

#pragma once

#ifdef _WIN32

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ExtChannel.h"
#include "FileManProto.h"

namespace fileman {

struct ClientCallbacks {
	std::function<bool(extmsg::ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload)> send;
	std::function<void(int level, const std::string& msg)> log;
};

// Result of a request: status (kOk or an error) and the server's message.
struct Result {
	uint32_t status = kOk;
	std::string message;
	bool Ok() const { return status == kOk; }
	// "access denied: <system message>"
	std::string Describe() const;
};

// Optional: the text for a status in the user's language (UTF-8); used by
// Result::Describe. Set once by the viewer.
void SetStatusText(std::string (*text)(uint32_t status));

class FileManClient;

// Reads one remote file from start to end, with read-ahead (files dragged
// to Explorer are streamed through this). Not thread safe; one thread uses it.
class RemoteReader {
public:
	~RemoteReader();
	uint64_t Size() const { return m_size; }
	uint64_t MTime() const { return m_mtime; }
	uint64_t Position() const { return m_pos; }
	// Up to n bytes; got = 0 at the end of the file. Blocks for the network.
	Result Read(void* buf, size_t n, size_t& got);
	// Stop reading and close the remote file (also done by the destructor).
	void Close();

private:
	friend class FileManClient;
	struct State;
	explicit RemoteReader(FileManClient& client) : m_client(&client) {}
	bool IssueReads();
	FileManClient* m_client;
	std::shared_ptr<State> m_state;
	uint32_t m_handle = 0;
	uint64_t m_size = 0, m_mtime = 0;
	uint64_t m_pos = 0;        // bytes handed out
	uint64_t m_requested = 0;  // bytes asked for
	uint64_t m_received = 0;   // bytes arrived
	bool m_eof = false;
	bool m_closed = false;
	std::vector<uint32_t> m_readIds;
};

class FileManClient {
public:
	explicit FileManClient(ClientCallbacks cb);
	~FileManClient();

	FileManClient(const FileManClient&) = delete;
	FileManClient& operator=(const FileManClient&) = delete;

	// Connection state. Disconnect fails everything pending.
	void OnConnected();
	void OnDisconnected();
	bool Connected() const { return m_connected.load(); }

	// A complete message on channel kChFiles (reader thread).
	void OnMessage(uint8_t subtype, std::vector<uint8_t>& payload);

	// Asynchronous requests; `done` runs on the reader thread (or on the
	// caller's thread if the request cannot be sent).
	void Roots(std::function<void(const Result&, std::vector<Root>&)> done);
	void List(const std::string& path, std::function<void(const Result&, std::vector<Entry>&)> done);
	void MkDir(const std::string& path, std::function<void(const Result&)> done);
	void Rename(const std::string& from, const std::string& to, std::function<void(const Result&)> done);
	void Delete(const std::string& path, std::function<void(const Result&)> done);

	// Blocking versions (worker threads).
	Result ListSync(const std::string& path, std::vector<Entry>& out);
	Result StatSync(const std::string& path, Entry& out);
	Result MkDirSync(const std::string& path);
	Result DeleteSync(const std::string& path);

	// Whole-file transfers (worker threads). `progress` gets the bytes done
	// so far; `cancel` is polled. Download writes to a temporary file next
	// to localPath and renames it into place (replacing an existing file
	// only with overwrite). Upload uses OpenWrite / Write / CloseWrite.
	using Progress = std::function<void(uint64_t done)>;
	Result Download(const std::string& remotePath, const std::wstring& localPath, bool overwrite,
	                const Progress& progress, const std::atomic<bool>& cancel);
	Result Upload(const std::wstring& localPath, const std::string& remotePath, bool overwrite,
	              const Progress& progress, const std::atomic<bool>& cancel);

	// Open a remote file for sequential reading (null on failure).
	std::unique_ptr<RemoteReader> OpenReader(const std::string& remotePath, Result& result);

	// Tuning (tests use smaller values).
	uint32_t readChunk = 1024 * 1024;     // bytes per Read request
	uint32_t readWindow = 4;              // Read requests in flight
	uint64_t writeWindow = 4 * 1024 * 1024; // unacknowledged upload bytes
	std::chrono::milliseconds stallTimeout{ 60000 };

private:
	friend class RemoteReader;
	using ReplyFn = std::function<void(const ReplyHeader&, const uint8_t* body, size_t len)>;
	struct DataSink;
	struct AckSink;

	uint32_t NewRequest(ReplyFn fn);
	void DropRequest(uint32_t reqId);
	bool SendRequest(uint8_t subtype, uint32_t reqId, const std::vector<uint8_t>& payload);
	Result Wait(std::function<void(std::function<void(const Result&)>)> start,
	            std::chrono::milliseconds timeout);
	void Log(int level, const std::string& msg);

	ClientCallbacks m_cb;
	std::atomic<bool> m_connected{false};
	std::mutex m_mutex;
	uint32_t m_nextReqId = 1;
	std::map<uint32_t, ReplyFn> m_pending;
	std::map<uint32_t, std::shared_ptr<DataSink>> m_dataSinks; // by handle
	std::map<uint32_t, std::shared_ptr<AckSink>> m_ackSinks;   // by handle
	uint32_t m_tempCounter = 0;
};

} // namespace fileman

#endif // _WIN32
