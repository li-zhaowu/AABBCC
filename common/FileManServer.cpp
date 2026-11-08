// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "FileManServer.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

using namespace extmsg;

namespace fileman {

// --- helpers -----------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& s)
{
	if (s.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), NULL, 0);
	std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

std::string WideToUtf8(const std::wstring& w)
{
	if (w.empty())
		return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), NULL, 0, NULL, NULL);
	std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
	if (n > 0)
		WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, NULL, NULL);
	return s;
}

std::wstring LongPath(const std::string& utf8)
{
	const std::wstring w = Utf8ToWide(utf8);
	if (w.size() >= 2 && w[0] == L'\\' && w[1] == L'\\')
		return L"\\\\?\\UNC\\" + w.substr(2);
	return L"\\\\?\\" + w;
}

std::string Win32Message(DWORD error)
{
	wchar_t* buf = nullptr;
	const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, error, 0, reinterpret_cast<LPWSTR>(&buf), 0, NULL);
	std::wstring w = n && buf ? std::wstring(buf, n) : std::wstring();
	if (buf)
		LocalFree(buf);
	while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.'))
		w.pop_back();
	if (w.empty())
		return "error " + std::to_string(error);
	return WideToUtf8(w);
}

uint64_t FileTimeToU64(const FILETIME& ft)
{
	return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

FILETIME U64ToFileTime(uint64_t v)
{
	FILETIME ft;
	ft.dwLowDateTime = static_cast<DWORD>(v);
	ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
	return ft;
}

uint32_t AttributesToWire(DWORD a)
{
	return static_cast<uint32_t>(a) & kAttrMask;
}

void PurgeOldPartFiles(const std::wstring& folder)
{
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW((folder + L"~uvnc-*.part").c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE)
		return;
	FILETIME nowFt;
	GetSystemTimeAsFileTime(&nowFt);
	const uint64_t now = FileTimeToU64(nowFt);
	const uint64_t week = 7ULL * 24 * 3600 * 10000000ULL;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && now - FileTimeToU64(fd.ftLastWriteTime) > week)
			DeleteFileW((folder + fd.cFileName).c_str());
	} while (FindNextFileW(find, &fd));
	FindClose(find);
}

namespace {

// Impersonates for the lifetime of the object (if a token is given).
class Impersonation {
public:
	explicit Impersonation(HANDLE token)
	{
		if (token) {
			m_active = ImpersonateLoggedOnUser(token) != FALSE;
			CloseHandle(token);
		}
	}
	~Impersonation()
	{
		if (m_active)
			RevertToSelf();
	}
	Impersonation(const Impersonation&) = delete;
	Impersonation& operator=(const Impersonation&) = delete;

private:
	bool m_active = false;
};

// No "insert a disk" dialogs on the server's desktop.
class NoErrorDialogs {
public:
	NoErrorDialogs() { SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &m_old); }
	~NoErrorDialogs() { SetThreadErrorMode(m_old, NULL); }

private:
	DWORD m_old = 0;
};

// True when running as LocalSystem without impersonating anybody (a
// service with nobody logged on).
bool ActingAsLocalSystem()
{
	HANDLE t = NULL;
	if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t) &&
	    !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t))
		return false;
	union {
		TOKEN_USER user;
		BYTE bytes[256];
	} info;
	DWORD n = 0;
	bool system = false;
	if (GetTokenInformation(t, TokenUser, &info, sizeof(info), &n))
		system = IsWellKnownSid(info.user.User.Sid, WinLocalSystemSid) != FALSE;
	CloseHandle(t);
	return system;
}

bool IsDots(const wchar_t* name)
{
	return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

// Delete a directory tree. Junctions and symbolic links are removed as
// links, never followed.
DWORD DeleteTree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
		FIND_FIRST_EX_LARGE_FETCH);
	if (find != INVALID_HANDLE_VALUE) {
		do {
			if (IsDots(fd.cFileName))
				continue;
			const std::wstring child = dir + L"\\" + fd.cFileName;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
				SetFileAttributesW(child.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
			DWORD err = 0;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
					if (!RemoveDirectoryW(child.c_str()))
						err = GetLastError();
				} else {
					err = DeleteTree(child);
				}
			} else if (!DeleteFileW(child.c_str())) {
				err = GetLastError();
			}
			if (err) {
				FindClose(find);
				return err;
			}
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}
	if (!RemoveDirectoryW(dir.c_str()))
		return GetLastError();
	return 0;
}

} // namespace

