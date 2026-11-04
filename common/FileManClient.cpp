// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "FileManClient.h"
#include "FileManServer.h" // string and time helpers

#include <algorithm>
#include <deque>

using namespace extmsg;
using Clock = std::chrono::steady_clock;

namespace fileman {

std::string Result::Describe() const
{
	if (status == kOk)
		return "OK";
	std::string s = StatusName(status);
	if (!message.empty())
		s += ": " + message;
	return s;
}

struct FileManClient::DataSink {
	std::mutex mutex;
	std::condition_variable cond;
	struct Piece {
		uint64_t offset;
		std::vector<uint8_t> bytes;
		bool eof;
	};
	std::deque<Piece> pieces;
	Result error;
};

struct FileManClient::AckSink {
	std::mutex mutex;
	std::condition_variable cond;
	uint64_t acked = 0;
	Result error;
};

namespace {

// Long form for local paths that need it.
std::wstring LocalLong(const std::wstring& p)
{
	if (p.size() < MAX_PATH - 12 || p.compare(0, 4, L"\\\\?\\") == 0)
		return p;
	if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
		return L"\\\\?\\UNC\\" + p.substr(2);
	return L"\\\\?\\" + p;
}

std::wstring DirOf(const std::wstring& p)
{
	const size_t i = p.find_last_of(L'\\');
	return i == std::wstring::npos ? std::wstring() : p.substr(0, i + 1);
}

Result Win32Result(DWORD e)
{
	Result r;
	r.status = FileManServer::StatusFromWin32(e);
	r.message = Win32Message(e);
	return r;
}

Result MakeResult(uint32_t status, const std::string& message = std::string())
{
	Result r;
	r.status = status;
	r.message = message;
	return r;
}

} // namespace

FileManClient::FileManClient(ClientCallbacks cb) : m_cb(std::move(cb)) {}

FileManClient::~FileManClient()
{
	OnDisconnected();
}

void FileManClient::Log(int level, const std::string& msg)
{
	if (m_cb.log)
		m_cb.log(level, "file manager: " + msg);
}

void FileManClient::OnConnected()
{
	m_connected = true;
}

void FileManClient::OnDisconnected()
{
	m_connected = false;
	std::map<uint32_t, ReplyFn> pending;
	std::map<uint32_t, std::shared_ptr<DataSink>> data;
	std::map<uint32_t, std::shared_ptr<AckSink>> acks;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		pending.swap(m_pending);
		data.swap(m_dataSinks);
		acks.swap(m_ackSinks);
	}
	ReplyHeader h;
	h.status = kErrDisconnected;
	for (auto& kv : pending) {
		h.reqId = kv.first;
		kv.second(h, nullptr, 0);
	}
	for (auto& kv : data) {
		std::lock_guard<std::mutex> lk(kv.second->mutex);
		kv.second->error = MakeResult(kErrDisconnected);
		kv.second->cond.notify_all();
	}
	for (auto& kv : acks) {
		std::lock_guard<std::mutex> lk(kv.second->mutex);
		kv.second->error = MakeResult(kErrDisconnected);
		kv.second->cond.notify_all();
	}
}

uint32_t FileManClient::NewRequest(ReplyFn fn)
{
	std::lock_guard<std::mutex> lk(m_mutex);
	uint32_t id = m_nextReqId++;
	if (id == 0)
		id = m_nextReqId++;
	m_pending[id] = std::move(fn);
	return id;
}

void FileManClient::DropRequest(uint32_t reqId)
{
	std::lock_guard<std::mutex> lk(m_mutex);
	m_pending.erase(reqId);
}

// Sends; on failure answers the request locally with "disconnected".
bool FileManClient::SendRequest(uint8_t subtype, uint32_t reqId, const std::vector<uint8_t>& payload)
{
	if (m_connected && m_cb.send(kPrioControl, subtype, payload))
		return true;
	ReplyFn fn;
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		auto it = m_pending.find(reqId);
		if (it == m_pending.end())
			return false;
		fn = std::move(it->second);
		m_pending.erase(it);
	}
	ReplyHeader h;
	h.reqId = reqId;
	h.status = kErrDisconnected;
	fn(h, nullptr, 0);
	return false;
}

