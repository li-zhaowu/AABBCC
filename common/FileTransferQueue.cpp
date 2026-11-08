// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "FileTransferQueue.h"
#include "FileManServer.h" // string and time helpers

#include <algorithm>

namespace fileman {

namespace {

constexpr int kMaxDepth = 64;

bool IsDots(const wchar_t* name)
{
	return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

// "name.ext" -> "name (n).ext"
std::string NumberedName(const std::string& name, int n)
{
	const size_t dot = name.rfind('.');
	const bool hasExt = dot != std::string::npos && dot > 0;
	const std::string base = hasExt ? name.substr(0, dot) : name;
	const std::string ext = hasExt ? name.substr(dot) : std::string();
	return base + " (" + std::to_string(n) + ")" + ext;
}

} // namespace

std::wstring LocalPath(const std::string& utf8)
{
	const std::wstring w = Utf8ToWide(utf8);
	if (w.size() < MAX_PATH - 12)
		return w;
	return LongPath(utf8);
}

struct FileTransferQueue::Item {
	TransferItem info;            // guarded by the queue mutex
	std::atomic<bool> cancel{false};
};

FileTransferQueue::FileTransferQueue(FileManClient& client, QueueCallbacks cb) : m_client(client), m_cb(std::move(cb))
{
	m_worker = std::thread(&FileTransferQueue::WorkerMain, this);
}

FileTransferQueue::~FileTransferQueue()
{
	Shutdown();
}

void FileTransferQueue::Shutdown()
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_stop = true;
		for (auto& item : m_items)
			item->cancel = true;
	}
	m_cond.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}

void FileTransferQueue::Notify()
{
	if (m_cb.changed)
		m_cb.changed();
}

uint32_t FileTransferQueue::Add(Direction dir, const std::string& source, bool directory, uint64_t size,
                                const std::string& targetDir)
{
	auto item = std::make_shared<Item>();
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		item->info.id = m_nextId++;
		item->info.direction = dir;
		item->info.name = FileNamePart(source);
		item->info.source = source;
		item->info.targetDir = targetDir;
		item->info.directory = directory;
		item->info.totalBytes = directory ? 0 : size;
		item->info.files = directory ? 0 : 1;
		m_items.push_back(item);
	}
	m_cond.notify_all();
	Notify();
	return item->info.id;
}

void FileTransferQueue::Cancel(uint32_t id)
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		for (auto& item : m_items) {
			if (item->info.id != id)
				continue;
			item->cancel = true;
			if (item->info.state == ItemState::Queued)
				item->info.state = ItemState::Cancelled;
		}
	}
	Notify();
}

void FileTransferQueue::CancelAll()
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		for (auto& item : m_items) {
			item->cancel = true;
			if (item->info.state == ItemState::Queued)
				item->info.state = ItemState::Cancelled;
		}
	}
	Notify();
}

void FileTransferQueue::ClearFinished()
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [](const std::shared_ptr<Item>& i) {
			return i->info.state == ItemState::Done || i->info.state == ItemState::Failed ||
				i->info.state == ItemState::Cancelled;
		}), m_items.end());
	}
	Notify();
}

size_t FileTransferQueue::Retry(bool interruptedOnly)
{
	size_t n = 0;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		for (auto& item : m_items) {
			TransferItem& t = item->info;
			if (t.state != ItemState::Failed || (interruptedOnly && !t.interrupted))
				continue;
			const TransferItem fresh = [&] {
				TransferItem f;
				f.id = t.id;
				f.direction = t.direction;
				f.name = t.name;
				f.source = t.source;
				f.targetDir = t.targetDir;
				f.directory = t.directory;
				f.totalBytes = t.directory ? 0 : t.totalBytes;
				f.files = t.directory ? 0 : 1;
				return f;
			}();
			t = fresh;
			item->cancel = false;
			++n;
		}
	}
	if (n) {
		m_cond.notify_all();
		Notify();
	}
	return n;
}

std::vector<TransferItem> FileTransferQueue::Snapshot() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	std::vector<TransferItem> out;
	out.reserve(m_items.size());
	for (const auto& item : m_items)
		out.push_back(item->info);
	return out;
}

bool FileTransferQueue::Busy() const
{
	std::lock_guard<std::mutex> lk(m_mutex);
	for (const auto& item : m_items)
		if (item->info.state == ItemState::Queued || item->info.state == ItemState::Running)
			return true;
	return false;
}

