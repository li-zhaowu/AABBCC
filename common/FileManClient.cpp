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
#include <cstring>
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

	// Local temporary file next to the target, named after the target and
	// the remote file's size and time: if an earlier attempt of this very
	// file was interrupted, continue where it stopped.
	const std::wstring localPath = LocalLong(localPathIn);
	const std::wstring temp = DirOf(localPath) + Utf8ToWide(ResumeTempName(WideToUtf8(localPathIn), size, opened->mtime));
	uint64_t resumeAt = 0;
	HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER existing, zero = {};
		if (GetFileSizeEx(file, &existing) && static_cast<uint64_t>(existing.QuadPart) <= size &&
		    SetFilePointerEx(file, zero, NULL, FILE_END)) {
			resumeAt = static_cast<uint64_t>(existing.QuadPart);
		} else {
			CloseHandle(file);
			file = INVALID_HANDLE_VALUE;
		}
	}
	if (file == INVALID_HANDLE_VALUE) {
		PurgeOldPartFiles(DirOf(localPath));
		file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	}
	if (file == INVALID_HANDLE_VALUE) {
		r = Win32Result(GetLastError());
		closeRemote();
		return r;
	}
	if (resumeAt) {
		Log(1, "continuing " + remotePath + " at byte " + std::to_string(resumeAt));
		if (progress)
			progress(resumeAt);
	}

	uint64_t requested = resumeAt, received = resumeAt;
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
	bool eof = received >= size;
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
	// Interrupted by the connection: keep what we have for the next try.
	// Anything else (cancel, error): throw it away.
	if (r.status != kErrDisconnected && r.status != kErrTimeout)
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

	// Resumable: if an earlier upload of this very file (same size and
	// time) was cut off, the server tells us how much it already has.
	auto opened = std::make_shared<OpenWriteReply>();
	Result r = Wait([&](std::function<void(const Result&)> set) {
		OpenWriteRequest ow;
		ow.path = remotePath;
		ow.size = size;
		ow.mtime = FileTimeToU64(mtime);
		ow.flags = static_cast<uint8_t>((overwrite ? kWriteOverwrite : 0) | kWriteResume);
		ow.reqId = NewRequest([set, opened](const ReplyHeader& h, const uint8_t* body, size_t len) {
			Result res = MakeResult(h.status, h.message);
			if (res.Ok() && (!DecodeOpenWriteReply(body, len, *opened) || opened->offset > 0xFFFFFFFFFFFFULL))
				res = MakeResult(kErrBadRequest, "malformed reply");
			set(res);
		});
		SendRequest(kOpenWrite, ow.reqId, EncodeOpenWrite(ow));
	}, std::chrono::minutes(1));
	if (!r.Ok()) {
		CloseHandle(file);
		return r;
	}
	auto handle = std::make_shared<uint32_t>(opened->handle);
	uint64_t sent = 0, acked = 0;
	if (opened->offset > 0 && opened->offset <= size) {
		LARGE_INTEGER pos;
		pos.QuadPart = static_cast<LONGLONG>(opened->offset);
		if (SetFilePointerEx(file, pos, NULL, FILE_BEGIN)) {
			sent = acked = opened->offset;
			Log(1, "continuing " + remotePath + " at byte " + std::to_string(sent));
			if (progress)
				progress(acked);
		}
	}

	auto sink = std::make_shared<AckSink>();
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_ackSinks[*handle] = sink;
	}
	std::vector<uint8_t> buf(kDataPieceBytes);
	{
		std::lock_guard<std::mutex> lk(sink->mutex);
		sink->acked = acked;
	}
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

// --- sequential reader -----------------------------------------------------------

struct RemoteReader::State {
	std::shared_ptr<FileManClient::DataSink> sink;
	std::deque<FileManClient::DataSink::Piece> buffer;
	size_t frontUsed = 0; // bytes of buffer.front() already handed out
	Clock::time_point lastProgress;
};