void FileManClient::OnMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	const uint8_t* p = payload.data();
	const size_t n = payload.size();
	switch (subtype) {
	case kReply: {
		ReplyHeader h;
		size_t off = 0;
		if (!DecodeReplyHeader(p, n, h, off))
			return;
		ReplyFn fn;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			auto it = m_pending.find(h.reqId);
			if (it == m_pending.end())
				return; // timed out or cancelled
			if (h.flags & kReplyMore) {
				fn = it->second; // more pages follow: keep it registered
			} else {
				fn = std::move(it->second);
				m_pending.erase(it);
			}
		}
		fn(h, p + off, n - off);
		break;
	}
	case kData: {
		DataHeader h;
		if (!DecodeDataHeader(p, n, h))
			return;
		std::shared_ptr<DataSink> sink;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			auto it = m_dataSinks.find(h.handle);
			if (it == m_dataSinks.end())
				return; // cancelled download
			sink = it->second;
		}
		std::lock_guard<std::mutex> lk(sink->mutex);
		sink->pieces.push_back(DataSink::Piece{ h.offset,
			std::vector<uint8_t>(p + kDataHeaderSize, p + n), (h.flags & kDataEof) != 0 });
		sink->cond.notify_all();
		break;
	}
	case kWriteAck: {
		WriteAck a;
		if (!DecodeWriteAck(p, n, a))
			return;
		std::shared_ptr<AckSink> sink;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			auto it = m_ackSinks.find(a.handle);
			if (it == m_ackSinks.end())
				return;
			sink = it->second;
		}
		std::lock_guard<std::mutex> lk(sink->mutex);
		if (a.status != kOk)
			sink->error = MakeResult(a.status);
		else
			sink->acked = (std::max)(sink->acked, a.written);
		sink->cond.notify_all();
		break;
	}
	default:
		break;
	}
}

// --- asynchronous requests ---------------------------------------------------

void FileManClient::Roots(std::function<void(const Result&, std::vector<Root>&)> done)
{
	const uint32_t id = NewRequest([done](const ReplyHeader& h, const uint8_t* body, size_t len) {
		std::vector<Root> roots;
		Result r = MakeResult(h.status, h.message);
		if (r.Ok() && !DecodeRoots(body, len, roots))
			r = MakeResult(kErrBadRequest, "malformed reply");
		done(r, roots);
	});
	SendRequest(kRoots, id, EncodeRootsRequest(id));
}

void FileManClient::List(const std::string& path, std::function<void(const Result&, std::vector<Entry>&)> done)
{
	auto all = std::make_shared<std::vector<Entry>>();
	const uint32_t id = NewRequest([done, all](const ReplyHeader& h, const uint8_t* body, size_t len) {
		Result r = MakeResult(h.status, h.message);
		if (r.Ok()) {
			std::vector<Entry> page;
			if (!DecodeEntries(body, len, page))
				r = MakeResult(kErrBadRequest, "malformed reply");
			else
				all->insert(all->end(), std::make_move_iterator(page.begin()), std::make_move_iterator(page.end()));
		}
		if (r.Ok() && (h.flags & kReplyMore))
			return; // wait for the rest
		if (!r.Ok())
			all->clear();
		done(r, *all);
	});
	SendRequest(kList, id, EncodePathRequest(id, path));
}

namespace {
std::function<void(const ReplyHeader&, const uint8_t*, size_t)> StatusOnly(std::function<void(const Result&)> done)
{
	return [done](const ReplyHeader& h, const uint8_t*, size_t) { done(MakeResult(h.status, h.message)); };
}
} // namespace

void FileManClient::MkDir(const std::string& path, std::function<void(const Result&)> done)
{
	const uint32_t id = NewRequest(StatusOnly(std::move(done)));
	SendRequest(kMkDir, id, EncodePathRequest(id, path));
}

