// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Transfer queue of the file transfer window (docs/design.md section 6).
// Each item is a file or a folder (copied with everything inside) going
// one way. Items run one after another on a worker thread; the window
// reads snapshots for its queue list and answers name conflicts.

#pragma once

#ifdef _WIN32

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "FileManClient.h"

namespace fileman {

enum class Direction { Upload, Download };

enum class ItemState { Queued, Running, Done, Failed, Cancelled };

struct TransferItem {
	uint32_t id = 0;
	Direction direction = Direction::Upload;
	std::string name;          // display name (UTF-8)
	std::string source;        // full source path (UTF-8)
	std::string targetDir;     // folder it goes into (UTF-8)
	bool directory = false;
	ItemState state = ItemState::Queued;
	uint64_t totalBytes = 0;   // known once the folder has been scanned
	uint64_t doneBytes = 0;
	uint32_t files = 0;
	uint32_t filesDone = 0;
	uint32_t filesSkipped = 0;   // by the user's choice
	uint32_t filesIdentical = 0; // the target already had the same file
	uint32_t errors = 0;
	std::string currentFile;   // UTF-8, while running
	std::string lastError;     // UTF-8
	uint64_t startedMs = 0;    // GetTickCount64 at start, 0 = not started
	uint64_t finishedMs = 0;
	bool interrupted = false;  // failed because the connection dropped
};

enum class ConflictChoice { Overwrite, Skip, KeepBoth, Cancel };

struct Conflict {
	Direction direction = Direction::Upload;
	std::string targetPath;    // UTF-8
	uint64_t sourceSize = 0, sourceTime = 0;
	uint64_t targetSize = 0, targetTime = 0;
};

struct QueueCallbacks {
	// Something changed (state, progress). Called from the worker thread;
	// keep it cheap (post a message).
	std::function<void()> changed;
	// An item finished (refresh the target folder). Worker thread.
	std::function<void(const TransferItem&)> finished;
	// The target exists: what now? Blocks the worker until answered.
	// applyToAll: use the answer for the remaining conflicts.
	std::function<ConflictChoice(const Conflict&, bool& applyToAll)> askConflict;
};

class FileTransferQueue {
public:
	FileTransferQueue(FileManClient& client, QueueCallbacks cb);
	~FileTransferQueue();

	FileTransferQueue(const FileTransferQueue&) = delete;
	FileTransferQueue& operator=(const FileTransferQueue&) = delete;

	// Add an item: an upload of a local file/folder into a remote folder,
	// or a download of a remote file/folder into a local folder. Paths are
	// absolute, UTF-8. Returns the item id.
	uint32_t Add(Direction dir, const std::string& source, bool directory, uint64_t size,
	             const std::string& targetDir);

	void Cancel(uint32_t id);
	void CancelAll();
	// Remove finished, failed and cancelled items.
	void ClearFinished();
	// Queue failed items again (only those cut off by the connection, or
	// all failed ones). Files already complete at the target are skipped;
	// partial ones continue where they stopped. Returns how many.
	size_t Retry(bool interruptedOnly);

	std::vector<TransferItem> Snapshot() const;
	bool Busy() const;

	// Stop the worker (cancels everything). Called before destruction.
	void Shutdown();

private:
	struct Item;
	struct FileTask {
		std::string source;     // UTF-8 absolute
		std::string target;     // UTF-8 absolute
		uint64_t size = 0;
		uint64_t mtime = 0;
	};

	void WorkerMain();
	void Run(const std::shared_ptr<Item>& item);
	bool Expand(const std::shared_ptr<Item>& item, std::vector<FileTask>& files, std::string& error);
	bool ExpandLocal(const std::shared_ptr<Item>& item, const std::string& src, const std::string& dst,
	                 std::vector<FileTask>& files, std::string& error, int depth);
	bool ExpandRemote(const std::shared_ptr<Item>& item, const std::string& src, const std::string& dst,
	                  std::vector<FileTask>& files, std::string& error, int depth);
	// Target exists? Fills size/time.
	bool TargetExists(Direction dir, const std::string& path, Entry& info);
	std::string FreeName(Direction dir, const std::string& path);
	void Notify();

	FileManClient& m_client;
	QueueCallbacks m_cb;
	mutable std::mutex m_mutex;
	std::condition_variable m_cond;
	std::vector<std::shared_ptr<Item>> m_items;
	uint32_t m_nextId = 1;
	bool m_stop = false;
	std::thread m_worker;
	// Conflict answer kept for the rest of the busy period ("apply to all").
	bool m_haveDefaultChoice = false;
	ConflictChoice m_defaultChoice = ConflictChoice::Overwrite;
};

// Local file system helpers used by the window as well.
// UTF-8 absolute path -> wide path (long form when needed).
std::wstring LocalPath(const std::string& utf8);

} // namespace fileman

#endif // _WIN32