std::unique_ptr<RemoteReader> FileManClient::OpenReader(const std::string& remotePath, Result& result)
{
	auto opened = std::make_shared<OpenReadReply>();
	result = Wait([&](std::function<void(const Result&)> set) {
		const uint32_t id = NewRequest([set, opened](const ReplyHeader& h, const uint8_t* body, size_t len) {
			Result res = MakeResult(h.status, h.message);
			if (res.Ok() && !DecodeOpenReadReply(body, len, *opened))
				res = MakeResult(kErrBadRequest, "malformed reply");
			set(res);
		});
		SendRequest(kOpenRead, id, EncodePathRequest(id, remotePath));
	}, std::chrono::minutes(1));
	if (!result.Ok())
		return nullptr;
	std::unique_ptr<RemoteReader> reader(new RemoteReader(*this));
	reader->m_handle = opened->handle;
	reader->m_size = opened->size;
	reader->m_mtime = opened->mtime;
	reader->m_eof = opened->size == 0;
	reader->m_state = std::make_shared<RemoteReader::State>();
	reader->m_state->sink = std::make_shared<DataSink>();
	reader->m_state->lastProgress = Clock::now();
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_dataSinks[reader->m_handle] = reader->m_state->sink;
	}
	return reader;
}

RemoteReader::~RemoteReader()
{
	Close();
}

void RemoteReader::Close()
{
	if (m_closed || !m_state)
		return;
	m_closed = true;
	FileManClient& c = *m_client;
	{
		std::lock_guard<std::mutex> lk(c.m_mutex);
		c.m_dataSinks.erase(m_handle);
		for (uint32_t id : m_readIds)
			c.m_pending.erase(id);
	}
	const uint32_t id = c.NewRequest([](const ReplyHeader&, const uint8_t*, size_t) {});
	c.SendRequest(kClose, id, EncodeHandleRequest(id, m_handle));
}

bool RemoteReader::IssueReads()
{
	FileManClient& c = *m_client;
	auto sink = m_state->sink;
	while (m_requested < m_size && m_requested - m_pos < uint64_t(c.readChunk) * c.readWindow) {
		ReadRequest rq;
		rq.handle = m_handle;
		rq.offset = m_requested;
		rq.length = static_cast<uint32_t>((std::min<uint64_t>)(c.readChunk, m_size - m_requested));
		rq.reqId = c.NewRequest([sink](const ReplyHeader& h, const uint8_t*, size_t) {
			std::lock_guard<std::mutex> lk(sink->mutex);
			if (sink->error.Ok())
				sink->error = MakeResult(h.status == kOk ? kErrIo : h.status, h.message);
			sink->cond.notify_all();
		});
		m_readIds.push_back(rq.reqId);
		if (!c.SendRequest(kRead, rq.reqId, EncodeRead(rq)))
			return false;
		m_requested += rq.length;
	}
	return true;
}

Result RemoteReader::Read(void* buf, size_t n, size_t& got)
{
	got = 0;
	if (m_closed)
		return MakeResult(kErrCancelled);
	State& st = *m_state;
	uint8_t* out = static_cast<uint8_t*>(buf);
	while (got < n) {
		// Hand out what has arrived.
		while (got < n && !st.buffer.empty()) {
			auto& front = st.buffer.front();
			const size_t take = (std::min)(n - got, front.bytes.size() - st.frontUsed);
			memcpy(out + got, front.bytes.data() + st.frontUsed, take);
			got += take;
			st.frontUsed += take;
			m_pos += take;
			if (st.frontUsed == front.bytes.size()) {
				st.buffer.pop_front();
				st.frontUsed = 0;
			}
		}
		if (got == n || (st.buffer.empty() && (m_eof || m_received >= m_size)))
			break;
		// Keep the read-ahead going, then wait for more.
		if (!IssueReads())
			return MakeResult(kErrDisconnected);
		std::deque<FileManClient::DataSink::Piece> arrived;
		{
			std::unique_lock<std::mutex> lk(st.sink->mutex);
			st.sink->cond.wait_for(lk, std::chrono::milliseconds(100),
				[&] { return !st.sink->pieces.empty() || !st.sink->error.Ok(); });
			if (!st.sink->error.Ok())
				return st.sink->error;
			arrived.swap(st.sink->pieces);
		}
		if (arrived.empty()) {
			if (Clock::now() - st.lastProgress > m_client->stallTimeout)
				return MakeResult(kErrTimeout);
			continue;
		}
		st.lastProgress = Clock::now();
		for (auto& piece : arrived) {
			if (piece.offset != m_received)
				return MakeResult(kErrIo, "data out of order");
			m_received += piece.bytes.size();
			const bool eof = piece.eof;
			if (!piece.bytes.empty())
				st.buffer.push_back(std::move(piece));
			if (eof) {
				m_eof = true; // the file got shorter since it was opened
				break;
			}
		}
	}
	if (got == 0 && m_received < m_size && !m_eof)
		IssueReads();
	return MakeResult(kOk);
}

} // namespace fileman