void FileManClient::Rename(const std::string& from, const std::string& to, std::function<void(const Result&)> done)
{
	const uint32_t id = NewRequest(StatusOnly(std::move(done)));
	RenameRequest r;
	r.reqId = id;
	r.from = from;
	r.to = to;
	SendRequest(kRename, id, EncodeRename(r));
}

void FileManClient::Delete(const std::string& path, std::function<void(const Result&)> done)
{
	const uint32_t id = NewRequest(StatusOnly(std::move(done)));
	SendRequest(kDelete, id, EncodePathRequest(id, path));
}

// --- blocking requests ---------------------------------------------------------

namespace {
struct Waiter {
	std::mutex mutex;
	std::condition_variable cond;
	bool done = false;
	Result result;
	void Set(const Result& r)
	{
		std::lock_guard<std::mutex> lk(mutex);
		result = r;
		done = true;
		cond.notify_all();
	}
};
} // namespace

Result FileManClient::Wait(std::function<void(std::function<void(const Result&)>)> start, std::chrono::milliseconds timeout)
{
	auto w = std::make_shared<Waiter>();
	start([w](const Result& r) { w->Set(r); });
	std::unique_lock<std::mutex> lk(w->mutex);
	if (!w->cond.wait_for(lk, timeout, [&] { return w->done; }))
		return MakeResult(kErrTimeout);
	return w->result;
}

Result FileManClient::ListSync(const std::string& path, std::vector<Entry>& out)
{
	auto entries = std::make_shared<std::vector<Entry>>();
	Result r = Wait([&](std::function<void(const Result&)> set) {
		List(path, [set, entries](const Result& res, std::vector<Entry>& e) {
			entries->swap(e);
			set(res);
		});
	}, std::chrono::minutes(5));
	out = *entries;
	return r;
}

Result FileManClient::StatSync(const std::string& path, Entry& out)
{
	auto entry = std::make_shared<Entry>();
	Result r = Wait([&](std::function<void(const Result&)> set) {
		const uint32_t id = NewRequest([set, entry](const ReplyHeader& h, const uint8_t* body, size_t len) {
			Result res = MakeResult(h.status, h.message);
			if (res.Ok() && !DecodeStat(body, len, *entry))
				res = MakeResult(kErrBadRequest, "malformed reply");
			set(res);
		});
		SendRequest(kStat, id, EncodePathRequest(id, path));
	}, std::chrono::minutes(1));
	out = *entry;
	out.name = FileNamePart(path);
	return r;
}

Result FileManClient::MkDirSync(const std::string& path)
{
	return Wait([&](std::function<void(const Result&)> set) { MkDir(path, set); }, std::chrono::minutes(1));
}

Result FileManClient::DeleteSync(const std::string& path)
{
	return Wait([&](std::function<void(const Result&)> set) { Delete(path, set); }, std::chrono::hours(1));
}

// --- download ------------------------------------------------------------------