// --- engine ------------------------------------------------------------------

FileManServer::FileManServer(ServerCallbacks cb) : m_cb(std::move(cb)) {}

FileManServer::~FileManServer()
{
	Stop();
}

void FileManServer::Log(int level, const std::string& msg)
{
	if (m_cb.log)
		m_cb.log(level, "file manager: " + msg);
}

uint32_t FileManServer::StatusFromWin32(DWORD e)
{
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
	case ERROR_NOT_READY:
	case ERROR_DEV_NOT_EXIST:
		return kErrNotFound;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_WRITE_PROTECT:
	case ERROR_PRIVILEGE_NOT_HELD:
		return kErrAccess;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return kErrExists;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return kErrDiskFull;
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_FILENAME_EXCED_RANGE:
	case ERROR_DIRECTORY:
		return kErrBadPath;
	case ERROR_NOT_SUPPORTED:
		return kErrUnsupported;
	default:
		return kErrIo;
	}
}

void FileManServer::Start()
{
	std::lock_guard<std::mutex> lk(m_mutex);
	if (m_started)
		return;
	m_stop = false;
	m_started = true;
	m_worker = std::thread(&FileManServer::WorkerMain, this);
}

void FileManServer::Stop()
{
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (!m_started)
			return;
		m_stop = true;
		m_jobs.clear();
	}
	m_cond.notify_all();
	if (m_worker.joinable())
		m_worker.join();
	std::lock_guard<std::mutex> lk(m_mutex);
	m_started = false;
}

void FileManServer::OnMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	std::lock_guard<std::mutex> lk(m_mutex);
	if (!m_started || m_stop)
		return;
	Job job;
	job.subtype = subtype;
	job.payload.swap(payload);
	m_jobs.push_back(std::move(job));
	m_cond.notify_one();
}

void FileManServer::WorkerMain()
{
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lk(m_mutex);
			m_cond.wait(lk, [&] { return m_stop || !m_jobs.empty(); });
			if (m_stop)
				break;
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
		}
		HANDLE token = NULL;
		if (m_cb.acquireUserToken && !m_cb.acquireUserToken(&token)) {
			// No logged-on user to act as: refuse (requests with an id).
			uint32_t reqId = 0;
			if (job.payload.size() >= 4 && job.subtype != kWrite) {
				reqId = extmsg::GetU32(job.payload.data());
				Reply(reqId, kErrAccess, {}, m_cb.refusedMessage);
			}
			continue;
		}
		Impersonation imp(token);
		NoErrorDialogs quiet;
		Handle(job);
	}
	{
		HANDLE token = NULL;
		if (!m_cb.acquireUserToken || m_cb.acquireUserToken(&token)) {
			Impersonation imp(token);
			CloseAll();
		} else {
			CloseAll();
		}
	}
}

void FileManServer::Reply(uint32_t reqId, uint32_t status, const std::vector<uint8_t>& body, const std::string& message,
                          uint8_t flags)
{
	ReplyHeader h;
	h.reqId = reqId;
	h.status = status;
	h.flags = flags;
	h.message = message;
	m_cb.send(kPrioControl, kReply, EncodeReply(h, status == kOk ? body : std::vector<uint8_t>()));
}

void FileManServer::ReplyError(uint32_t reqId, DWORD e)
{
	Reply(reqId, StatusFromWin32(e), {}, Win32Message(e));
}