void FileTransferQueue::WorkerMain()
{
	for (;;) {
		std::shared_ptr<Item> next;
		{
			std::unique_lock<std::mutex> lk(m_mutex);
			m_cond.wait(lk, [&] {
				if (m_stop)
					return true;
				for (auto& item : m_items)
					if (item->info.state == ItemState::Queued)
						return true;
				return false;
			});
			if (m_stop)
				return;
			for (auto& item : m_items) {
				if (item->info.state == ItemState::Queued) {
					next = item;
					break;
				}
			}
			next->info.state = ItemState::Running;
			next->info.startedMs = GetTickCount64();
		}
		Notify();
		Run(next);
		TransferItem done;
		bool idle = true;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			next->info.finishedMs = GetTickCount64();
			next->info.currentFile.clear();
			done = next->info;
			for (auto& item : m_items)
				if (item->info.state == ItemState::Queued)
					idle = false;
			if (idle)
				m_haveDefaultChoice = false; // "apply to all" ends with the batch
		}
		if (m_cb.finished)
			m_cb.finished(done);
		Notify();
	}
}

void FileTransferQueue::Run(const std::shared_ptr<Item>& item)
{
	std::vector<FileTask> files;
	std::string error;
	auto setState = [&](ItemState s, const std::string& err) {
		std::lock_guard<std::mutex> lk(m_mutex);
		item->info.state = s;
		if (!err.empty())
			item->info.lastError = err;
	};
	auto markInterrupted = [&] {
		std::lock_guard<std::mutex> lk(m_mutex);
		item->info.interrupted = !m_client.Connected();
	};
	if (!Expand(item, files, error)) {
		setState(item->cancel ? ItemState::Cancelled : ItemState::Failed, item->cancel ? std::string() : error);
		if (!item->cancel)
			markInterrupted();
		return;
	}
	uint64_t total = 0;
	for (const auto& f : files)
		total += f.size;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		item->info.files = static_cast<uint32_t>(files.size());
		item->info.totalBytes = total;
	}
	Notify();

	const Direction dir = item->info.direction;
	uint64_t completed = 0;
	for (const FileTask& task : files) {
		if (item->cancel)
			break;
		std::string target = task.target;
		bool overwrite = false;
		Entry existing;
		const bool exists = TargetExists(dir, target, existing);
		// Already there, same size and time (e.g. sent before an interrupted
		// attempt): nothing to do, no question.
		if (exists && !existing.IsDirectory() && existing.size == task.size && task.mtime &&
		    existing.mtime == task.mtime) {
			std::lock_guard<std::mutex> lk(m_mutex);
			item->info.filesIdentical++;
			item->info.doneBytes = (completed += task.size);
			continue;
		}
		if (exists) {
			ConflictChoice choice = ConflictChoice::Overwrite;
			bool haveChoice = false;
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				if (m_haveDefaultChoice) {
					choice = m_defaultChoice;
					haveChoice = true;
				}
			}
			if (existing.IsDirectory()) {
				choice = ConflictChoice::KeepBoth; // never replace a folder with a file
				haveChoice = true;
			}
			if (!haveChoice && m_cb.askConflict) {
				Conflict c;
				c.direction = dir;
				c.targetPath = target;
				c.sourceSize = task.size;
				c.sourceTime = task.mtime;
				c.targetSize = existing.size;
				c.targetTime = existing.mtime;
				bool all = false;
				choice = m_cb.askConflict(c, all);
				if (all && choice != ConflictChoice::Cancel) {
					std::lock_guard<std::mutex> lk(m_mutex);
					m_haveDefaultChoice = true;
					m_defaultChoice = choice;
				}
			}
			if (choice == ConflictChoice::Cancel) {
				item->cancel = true;
				break;
			}
			if (choice == ConflictChoice::Skip) {
				std::lock_guard<std::mutex> lk(m_mutex);
				item->info.filesSkipped++;
				item->info.doneBytes = (completed += task.size);
				continue;
			}
			if (choice == ConflictChoice::KeepBoth)
				target = FreeName(dir, target);
			else
				overwrite = true;
		}
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			item->info.currentFile = FileNamePart(target);
		}
		Notify();
		const uint64_t base = completed;
		auto progress = [&](uint64_t done) {
			{
				std::lock_guard<std::mutex> lk(m_mutex);
				item->info.doneBytes = base + done;
			}
			Notify();
		};
		const Result r = dir == Direction::Upload
			? m_client.Upload(LocalPath(task.source), target, overwrite, progress, item->cancel)
			: m_client.Download(task.source, LocalPath(target), overwrite, progress, item->cancel);
		completed += task.size;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			item->info.doneBytes = completed;
			if (r.Ok()) {
				item->info.filesDone++;
			} else if (r.status != kErrCancelled) {
				item->info.errors++;
				item->info.lastError = FileNamePart(target) + ": " + r.Describe();
			}
		}
		if (r.status == kErrDisconnected || r.status == kErrTimeout) {
			setState(ItemState::Failed, r.Describe());
			std::lock_guard<std::mutex> lk(m_mutex);
			item->info.interrupted = true;
			return;
		}
	}
	if (item->cancel)
		setState(ItemState::Cancelled, std::string());
	else
		setState(item->info.errors ? ItemState::Failed : ItemState::Done, std::string());
	if (!item->cancel && item->info.errors)
		markInterrupted();
}