Result FileManClient::Download(const std::string& remotePath, const std::wstring& localPathIn, bool overwrite,
                               const Progress& progress, const std::atomic<bool>& cancel)
{
	// Open on the server.
	auto opened = std::make_shared<OpenReadReply>();
	Result r = Wait([&](std::function<void(const Result&)> set) {
		const uint32_t id = NewRequest([set, opened](const ReplyHeader& h, const uint8_t* body, size_t len) {
			Result res = MakeResult(h.status, h.message);
			if (res.Ok() && !DecodeOpenReadReply(body, len, *opened))
				res = MakeResult(kErrBadRequest, "malformed reply");
			set(res);
		});
		SendRequest(kOpenRead, id, EncodePathRequest(id, remotePath));
	}, std::chrono::minutes(1));
	if (!r.Ok())
		return r;
	const uint32_t handle = opened->handle;
	uint64_t size = opened->size;

	auto sink = std::make_shared<DataSink>();
	std::vector<uint32_t> readIds; // replies to reads only come on error
	auto onReadError = [sink](const ReplyHeader& h, const uint8_t*, size_t) {
		std::lock_guard<std::mutex> lk(sink->mutex);
		if (sink->error.Ok())
			sink->error = MakeResult(h.status == kOk ? kErrIo : h.status, h.message);
		sink->cond.notify_all();
	};
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_dataSinks[handle] = sink;
	}
	auto closeRemote = [&] {
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_dataSinks.erase(handle);
			for (uint32_t id : readIds)
				m_pending.erase(id);
		}
		const uint32_t id = NewRequest([](const ReplyHeader&, const uint8_t*, size_t) {});
		SendRequest(kClose, id, EncodeHandleRequest(id, handle));
	};

	// Local temporary file next to the target.
	const std::wstring localPath = LocalLong(localPathIn);
	const std::wstring temp = DirOf(localPath) + L"~uvnc-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
		std::to_wstring(++m_tempCounter) + L".part";
	HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		r = Win32Result(GetLastError());
		closeRemote();
		return r;
	}

	uint64_t requested = 0, received = 0;
	auto lastProgress = Clock::now();
	auto issueReads = [&]() -> bool {
		while (requested < size && requested - received < uint64_t(readChunk) * readWindow) {
			ReadRequest rq;
			rq.handle = handle;
			rq.offset = requested;
			rq.length = static_cast<uint32_t>((std::min<uint64_t>)(readChunk, size - requested));
			rq.reqId = NewRequest(onReadError);
			readIds.push_back(rq.reqId);
			if (!SendRequest(kRead, rq.reqId, EncodeRead(rq)))
				return false;
			requested += rq.length;
		}
		return true;
	};
	bool eof = size == 0;
	if (!issueReads())
		r = MakeResult(kErrDisconnected);
	while (r.Ok() && !eof && received < size) {
		std::deque<DataSink::Piece> pieces;
		{
			std::unique_lock<std::mutex> lk(sink->mutex);
			sink->cond.wait_for(lk, std::chrono::milliseconds(100),
				[&] { return !sink->pieces.empty() || !sink->error.Ok(); });
			if (!sink->error.Ok()) {
				r = sink->error;
				break;
			}
			pieces.swap(sink->pieces);
		}
		if (cancel) {
			r = MakeResult(kErrCancelled);
			break;
		}
		if (pieces.empty()) {
			if (Clock::now() - lastProgress > stallTimeout)
				r = MakeResult(kErrTimeout);
			continue;
		}
		for (auto& piece : pieces) {
			if (piece.offset != received) {
				r = MakeResult(kErrIo, "data out of order");
				break;
			}
			DWORD done = 0;
			if (!piece.bytes.empty() &&
			    (!WriteFile(file, piece.bytes.data(), static_cast<DWORD>(piece.bytes.size()), &done, NULL) ||
			     done != piece.bytes.size())) {
				r = Win32Result(GetLastError());
				break;
			}
			received += piece.bytes.size();
			if (piece.eof) {
				eof = true; // the file got shorter since it was opened
				size = received;
				break;
			}
		}
		lastProgress = Clock::now();
		if (progress)
			progress(received);
		if (r.Ok() && !eof && !issueReads())
			r = MakeResult(kErrDisconnected);
	}
	closeRemote();

	if (r.Ok() && opened->mtime) {
		const FILETIME ft = U64ToFileTime(opened->mtime);
		SetFileTime(file, NULL, NULL, &ft);
	}
	CloseHandle(file);
	if (r.Ok()) {
		if (overwrite) {
			const DWORD attrs = GetFileAttributesW(localPath.c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
				SetFileAttributesW(localPath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
		}
		if (MoveFileExW(temp.c_str(), localPath.c_str(), overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) {
			const DWORD attrs = GetFileAttributesW(localPath.c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES)
				SetFileAttributesW(localPath.c_str(), attrs & ~FILE_ATTRIBUTE_HIDDEN);
			return r;
		}
		r = Win32Result(GetLastError());
	}
	DeleteFileW(temp.c_str());
	return r;
}

// --- upload --------------------------------------------------------------------

Result FileManClient::Upload(const std::wstring& localPathIn, const std::string& remotePath, bool overwrite,
                             const Progress& progress, const std::atomic<bool>& cancel)
{
	const std::wstring localPath = LocalLong(localPathIn);
	HANDLE file = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return Win32Result(GetLastError());
	LARGE_INTEGER li;
	FILETIME mtime = {};
	if (!GetFileSizeEx(file, &li) || !GetFileTime(file, NULL, NULL, &mtime)) {
		const Result err = Win32Result(GetLastError());
		CloseHandle(file);
		return err;
	}
	const uint64_t size = static_cast<uint64_t>(li.QuadPart);

	auto handle = std::make_shared<uint32_t>(0);
	Result r = Wait([&](std::function<void(const Result&)> set) {
		OpenWriteRequest ow;
		ow.path = remotePath;
		ow.size = size;
		ow.flags = overwrite ? kWriteOverwrite : 0;
		ow.reqId = NewRequest([set, handle](const ReplyHeader& h, const uint8_t* body, size_t len) {
			Result res = MakeResult(h.status, h.message);
			if (res.Ok() && !DecodeHandle(body, len, *handle))
				res = MakeResult(kErrBadRequest, "malformed reply");
			set(res);
		});
		SendRequest(kOpenWrite, ow.reqId, EncodeOpenWrite(ow));
	}, std::chrono::minutes(1));
	if (!r.Ok()) {
		CloseHandle(file);
		return r;
	}

	auto sink = std::make_shared<AckSink>();
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_ackSinks[*handle] = sink;
	}
	std::vector<uint8_t> buf(kDataPieceBytes);
	uint64_t sent = 0, acked = 0;
	auto lastProgress = Clock::now();
	while (r.Ok() && acked < size) {
		// Send while the window allows.
		while (r.Ok() && sent < size && sent - acked < writeWindow) {
			const DWORD want = static_cast<DWORD>((std::min<uint64_t>)(kDataPieceBytes, size - sent));
			DWORD got = 0;
			if (!ReadFile(file, buf.data(), want, &got, NULL)) {
				r = Win32Result(GetLastError());
				break;
			}
			if (got != want) {
				r = MakeResult(kErrIo, "the file changed while it was being sent");
				break;
			}
			WriteHeader wh;
			wh.handle = *handle;
			wh.offset = sent;
			if (!m_connected || !m_cb.send(kPrioBulk, kWrite, EncodeWrite(wh, buf.data(), got))) {
				r = MakeResult(kErrDisconnected);
				break;
			}
			sent += got;
		}
		if (!r.Ok())
			break;
		std::unique_lock<std::mutex> lk(sink->mutex);
		sink->cond.wait_for(lk, std::chrono::milliseconds(100),
			[&] { return sink->acked > acked || !sink->error.Ok(); });
		if (!sink->error.Ok()) {
			r = sink->error;
			break;
		}
		if (sink->acked > acked) {
			acked = sink->acked;
			lastProgress = Clock::now();
			lk.unlock();
			if (progress)
				progress(acked);
		} else if (Clock::now() - lastProgress > stallTimeout) {
			r = MakeResult(kErrTimeout);
		}
		if (cancel)
			r = MakeResult(kErrCancelled);
	}
	CloseHandle(file);
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_ackSinks.erase(*handle);
	}
	// All data is acknowledged (or we give up): commit or discard.
	const bool commit = r.Ok();
	if (r.status == kErrDisconnected)
		return r;
	const Result closed = Wait([&](std::function<void(const Result&)> set) {
		CloseWriteRequest cw;
		cw.handle = *handle;
		cw.commit = commit;
		cw.mtime = FileTimeToU64(mtime);
		cw.reqId = NewRequest(StatusOnly(set));
		SendRequest(kCloseWrite, cw.reqId, EncodeCloseWrite(cw));
	}, std::chrono::minutes(1));
	if (commit) {
		if (progress && size == 0)
			progress(0);
		return closed;
	}
	return r;
}

} // namespace fileman