void FileManServer::Handle(Job& job)
{
	const uint8_t* p = job.payload.data();
	const size_t n = job.payload.size();
	uint32_t reqId = 0;
	std::string path;
	switch (job.subtype) {
	case kRoots:
		if (DecodeRootsRequest(p, n, reqId))
			DoRoots(reqId);
		return;
	case kList:
	case kStat:
	case kMkDir:
	case kDelete:
	case kOpenRead:
		if (!DecodePathRequest(p, n, reqId, path))
			break;
		if (!IsAcceptablePath(path)) {
			Reply(reqId, kErrBadPath);
			return;
		}
		if (job.subtype == kList)
			DoList(reqId, path);
		else if (job.subtype == kStat)
			DoStat(reqId, path);
		else if (job.subtype == kMkDir)
			DoMkDir(reqId, path);
		else if (job.subtype == kDelete)
			DoDelete(reqId, path);
		else
			DoOpenRead(reqId, path);
		return;
	case kRename: {
		RenameRequest r;
		if (!DecodeRename(p, n, r))
			break;
		DoRename(r);
		return;
	}
	case kRead: {
		ReadRequest r;
		if (!DecodeRead(p, n, r))
			break;
		DoRead(r);
		return;
	}
	case kClose: {
		uint32_t handle = 0;
		if (!DecodeHandleRequest(p, n, reqId, handle))
			break;
		DoClose(reqId, handle);
		return;
	}
	case kOpenWrite: {
		OpenWriteRequest r;
		if (!DecodeOpenWrite(p, n, r))
			break;
		DoOpenWrite(r);
		return;
	}
	case kWrite: {
		WriteHeader h;
		if (!DecodeWriteHeader(p, n, h))
			break;
		DoWrite(h, p + kWriteHeaderSize, n - kWriteHeaderSize);
		return;
	}
	case kCloseWrite: {
		CloseWriteRequest r;
		if (!DecodeCloseWrite(p, n, r))
			break;
		DoCloseWrite(r);
		return;
	}
	default:
		break;
	}
	// Malformed or unknown: answer if there is a request id to answer to.
	Log(0, "bad request (subtype " + std::to_string(job.subtype) + ")");
	if (n >= 4 && job.subtype != kWrite)
		Reply(extmsg::GetU32(p), kErrBadRequest);
}

void FileManServer::DoRoots(uint32_t reqId)
{
	std::vector<Root> roots;
	const DWORD drives = GetLogicalDrives();
	for (int i = 0; i < 26; ++i) {
		if (!(drives & (1u << i)))
			continue;
		wchar_t rootW[] = { wchar_t(L'A' + i), L':', L'\\', 0 };
		Root root;
		root.path = WideToUtf8(rootW);
		switch (GetDriveTypeW(rootW)) {
		case DRIVE_FIXED: root.type = kRootFixed; break;
		case DRIVE_REMOVABLE: root.type = kRootRemovable; break;
		case DRIVE_REMOTE: root.type = kRootNetwork; break;
		case DRIVE_CDROM: root.type = kRootCdrom; break;
		case DRIVE_RAMDISK: root.type = kRootRam; break;
		case DRIVE_NO_ROOT_DIR: continue;
		default: root.type = kRootUnknown; break;
		}
		// Removable and optical drives may be empty: no label, no sizes,
		// and asking them can take seconds. Fixed and network drives only.
		if (root.type == kRootFixed || root.type == kRootNetwork || root.type == kRootRam) {
			wchar_t label[MAX_PATH + 1] = {};
			if (GetVolumeInformationW(rootW, label, MAX_PATH, NULL, NULL, NULL, NULL, 0))
				root.label = WideToUtf8(label);
			ULARGE_INTEGER freeToUser, total;
			if (GetDiskFreeSpaceExW(rootW, &freeToUser, &total, NULL)) {
				root.totalBytes = total.QuadPart;
				root.freeBytes = freeToUser.QuadPart;
			}
		}
		roots.push_back(std::move(root));
	}
	// Nobody logged on (service identity): the system account's own folders
	// are of no use; offer the users' folders instead.
	if (ActingAsLocalSystem()) {
		PWSTR users = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProfiles, 0, NULL, &users)) && users) {
			Root root;
			root.path = WideToUtf8(users);
			root.label = "Users";
			root.type = kRootFolder;
			if (IsAcceptablePath(root.path))
				roots.push_back(std::move(root));
		}
		CoTaskMemFree(users);
		Reply(reqId, kOk, EncodeRoots(roots));
		return;
	}
	// The (impersonated) user's own folders.
	HANDLE token = NULL;
	OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &token);
	const struct { const KNOWNFOLDERID* id; const char* name; } folders[] = {
		{ &FOLDERID_Desktop, "Desktop" },
		{ &FOLDERID_Documents, "Documents" },
		{ &FOLDERID_Downloads, "Downloads" },
		{ &FOLDERID_Profile, "Home" },
	};
	for (const auto& f : folders) {
		PWSTR path = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(*f.id, KF_FLAG_DONT_VERIFY, token, &path)) && path) {
			Root root;
			root.path = WideToUtf8(path);
			root.label = f.name;
			root.type = kRootFolder;
			if (IsAcceptablePath(root.path))
				roots.push_back(std::move(root));
		}
		CoTaskMemFree(path);
	}
	if (token)
		CloseHandle(token);
	Reply(reqId, kOk, EncodeRoots(roots));
}