bool FileTransferQueue::Expand(const std::shared_ptr<Item>& item, std::vector<FileTask>& files, std::string& error)
{
	const std::string src = item->info.source;
	const std::string dst = JoinPath(item->info.targetDir, FileNamePart(src));
	if (!item->info.directory) {
		FileTask t;
		t.source = src;
		t.target = dst;
		t.size = item->info.totalBytes;
		if (item->info.direction == Direction::Upload) {
			WIN32_FILE_ATTRIBUTE_DATA a;
			if (!GetFileAttributesExW(LocalPath(src).c_str(), GetFileExInfoStandard, &a)) {
				error = Win32Message(GetLastError());
				return false;
			}
			t.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
			t.mtime = FileTimeToU64(a.ftLastWriteTime);
		} else {
			Entry e;
			const Result r = m_client.StatSync(src, e);
			if (!r.Ok()) {
				error = r.Describe();
				return false;
			}
			t.size = e.size;
			t.mtime = e.mtime;
		}
		files.push_back(t);
		return true;
	}
	return item->info.direction == Direction::Upload ? ExpandLocal(item, src, dst, files, error, 0)
	                                                 : ExpandRemote(item, src, dst, files, error, 0);
}

// Upload of a folder: create it (and its subfolders) on the server, list the
// files. Junctions / symbolic links to folders are not followed.
bool FileTransferQueue::ExpandLocal(const std::shared_ptr<Item>& item, const std::string& src, const std::string& dst,
                                    std::vector<FileTask>& files, std::string& error, int depth)
{
	if (item->cancel || depth > kMaxDepth) {
		error = item->cancel ? "cancelled" : "folders nested too deeply";
		return false;
	}
	const Result made = m_client.MkDirSync(dst);
	if (!made.Ok() && made.status != kErrExists) {
		error = FileNamePart(dst) + ": " + made.Describe();
		return false;
	}
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileExW((LocalPath(src) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
		FIND_FIRST_EX_LARGE_FETCH);
	if (find == INVALID_HANDLE_VALUE) {
		const DWORD e = GetLastError();
		if (e == ERROR_FILE_NOT_FOUND)
			return true;
		error = FileNamePart(src) + ": " + Win32Message(e);
		return false;
	}
	bool ok = true;
	do {
		if (IsDots(fd.cFileName))
			continue;
		const std::string name = WideToUtf8(fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				continue;
			ok = ExpandLocal(item, JoinPath(src, name), JoinPath(dst, name), files, error, depth + 1);
		} else {
			FileTask t;
			t.source = JoinPath(src, name);
			t.target = JoinPath(dst, name);
			t.size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
			t.mtime = FileTimeToU64(fd.ftLastWriteTime);
			files.push_back(t);
		}
	} while (ok && FindNextFileW(find, &fd));
	FindClose(find);
	return ok;
}

// Download of a folder: create it locally, list it on the server.
bool FileTransferQueue::ExpandRemote(const std::shared_ptr<Item>& item, const std::string& src, const std::string& dst,
                                     std::vector<FileTask>& files, std::string& error, int depth)
{
	if (item->cancel || depth > kMaxDepth) {
		error = item->cancel ? "cancelled" : "folders nested too deeply";
		return false;
	}
	if (!CreateDirectoryW(LocalPath(dst).c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		error = FileNamePart(dst) + ": " + Win32Message(GetLastError());
		return false;
	}
	std::vector<Entry> entries;
	const Result r = m_client.ListSync(src, entries);
	if (!r.Ok()) {
		error = FileNamePart(src) + ": " + r.Describe();
		return false;
	}
	for (const Entry& e : entries) {
		// The server sends names; refuse anything that could leave the
		// target folder.
		if (!IsValidName(e.name))
			continue;
		if (e.IsDirectory()) {
			if (!ExpandRemote(item, JoinPath(src, e.name), JoinPath(dst, e.name), files, error, depth + 1))
				return false;
		} else {
			FileTask t;
			t.source = JoinPath(src, e.name);
			t.target = JoinPath(dst, e.name);
			t.size = e.size;
			t.mtime = e.mtime;
			files.push_back(t);
		}
	}
	return true;
}

bool FileTransferQueue::TargetExists(Direction dir, const std::string& path, Entry& info)
{
	if (dir == Direction::Upload) {
		const Result r = m_client.StatSync(path, info);
		return r.Ok();
	}
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(LocalPath(path).c_str(), GetFileExInfoStandard, &a))
		return false;
	info.attributes = AttributesToWire(a.dwFileAttributes);
	info.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
	info.mtime = FileTimeToU64(a.ftLastWriteTime);
	return true;
}

std::string FileTransferQueue::FreeName(Direction dir, const std::string& path)
{
	const std::string parent = ParentPath(path);
	const std::string name = FileNamePart(path);
	for (int n = 2; n < 10000; ++n) {
		const std::string candidate = JoinPath(parent, NumberedName(name, n));
		Entry e;
		if (!TargetExists(dir, candidate, e))
			return candidate;
	}
	return path;
}

} // namespace fileman