void FileManServer::DoList(uint32_t reqId, const std::string& path)
{
	const std::wstring dir = LongPath(path);
	WIN32_FIND_DATAW fd;
	const std::wstring pattern = dir.back() == L'\\' ? dir + L"*" : dir + L"\\*";
	HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
		FIND_FIRST_EX_LARGE_FETCH);
	if (find == INVALID_HANDLE_VALUE) {
		const DWORD e = GetLastError();
		if (e == ERROR_FILE_NOT_FOUND) { // an empty drive root
			Reply(reqId, kOk, EncodeEntries({}));
			return;
		}
		ReplyError(reqId, e);
		return;
	}
	std::vector<Entry> page;
	size_t total = 0;
	do {
		if (IsDots(fd.cFileName))
			continue;
		Entry e;
		e.name = WideToUtf8(fd.cFileName);
		e.attributes = AttributesToWire(fd.dwFileAttributes);
		e.size = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0
			: (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
		e.mtime = FileTimeToU64(fd.ftLastWriteTime);
		page.push_back(std::move(e));
		if (page.size() == kListPageEntries) {
			Reply(reqId, kOk, EncodeEntries(page), std::string(), kReplyMore);
			total += page.size();
			page.clear();
		}
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			if (m_stop)
				break;
		}
	} while (FindNextFileW(find, &fd));
	FindClose(find);
	total += page.size();
	Reply(reqId, kOk, EncodeEntries(page));
	Log(2, "listed " + path + " (" + std::to_string(total) + " entries)");
}

void FileManServer::DoStat(uint32_t reqId, const std::string& path)
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &a)) {
		ReplyError(reqId, GetLastError());
		return;
	}
	Entry e;
	e.attributes = AttributesToWire(a.dwFileAttributes);
	e.size = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
	e.mtime = FileTimeToU64(a.ftLastWriteTime);
	Reply(reqId, kOk, EncodeStat(e));
}

void FileManServer::DoMkDir(uint32_t reqId, const std::string& path)
{
	if (IsRootPath(path)) {
		Reply(reqId, kErrBadPath);
		return;
	}
	if (!CreateDirectoryW(LongPath(path).c_str(), NULL)) {
		ReplyError(reqId, GetLastError());
		return;
	}
	Log(1, "created folder " + path);
	Reply(reqId, kOk);
}

void FileManServer::DoRename(const RenameRequest& r)
{
	if (!IsAcceptablePath(r.from) || !IsAcceptablePath(r.to) || IsRootPath(r.from) || IsRootPath(r.to)) {
		Reply(r.reqId, kErrBadPath);
		return;
	}
	if (!MoveFileExW(LongPath(r.from).c_str(), LongPath(r.to).c_str(), MOVEFILE_COPY_ALLOWED)) {
		ReplyError(r.reqId, GetLastError());
		return;
	}
	Log(1, "renamed " + r.from + " to " + r.to);
	Reply(r.reqId, kOk);
}

void FileManServer::DoDelete(uint32_t reqId, const std::string& path)
{
	if (IsRootPath(path)) {
		Reply(reqId, kErrBadPath, {}, "a drive cannot be deleted");
		return;
	}
	const std::wstring w = LongPath(path);
	const DWORD attrs = GetFileAttributesW(w.c_str());
	if (attrs == INVALID_FILE_ATTRIBUTES) {
		ReplyError(reqId, GetLastError());
		return;
	}
	if (attrs & FILE_ATTRIBUTE_READONLY)
		SetFileAttributesW(w.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
	DWORD err = 0;
	if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
		if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
			err = RemoveDirectoryW(w.c_str()) ? 0 : GetLastError();
		else
			err = DeleteTree(w);
	} else {
		err = DeleteFileW(w.c_str()) ? 0 : GetLastError();
	}
	if (err) {
		ReplyError(reqId, err);
		return;
	}
	Log(1, "deleted " + path);
	Reply(reqId, kOk);
}

void FileManServer::DoOpenRead(uint32_t reqId, const std::string& path)
{
	if (m_open.size() >= kMaxOpenHandles) {
		Reply(reqId, kErrBusy);
		return;
	}
	HANDLE h = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		ReplyError(reqId, GetLastError());
		return;
	}
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		CloseHandle(h);
		Reply(reqId, kErrBadPath, {}, "not a file");
		return;
	}
	Open o;
	o.handle = h;
	const uint32_t id = m_nextHandle++;
	m_open[id] = o;
	OpenReadReply rep;
	rep.handle = id;
	rep.size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
	rep.mtime = FileTimeToU64(info.ftLastWriteTime);
	Log(1, "sending " + path);
	Reply(reqId, kOk, EncodeOpenReadReply(rep));
}

void FileManServer::DoRead(const ReadRequest& r)
{
	auto it = m_open.find(r.handle);
	if (it == m_open.end() || it->second.write) {
		Reply(r.reqId, kErrBadHandle);
		return;
	}
	HANDLE h = it->second.handle;
	LARGE_INTEGER pos;
	pos.QuadPart = static_cast<LONGLONG>(r.offset);
	if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) {
		ReplyError(r.reqId, GetLastError());
		return;
	}
	std::vector<uint8_t> buf(kDataPieceBytes);
	uint64_t offset = r.offset;
	uint64_t left = r.length;
	while (left > 0) {
		const DWORD want = static_cast<DWORD>((std::min<uint64_t>)(left, kDataPieceBytes));
		DWORD got = 0;
		if (!ReadFile(h, buf.data(), want, &got, NULL)) {
			ReplyError(r.reqId, GetLastError());
			return;
		}
		DataHeader dh;
		dh.handle = r.handle;
		dh.offset = offset;
		dh.flags = got < want ? kDataEof : 0;
		if (!m_cb.send(kPrioBulk, kData, EncodeData(dh, buf.data(), got)))
			return; // connection gone
		offset += got;
		left -= got;
		if (got < want)
			return; // end of file
		std::lock_guard<std::mutex> lk(m_mutex);
		if (m_stop)
			return;
	}
}

void FileManServer::DoClose(uint32_t reqId, uint32_t handle)
{
	auto it = m_open.find(handle);
	if (it == m_open.end() || it->second.write) {
		Reply(reqId, kErrBadHandle);
		return;
	}
	CloseHandle(it->second.handle);
	m_open.erase(it);
	Reply(reqId, kOk);
}

void FileManServer::DoOpenWrite(const OpenWriteRequest& r)
{
	if (!IsAcceptablePath(r.path) || IsRootPath(r.path)) {
		Reply(r.reqId, kErrBadPath);
		return;
	}
	if (m_open.size() >= kMaxOpenHandles) {
		Reply(r.reqId, kErrBusy);
		return;
	}
	const std::wstring target = LongPath(r.path);
	const DWORD attrs = GetFileAttributesW(target.c_str());
	if (attrs != INVALID_FILE_ATTRIBUTES) {
		if ((attrs & FILE_ATTRIBUTE_DIRECTORY) || !(r.flags & kWriteOverwrite)) {
			Reply(r.reqId, kErrExists, {}, (attrs & FILE_ATTRIBUTE_DIRECTORY) ? "a folder with this name exists" : "");
			return;
		}
	}
	const std::string parent = ParentPath(r.path);
	if (parent.empty()) {
		Reply(r.reqId, kErrBadPath);
		return;
	}
	std::wstring parentW = LongPath(parent);
	if (parentW.back() != L'\\')
		parentW += L'\\';
	// Hidden temporary file in the same folder (same volume: the final
	// rename is atomic). Resumable: named after the target, size and time.
	const bool resumable = (r.flags & kWriteResume) != 0;
	const std::wstring temp = resumable ? parentW + Utf8ToWide(ResumeTempName(r.path, r.size, r.mtime))
		: parentW + L"~uvnc-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++m_tempCounter) + L".part";
	if (resumable) {
		// A retry while the earlier attempt is still open (e.g. after a
		// timeout): close that one, keeping its data.
		for (auto it = m_open.begin(); it != m_open.end(); ++it) {
			if (it->second.write && _wcsicmp(it->second.temp.c_str(), temp.c_str()) == 0) {
				CloseHandle(it->second.handle);
				m_open.erase(it);
				break;
			}
		}
	}
	// Continue a partial upload of the same file version.
	uint64_t offset = 0;
	HANDLE h = INVALID_HANDLE_VALUE;
	if (resumable) {
		h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		LARGE_INTEGER existing;
		if (h != INVALID_HANDLE_VALUE) {
			LARGE_INTEGER zero = {};
			if (GetFileSizeEx(h, &existing) && static_cast<uint64_t>(existing.QuadPart) <= r.size &&
			    SetFilePointerEx(h, zero, NULL, FILE_END)) {
				offset = static_cast<uint64_t>(existing.QuadPart);
			} else {
				CloseHandle(h); // bigger than the file: not ours to continue
				h = INVALID_HANDLE_VALUE;
			}
		}
	}
	ULARGE_INTEGER freeToUser;
	if (GetDiskFreeSpaceExW(parentW.c_str(), &freeToUser, NULL, NULL) && freeToUser.QuadPart < r.size - offset) {
		if (h != INVALID_HANDLE_VALUE)
			CloseHandle(h);
		Reply(r.reqId, kErrDiskFull, {}, Win32Message(ERROR_DISK_FULL));
		return;
	}
	if (h == INVALID_HANDLE_VALUE) {
		if (resumable)
			PurgeOldPartFiles(parentW);
		h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
		if (h == INVALID_HANDLE_VALUE) {
			ReplyError(r.reqId, GetLastError());
			return;
		}
	}
	Open o;
	o.handle = h;
	o.write = true;
	o.target = target;
	o.temp = temp;
	o.overwrite = (r.flags & kWriteOverwrite) != 0;
	o.resumable = resumable;
	o.written = offset;
	const uint32_t id = m_nextHandle++;
	m_open[id] = o;
	Log(1, "receiving " + r.path + (offset ? " (continuing at byte " + std::to_string(offset) + ")" : ""));
	OpenWriteReply rep;
	rep.handle = id;
	rep.offset = offset;
	Reply(r.reqId, kOk, EncodeOpenWriteReply(rep));
}

void FileManServer::DoWrite(const WriteHeader& h, const uint8_t* bytes, size_t n)
{
	WriteAck ack;
	ack.handle = h.handle;
	auto it = m_open.find(h.handle);
	if (it == m_open.end() || !it->second.write) {
		ack.status = kErrBadHandle;
		m_cb.send(kPrioControl, kWriteAck, EncodeWriteAck(ack));
		return;
	}
	Open& o = it->second;
	if (o.failed == kOk) {
		if (h.offset != o.written) {
			o.failed = kErrBadRequest; // out of order
		} else {
			DWORD done = 0;
			if (n && (!WriteFile(o.handle, bytes, static_cast<DWORD>(n), &done, NULL) || done != n))
				o.failed = StatusFromWin32(GetLastError());
			else
				o.written += n;
		}
		ack.status = o.failed;
		ack.written = o.written;
		m_cb.send(kPrioControl, kWriteAck, EncodeWriteAck(ack));
	}
	// After a failure the viewer has been told once; it stops and sends
	// CloseWrite. Later writes are dropped quietly.
}

void FileManServer::DoCloseWrite(const CloseWriteRequest& r)
{
	auto it = m_open.find(r.handle);
	if (it == m_open.end() || !it->second.write) {
		Reply(r.reqId, kErrBadHandle);
		return;
	}
	Open o = it->second;
	m_open.erase(it);
	uint32_t status = o.failed;
	std::string message;
	if (r.commit && status == kOk && r.mtime) {
		const FILETIME ft = U64ToFileTime(r.mtime);
		SetFileTime(o.handle, NULL, NULL, &ft);
	}
	CloseHandle(o.handle);
	if (r.commit && status == kOk) {
		if (o.overwrite) {
			const DWORD attrs = GetFileAttributesW(o.target.c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
				SetFileAttributesW(o.target.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
		}
		if (MoveFileExW(o.temp.c_str(), o.target.c_str(), o.overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) {
			// The temporary file was hidden; the real one is not.
			const DWORD attrs = GetFileAttributesW(o.target.c_str());
			if (attrs != INVALID_FILE_ATTRIBUTES)
				SetFileAttributesW(o.target.c_str(), attrs & ~FILE_ATTRIBUTE_HIDDEN);
			Reply(r.reqId, kOk);
			return;
		}
		const DWORD e = GetLastError();
		status = StatusFromWin32(e);
		message = Win32Message(e);
	}
	DeleteFileW(o.temp.c_str());
	Reply(r.reqId, r.commit ? (status == kOk ? kErrIo : status) : kOk, {}, message);
}

void FileManServer::CloseAll()
{
	for (auto& kv : m_open) {
		CloseHandle(kv.second.handle);
		// Connection gone: a resumable upload keeps what it has so far.
		if (kv.second.write && !kv.second.resumable)
			DeleteFileW(kv.second.temp.c_str());
	}
	m_open.clear();
}

} // namespace fileman
