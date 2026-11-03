// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#ifdef _WIN32

#include "FileClipboard.h"

#include <shlobj.h>
#include <shellapi.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#endif

using extmsg::kPrioBulk;
using extmsg::kPrioControl;

namespace fileclip {

namespace {

// Message priorities. Everything that must keep its order relative to file
// data (Offer, Revoke, Data, Error) uses the bulk queue; the target's small
// requests use the control queue so they are not stuck behind file data.
constexpr extmsg::ExtPriority kPrioOffer  = kPrioBulk;
constexpr extmsg::ExtPriority kPrioRevoke = kPrioBulk;
constexpr extmsg::ExtPriority kPrioData   = kPrioBulk;
constexpr extmsg::ExtPriority kPrioError  = kPrioBulk;
constexpr extmsg::ExtPriority kPrioRead   = kPrioControl;
constexpr extmsg::ExtPriority kPrioCancel = kPrioControl;

// Target read-ahead.
constexpr uint32_t kReadChunk  = 1024 * 1024;
constexpr uint64_t kReadWindow = 8ull * 1024 * 1024;
constexpr auto kStallTimeout   = std::chrono::seconds(60);

// Source limits.
constexpr size_t kMaxOpenFiles = 16;
constexpr int kMaxDepth = 64;

const UINT WMU_OFFER      = WM_APP + 0x101;
const UINT WMU_REVOKE     = WM_APP + 0x102;
const UINT WMU_CHECK      = WM_APP + 0x103;
const UINT WMU_DISCONNECT = WM_APP + 0x104;
const UINT WMU_QUIT       = WM_APP + 0x105;

// Clipboard change notifications can be missed (we have seen it depend on
// how and as whom the process runs), so the sequence number is also polled.
const UINT_PTR kPollTimerId = 1;
const UINT kPollIntervalMs  = 500;

const wchar_t kWindowClass[] = L"UltraVNCFileClipboard";

struct Formats {
	CLIPFORMAT descriptor;
	CLIPFORMAT contents;
	CLIPFORMAT dropEffect;
	CLIPFORMAT marker; // identifies virtual files put there by UltraVNC
	Formats()
	{
		descriptor = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
		// Spelled out: CFSTR_FILECONTENTS and CFSTR_PREFERREDDROPEFFECT are
		// narrow strings in ANSI builds (the server is built without UNICODE).
		contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
		dropEffect = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Preferred DropEffect"));
		marker = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UltraVNC FileClip"));
	}
};

const Formats& Fmt()
{
	static const Formats f;
	return f;
}

std::wstring Utf8ToWide(const std::string& s)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), NULL, 0);
	std::wstring w(n > 0 ? n : 0, L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

std::string WideToUtf8(const std::wstring& w)
{
	if (w.empty())
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), NULL, 0, NULL, NULL);
	std::string s(n > 0 ? n : 0, '\0');
	if (n > 0)
		WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, NULL, NULL);
	return s;
}

// \\?\ form so that paths longer than MAX_PATH work.
std::wstring LongPath(const std::wstring& p)
{
	if (p.compare(0, 4, L"\\\\?\\") == 0)
		return p;
	if (p.compare(0, 2, L"\\\\") == 0)
		return L"\\\\?\\UNC\\" + p.substr(2);
	return L"\\\\?\\" + p;
}

uint64_t FileTimeToU64(const FILETIME& ft)
{
	return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

FILETIME U64ToFileTime(uint64_t v)
{
	FILETIME ft;
	ft.dwLowDateTime = static_cast<DWORD>(v);
	ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
	return ft;
}

// RAII thread impersonation.
class Impersonation {
public:
	explicit Impersonation(HANDLE token)
	{
		if (token)
			m_active = ImpersonateLoggedOnUser(token) != FALSE;
	}
	~Impersonation()
	{
		if (m_active)
			RevertToSelf();
	}
	bool Failed(HANDLE token) const { return token != NULL && !m_active; }

private:
	bool m_active = false;
};

HGLOBAL GlobalDword(DWORD v)
{
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
	if (h) {
		DWORD* p = static_cast<DWORD*>(GlobalLock(h));
		*p = v;
		GlobalUnlock(h);
	}
	return h;
}

} // namespace

// ===========================================================================
// Target side COM objects
// ===========================================================================

struct RemoteOffer {
	uint32_t seq = 0;
	std::vector<Entry> entries;   // safe entries only
	std::vector<uint32_t> idMap;  // entries[i] is file id idMap[i] at the source
};

class RemoteStream final : public IStream {
public:
	RemoteStream(FileClipboard* engine, uint32_t seq, uint32_t fileId, uint64_t size,
	             uint64_t mtime, std::wstring name)
		: m_engine(engine), m_seq(seq), m_fileId(fileId), m_size(size), m_mtime(mtime),
		  m_name(std::move(name))
	{
		m_id = m_engine->NewStreamId();
		m_engine->RegisterStream(m_id, this);
	}

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
			*ppv = static_cast<IStream*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (r == 0)
			delete this;
		return r;
	}

	// ISequentialStream
	STDMETHODIMP Read(void* pv, ULONG cb, ULONG* pcbRead) override
	{
		if (!pv)
			return STG_E_INVALIDPOINTER;
		ULONG done = 0;
		HRESULT hr = S_OK;
		auto lastProgress = std::chrono::steady_clock::now();
		std::unique_lock<std::mutex> lk(m_mutex);
		while (done < cb) {
			if (m_pos >= m_size)
				break;
			if (m_failed || m_engine->IsStopping() || !m_engine->IsConnected()) {
				hr = STG_E_READFAULT;
				break;
			}
			RequestMoreLocked();
			if (m_bufEnd > m_pos) {
				done += CopyOutLocked(static_cast<uint8_t*>(pv) + done, cb - done);
				lastProgress = std::chrono::steady_clock::now();
				continue;
			}
			if (m_eof)
				break; // the file is shorter than announced
			m_cond.wait_for(lk, std::chrono::milliseconds(500));
			if (std::chrono::steady_clock::now() - lastProgress > kStallTimeout) {
				m_failed = true;
				hr = STG_E_READFAULT;
				m_engine->Log(0, "file clipboard: no data for 60 s, giving up");
				break;
			}
		}
		if (pcbRead)
			*pcbRead = done;
		if (FAILED(hr))
			return done > 0 ? S_FALSE : hr;
		return done < cb ? S_FALSE : S_OK;
	}

	STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }

	// IStream
	STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override
	{
		std::unique_lock<std::mutex> lk(m_mutex);
		int64_t base;
		switch (origin) {
		case STREAM_SEEK_SET: base = 0; break;
		case STREAM_SEEK_CUR: base = static_cast<int64_t>(m_pos); break;
		case STREAM_SEEK_END: base = static_cast<int64_t>(m_size); break;
		default: return STG_E_INVALIDFUNCTION;
		}
		const int64_t target = base + move.QuadPart;
		if (target < 0)
			return STG_E_INVALIDFUNCTION;
		const uint64_t pos = static_cast<uint64_t>(target);

		if (pos >= m_pos && pos <= m_bufEnd) {
			// Forward within what we already have: just skip.
			SkipLocked(pos - m_pos);
		} else if (pos != m_pos) {
			// Start over at the new position with a fresh stream id, so that
			// data still in flight for the old position is ignored.
			const uint32_t oldId = m_id;
			m_id = m_engine->NewStreamId();
			m_pieces.clear();
			m_frontOff = 0;
			m_pos = m_bufEnd = m_requested = pos;
			m_eof = false;
			const uint32_t newId = m_id;
			lk.unlock();
			m_engine->Send(kPrioCancel, kCancel, EncodeCancel(oldId));
			m_engine->UnregisterStream(oldId);
			m_engine->RegisterStream(newId, this);
			lk.lock();
		}
		if (newPos)
			newPos->QuadPart = m_pos;
		return S_OK;
	}

	STDMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }

	STDMETHODIMP CopyTo(IStream* dest, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead,
	                    ULARGE_INTEGER* pcbWritten) override
	{
		if (!dest)
			return STG_E_INVALIDPOINTER;
		std::vector<uint8_t> buf(kDataPieceBytes);
		uint64_t remaining = cb.QuadPart, totalRead = 0, totalWritten = 0;
		HRESULT hr = S_OK;
		while (remaining > 0) {
			ULONG want = static_cast<ULONG>(std::min<uint64_t>(remaining, buf.size()));
			ULONG got = 0;
			hr = Read(buf.data(), want, &got);
			if (FAILED(hr))
				break;
			totalRead += got;
			if (got == 0)
				break;
			ULONG written = 0;
			hr = dest->Write(buf.data(), got, &written);
			totalWritten += written;
			if (FAILED(hr))
				break;
			remaining -= got;
			if (hr == S_FALSE || got < want)
				break;
		}
		if (pcbRead)
			pcbRead->QuadPart = totalRead;
		if (pcbWritten)
			pcbWritten->QuadPart = totalWritten;
		return FAILED(hr) ? hr : S_OK;
	}

	STDMETHODIMP Commit(DWORD) override { return S_OK; }
	STDMETHODIMP Revert() override { return E_NOTIMPL; }
	STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
	STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }

	STDMETHODIMP Stat(STATSTG* st, DWORD flags) override
	{
		if (!st)
			return STG_E_INVALIDPOINTER;
		std::memset(st, 0, sizeof(*st));
		st->type = STGTY_STREAM;
		st->cbSize.QuadPart = m_size;
		st->mtime = U64ToFileTime(m_mtime);
		st->grfMode = STGM_READ;
		if (!(flags & STATFLAG_NONAME)) {
			const size_t bytes = (m_name.size() + 1) * sizeof(wchar_t);
			st->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
			if (!st->pwcsName)
				return E_OUTOFMEMORY;
			std::memcpy(st->pwcsName, m_name.c_str(), bytes);
		}
		return S_OK;
	}

	STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

	// Called by the engine (transport reader thread) with m_streamMutex held.
	void Deliver(uint32_t id, uint64_t offset, uint8_t flags, const uint8_t* p, size_t n)
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (id != m_id || m_failed)
			return;
		if (offset != m_bufEnd) {
			m_failed = true; // out of order: cannot happen with a correct peer
		} else {
			if (n > 0) {
				m_pieces.emplace_back(p, p + n);
				m_bufEnd += n;
			}
			if (flags & kDataEof)
				m_eof = true;
		}
		m_cond.notify_all();
	}

	void Fail(uint32_t id)
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (id != 0 && id != m_id)
			return;
		m_failed = true;
		m_cond.notify_all();
	}

private:
	~RemoteStream()
	{
		m_engine->UnregisterStream(m_id);
		// Tell the source to close the file (harmless if it already did).
		m_engine->Send(kPrioCancel, kCancel, EncodeCancel(m_id));
	}

	void RequestMoreLocked()
	{
		const uint64_t want = std::min<uint64_t>(m_size, m_pos + kReadWindow);
		while (m_requested < want && !m_eof && !m_failed) {
			fileclip::Read rd;
			rd.seq = m_seq;
			rd.streamId = m_id;
			rd.fileId = m_fileId;
			rd.offset = m_requested;
			rd.length = static_cast<uint32_t>(std::min<uint64_t>(kReadChunk, want - m_requested));
			if (!m_engine->Send(kPrioRead, kRead, EncodeRead(rd))) {
				m_failed = true;
				break;
			}
			m_requested += rd.length;
		}
	}

	ULONG CopyOutLocked(uint8_t* out, ULONG cb)
	{
		ULONG done = 0;
		while (done < cb && !m_pieces.empty()) {
			std::vector<uint8_t>& front = m_pieces.front();
			const size_t n = std::min<size_t>(front.size() - m_frontOff, cb - done);
			std::memcpy(out + done, front.data() + m_frontOff, n);
			done += static_cast<ULONG>(n);
			m_frontOff += n;
			m_pos += n;
			if (m_frontOff == front.size()) {
				m_pieces.pop_front();
				m_frontOff = 0;
			}
		}
		return done;
	}

	void SkipLocked(uint64_t n)
	{
		while (n > 0 && !m_pieces.empty()) {
			std::vector<uint8_t>& front = m_pieces.front();
			const size_t k = static_cast<size_t>(std::min<uint64_t>(front.size() - m_frontOff, n));
			m_frontOff += k;
			m_pos += k;
			n -= k;
			if (m_frontOff == front.size()) {
				m_pieces.pop_front();
				m_frontOff = 0;
			}
		}
	}

	LONG m_ref = 1;
	FileClipboard* m_engine;
	const uint32_t m_seq;
	const uint32_t m_fileId;
	const uint64_t m_size;
	const uint64_t m_mtime;
	const std::wstring m_name;

	std::mutex m_mutex;
	std::condition_variable m_cond;
	uint32_t m_id = 0;
	uint64_t m_pos = 0;       // next byte Explorer reads
	uint64_t m_bufEnd = 0;    // end of received data; buffer holds [m_pos, m_bufEnd)
	uint64_t m_requested = 0; // end of requested data
	std::deque<std::vector<uint8_t>> m_pieces;
	size_t m_frontOff = 0;
	bool m_eof = false;
	bool m_failed = false;
};

class RemoteDataObject final : public IDataObject {
public:
	RemoteDataObject(FileClipboard* engine, std::shared_ptr<RemoteOffer> offer)
		: m_engine(engine), m_offer(std::move(offer))
	{
	}

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDataObject) {
			*ppv = static_cast<IDataObject*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (r == 0)
			delete this;
		return r;
	}

	// IDataObject
	STDMETHODIMP GetData(FORMATETC* fe, STGMEDIUM* medium) override
	{
		if (!fe || !medium)
			return E_INVALIDARG;
		std::memset(medium, 0, sizeof(*medium));
		const Formats& f = Fmt();

		if (fe->cfFormat == f.descriptor && (fe->tymed & TYMED_HGLOBAL)) {
			HGLOBAL h = BuildDescriptor();
			if (!h)
				return E_OUTOFMEMORY;
			medium->tymed = TYMED_HGLOBAL;
			medium->hGlobal = h;
			return S_OK;
		}
		if (fe->cfFormat == f.contents && (fe->tymed & TYMED_ISTREAM)) {
			if (fe->lindex < 0 || static_cast<size_t>(fe->lindex) >= m_offer->entries.size())
				return DV_E_LINDEX;
			const Entry& e = m_offer->entries[fe->lindex];
			if (e.IsDirectory())
				return DV_E_LINDEX;
			if (m_engine->IsStopping() || !m_engine->IsConnected())
				return E_FAIL;
			std::wstring name = Utf8ToWide(e.path);
			const size_t slash = name.find_last_of(L'/');
			if (slash != std::wstring::npos)
				name = name.substr(slash + 1);
			medium->tymed = TYMED_ISTREAM;
			medium->pstm = new RemoteStream(m_engine, m_offer->seq, m_offer->idMap[fe->lindex],
				e.size, e.mtime, name);
			return S_OK;
		}
		if (fe->cfFormat == f.dropEffect && (fe->tymed & TYMED_HGLOBAL)) {
			medium->tymed = TYMED_HGLOBAL;
			medium->hGlobal = GlobalDword(DROPEFFECT_COPY);
			return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
		}
		if (fe->cfFormat == f.marker && (fe->tymed & TYMED_HGLOBAL)) {
			medium->tymed = TYMED_HGLOBAL;
			medium->hGlobal = GlobalDword(GetCurrentProcessId());
			return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
		}
		return DV_E_FORMATETC;
	}

	STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }

	STDMETHODIMP QueryGetData(FORMATETC* fe) override
	{
		if (!fe)
			return E_INVALIDARG;
		const Formats& f = Fmt();
		if ((fe->cfFormat == f.descriptor || fe->cfFormat == f.dropEffect || fe->cfFormat == f.marker) &&
		    (fe->tymed & TYMED_HGLOBAL))
			return S_OK;
		if (fe->cfFormat == f.contents && (fe->tymed & TYMED_ISTREAM))
			return S_OK;
		return DV_E_FORMATETC;
	}

	STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override
	{
		if (out)
			out->ptd = NULL;
		return E_NOTIMPL;
	}

	// Explorer reports the paste result (CFSTR_PERFORMEDDROPEFFECT,
	// CFSTR_PASTESUCCEEDED, ...). Accept and ignore.
	STDMETHODIMP SetData(FORMATETC*, STGMEDIUM* medium, BOOL release) override
	{
		if (release && medium)
			ReleaseStgMedium(medium);
		return S_OK;
	}

	STDMETHODIMP EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override
	{
		if (!out)
			return E_POINTER;
		if (dir != DATADIR_GET) {
			*out = NULL;
			return E_NOTIMPL;
		}
		const Formats& f = Fmt();
		FORMATETC fmts[4] = {
			{ f.descriptor, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
			{ f.contents,   NULL, DVASPECT_CONTENT, -1, TYMED_ISTREAM },
			{ f.dropEffect, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
			{ f.marker,     NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
		};
		return SHCreateStdEnumFmtEtc(4, fmts, out);
	}

	STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
	STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
	~RemoteDataObject() = default;

	HGLOBAL BuildDescriptor() const
	{
		const size_t n = m_offer->entries.size();
		const size_t bytes = sizeof(FILEGROUPDESCRIPTORW) + (n > 0 ? n - 1 : 0) * sizeof(FILEDESCRIPTORW);
		HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
		if (!h)
			return NULL;
		FILEGROUPDESCRIPTORW* g = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(h));
		g->cItems = static_cast<UINT>(n);
		for (size_t i = 0; i < n; ++i) {
			const Entry& e = m_offer->entries[i];
			FILEDESCRIPTORW& d = g->fgd[i];
			d.dwFlags = FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI;
			if (e.IsDirectory()) {
				d.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | (e.attributes & kAttrHidden);
			} else {
				d.dwFlags |= FD_FILESIZE;
				d.dwFileAttributes = e.attributes & (kAttrReadOnly | kAttrHidden | kAttrArchive);
				if (d.dwFileAttributes == 0)
					d.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
				d.nFileSizeHigh = static_cast<DWORD>(e.size >> 32);
				d.nFileSizeLow = static_cast<DWORD>(e.size);
			}
			if (e.mtime != 0) {
				d.dwFlags |= FD_WRITESTIME;
				d.ftLastWriteTime = U64ToFileTime(e.mtime);
			}
			std::wstring name = Utf8ToWide(e.path);
			std::replace(name.begin(), name.end(), L'/', L'\\');
			// IsSafeRelativePath() guarantees it fits.
			const size_t len = std::min<size_t>(name.size(), MAX_PATH - 1);
			std::memcpy(d.cFileName, name.data(), len * sizeof(wchar_t));
			d.cFileName[len] = L'\0';
		}
		GlobalUnlock(h);
		return h;
	}

	LONG m_ref = 1;
	FileClipboard* m_engine;
	std::shared_ptr<RemoteOffer> m_offer;
};

// ===========================================================================
// FileClipboard
// ===========================================================================

FileClipboard::FileClipboard(Callbacks cb) : m_cb(std::move(cb)) {}

FileClipboard::~FileClipboard()
{
	Stop();
}

void FileClipboard::Log(int level, const std::string& msg)
{
	if (m_cb.log)
		m_cb.log(level, msg);
}

bool FileClipboard::Send(extmsg::ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload)
{
	return m_cb.send && m_cb.send(prio, subtype, payload);
}

bool FileClipboard::Start()
{
	if (m_started)
		return m_hwnd != NULL;
	m_started = true;
	m_stopping = false;
	{
		std::lock_guard<std::mutex> lk(m_jobMutex);
		m_workerStop = false;
	}
	m_readyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	m_worker = std::thread(&FileClipboard::WorkerMain, this);
	m_clipThread = std::thread(&FileClipboard::ClipThreadMain, this);
	if (m_readyEvent)
		WaitForSingleObject(m_readyEvent, 10000);
	if (!m_hwnd)
		Log(0, "file clipboard: could not create the clipboard window");
	return m_hwnd != NULL;
}

void FileClipboard::Stop()
{
	if (!m_started)
		return;
	m_stopping = true;
	m_connected = false;
	FailAllStreams(); // wakes an IStream::Read blocked on the clipboard thread
	if (m_hwnd)
		PostMessageW(m_hwnd, WMU_QUIT, 0, 0);
	if (m_clipThread.joinable())
		m_clipThread.join();
	{
		std::lock_guard<std::mutex> lk(m_jobMutex);
		m_workerStop = true;
		m_jobs.clear();
	}
	m_jobCond.notify_all();
	if (m_worker.joinable())
		m_worker.join();
	if (m_readyEvent) {
		CloseHandle(m_readyEvent);
		m_readyEvent = NULL;
	}
	m_started = false;
}

void FileClipboard::OnConnected()
{
	m_connected = true;
	if (m_hwnd)
		PostMessageW(m_hwnd, WMU_CHECK, 0, 0);
}

void FileClipboard::OnDisconnected()
{
	m_connected = false;
	FailAllStreams();
	{
		std::lock_guard<std::mutex> lk(m_jobMutex);
		m_jobs.clear();
		m_cancelled.clear();
		m_dropOpenFiles = true;
	}
	m_jobCond.notify_all();
	{
		std::lock_guard<std::mutex> lk(m_srcMutex);
		m_localSeq = 0;
		m_localFiles.clear();
	}
	if (m_hwnd)
		PostMessageW(m_hwnd, WMU_DISCONNECT, 0, 0);
}

void FileClipboard::OnMessage(uint8_t subtype, std::vector<uint8_t>& payload)
{
	const uint8_t* p = payload.data();
	const size_t n = payload.size();
	switch (subtype) {
	case kOffer: {
		std::unique_ptr<RemoteOffer> ro(new RemoteOffer);
		Offer offer;
		if (!DecodeOffer(p, n, offer)) {
			Log(0, "file clipboard: malformed offer ignored");
			return;
		}
		const size_t removed = FilterUnsafeEntries(offer, ro->idMap);
		if (removed > 0)
			Log(0, "file clipboard: dropped " + std::to_string(removed) + " entries with unsafe or too long names");
		ro->seq = offer.seq;
		ro->entries.swap(offer.entries);
		Log(1, "file clipboard: peer offers " + std::to_string(ro->entries.size()) + " entries");
		if (m_hwnd && PostMessageW(m_hwnd, WMU_OFFER, 0, reinterpret_cast<LPARAM>(ro.get())))
			ro.release(); // owned by the clipboard thread now
		break;
	}
	case kRevoke: {
		uint32_t seq = 0;
		if (DecodeRevoke(p, n, seq) && m_hwnd)
			PostMessageW(m_hwnd, WMU_REVOKE, static_cast<WPARAM>(seq), 0);
		break;
	}
	case kRead: {
		ReadJob job;
		if (!DecodeRead(p, n, job.read))
			return;
		{
			std::lock_guard<std::mutex> lk(m_jobMutex);
			m_jobs.push_back(job);
		}
		m_jobCond.notify_one();
		break;
	}
	case kCancel: {
		ReadJob job;
		job.cancel = true;
		if (!DecodeCancel(p, n, job.streamId))
			return;
		{
			std::lock_guard<std::mutex> lk(m_jobMutex);
			m_cancelled.insert(job.streamId);
			m_jobs.push_back(job);
		}
		m_jobCond.notify_one();
		break;
	}
	case kData: {
		DataHeader h;
		if (!DecodeDataHeader(p, n, h))
			return;
		std::lock_guard<std::mutex> lk(m_streamMutex);
		auto it = m_streams.find(h.streamId);
		if (it != m_streams.end())
			it->second->Deliver(h.streamId, h.offset, h.flags, p + kDataHeaderSize, n - kDataHeaderSize);
		break;
	}
	case kError: {
		Error e;
		if (!DecodeError(p, n, e))
			return;
		Log(0, "file clipboard: peer reports error " + std::to_string(e.code) +
			" on stream " + std::to_string(e.streamId));
		std::lock_guard<std::mutex> lk(m_streamMutex);
		auto it = m_streams.find(e.streamId);
		if (it != m_streams.end())
			it->second->Fail(e.streamId);
		break;
	}
	default:
		break; // from a newer peer
	}
}

// --- target stream registry ------------------------------------------------

uint32_t FileClipboard::NewStreamId()
{
	uint32_t id;
	do {
		id = m_nextStreamId++;
	} while (id == 0);
	return id;
}

void FileClipboard::RegisterStream(uint32_t id, RemoteStream* s)
{
	std::lock_guard<std::mutex> lk(m_streamMutex);
	m_streams[id] = s;
}

void FileClipboard::UnregisterStream(uint32_t id)
{
	std::lock_guard<std::mutex> lk(m_streamMutex);
	m_streams.erase(id);
}

void FileClipboard::FailAllStreams()
{
	std::lock_guard<std::mutex> lk(m_streamMutex);
	for (auto& kv : m_streams)
		kv.second->Fail(0);
}

// --- clipboard thread --------------------------------------------------------

LRESULT CALLBACK FileClipboard::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	FileClipboard* self = reinterpret_cast<FileClipboard*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (self)
		return self->HandleWindowMessage(hwnd, msg, wp, lp);
	return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT FileClipboard::HandleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_CLIPBOARDUPDATE) {
		OnLocalClipboardChanged(false);
		return 0;
	}
	if (msg == WM_TIMER && wp == kPollTimerId) {
		if (GetClipboardSequenceNumber() != m_lastClipSeq) {
			Log(2, "file clipboard: change found by polling");
			OnLocalClipboardChanged(false);
		}
		return 0;
	}
	if (msg == WMU_CHECK) {
		OnLocalClipboardChanged(true);
		return 0;
	}
	if (msg == WMU_OFFER) {
		ApplyRemoteOffer(reinterpret_cast<RemoteOffer*>(lp));
		return 0;
	}
	if (msg == WMU_REVOKE) {
		ApplyRemoteRevoke(static_cast<uint32_t>(wp));
		return 0;
	}
	if (msg == WMU_DISCONNECT) {
		RemoveRemoteObject();
		return 0;
	}
	if (msg == WMU_QUIT) {
		PostQuitMessage(0);
		return 0;
	}
	// m_hwnd is not set yet while the window is being created.
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void FileClipboard::ClipThreadMain()
{
	m_comOk = SUCCEEDED(OleInitialize(NULL));
	if (!m_comOk)
		Log(0, "file clipboard: OleInitialize failed");

	static std::once_flag classOnce;
	std::call_once(classOnce, [] {
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &FileClipboard::WndProc;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.lpszClassName = kWindowClass;
		RegisterClassExW(&wc);
	});

	if (m_comOk) {
		m_hwnd = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
			GetModuleHandleW(NULL), this);
		if (!m_hwnd) {
			Log(0, "file clipboard: CreateWindowEx failed, error " + std::to_string(GetLastError()));
		} else {
			if (m_cb.useClipboardListener && !AddClipboardFormatListener(m_hwnd))
				Log(0, "file clipboard: AddClipboardFormatListener failed, error " + std::to_string(GetLastError()));
			m_lastClipSeq = GetClipboardSequenceNumber();
			if (!SetTimer(m_hwnd, kPollTimerId, kPollIntervalMs, NULL))
				Log(0, "file clipboard: SetTimer failed, error " + std::to_string(GetLastError()));
		}
	}
	if (m_readyEvent)
		SetEvent(m_readyEvent);

	if (m_hwnd) {
		MSG msg;
		while (GetMessageW(&msg, NULL, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		// Free offers that arrived after the quit request.
		while (PeekMessageW(&msg, m_hwnd, WMU_OFFER, WMU_OFFER, PM_REMOVE))
			delete reinterpret_cast<RemoteOffer*>(msg.lParam);

		RemoveRemoteObject();
		KillTimer(m_hwnd, kPollTimerId);
		RemoveClipboardFormatListener(m_hwnd);
		HWND h = m_hwnd;
		m_hwnd = NULL;
		DestroyWindow(h);
	}
	if (m_comOk)
		OleUninitialize();
}

void FileClipboard::ApplyRemoteOffer(RemoteOffer* raw)
{
	std::shared_ptr<RemoteOffer> offer(raw);
	if (!m_connected || m_stopping)
		return;
	if (offer->entries.empty()) {
		RemoveRemoteObject();
		return;
	}
	RemoteDataObject* obj = new RemoteDataObject(this, offer);
	HRESULT hr = OleSetClipboard(obj);
	if (FAILED(hr)) {
		char buf[16];
		snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(hr));
		Log(0, std::string("file clipboard: OleSetClipboard failed, hr ") + buf);
		obj->Release();
		return;
	}
	if (m_remoteObject)
		m_remoteObject->Release(); // OleSetClipboard already replaced it
	m_remoteObject = obj;
	m_remoteSeq = offer->seq;
	Log(1, "file clipboard: " + std::to_string(offer->entries.size()) +
		" entries from the peer are on the clipboard, ready to paste");
}

void FileClipboard::ApplyRemoteRevoke(uint32_t seq)
{
	if (m_remoteObject && seq == m_remoteSeq)
		RemoveRemoteObject();
}

void FileClipboard::RemoveRemoteObject()
{
	if (!m_remoteObject)
		return;
	// Only clear the clipboard if it still holds our files; the user may
	// have copied something else meanwhile.
	if (OleIsCurrentClipboard(m_remoteObject) == S_OK)
		OleSetClipboard(NULL);
	m_remoteObject->Release();
	m_remoteObject = nullptr;
	m_remoteSeq = 0;
}

void FileClipboard::OnLocalClipboardChanged(bool force)
{
	// The listener and the poll timer can both report the same change.
	const DWORD clipSeq = GetClipboardSequenceNumber();
	if (!force && clipSeq == m_lastClipSeq)
		return;
	m_lastClipSeq = clipSeq;

	if (!m_connected || m_stopping)
		return;
	// Virtual files put there by UltraVNC (this or another connection) are
	// never offered back.
	if (IsClipboardFormatAvailable(Fmt().marker)) {
		Log(2, "file clipboard: clipboard holds UltraVNC virtual files, not offered");
		return;
	}
	if (!IsClipboardFormatAvailable(CF_HDROP)) {
		Log(2, "file clipboard: clipboard changed, no files on it");
		RevokeLocalOffer();
		return;
	}
	std::vector<std::wstring> paths;
	if (!ReadClipboardFiles(paths) || paths.empty()) {
		Log(0, "file clipboard: clipboard has files but the list could not be read");
		return;
	}
	Log(1, "file clipboard: clipboard has " + std::to_string(paths.size()) + " file(s)/folder(s)");

	Offer offer;
	std::vector<LocalFile> files;
	if (!BuildOffer(paths, offer, files)) {
		RevokeLocalOffer();
		return;
	}
	{
		std::lock_guard<std::mutex> lk(m_srcMutex);
		offer.seq = m_nextLocalSeq++;
		m_localSeq = offer.seq;
		m_localFiles.swap(files);
	}
	if (Send(kPrioOffer, kOffer, EncodeOffer(offer)))
		Log(1, "file clipboard: offered " + std::to_string(offer.entries.size()) + " entries");
	else
		Log(0, "file clipboard: offer could not be sent (too large or channel closed)");
}

void FileClipboard::RevokeLocalOffer()
{
	uint32_t seq = 0;
	{
		std::lock_guard<std::mutex> lk(m_srcMutex);
		seq = m_localSeq;
		m_localSeq = 0;
		m_localFiles.clear();
	}
	if (seq != 0)
		Send(kPrioRevoke, kRevoke, EncodeRevoke(seq));
}

bool FileClipboard::ReadClipboardFiles(std::vector<std::wstring>& paths)
{
	bool open = false;
	// Other programs (and UltraVNC's own text clipboard code) react to the
	// same change; give them up to about a second.
	for (int i = 0; i < 50 && !open; ++i) {
		open = OpenClipboard(m_hwnd) != FALSE;
		if (!open)
			Sleep(20);
	}
	if (!open) {
		Log(0, "file clipboard: could not open the clipboard, error " + std::to_string(GetLastError()));
		return false;
	}
	HDROP drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
	if (drop) {
		const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
		for (UINT i = 0; i < count; ++i) {
			const UINT len = DragQueryFileW(drop, i, NULL, 0);
			if (len == 0)
				continue;
			std::wstring s(len + 1, L'\0');
			DragQueryFileW(drop, i, &s[0], len + 1);
			s.resize(len);
			paths.push_back(s);
		}
	}
	CloseClipboard();
	return drop != NULL;
}

bool FileClipboard::BuildOffer(const std::vector<std::wstring>& paths, Offer& offer,
                               std::vector<LocalFile>& files)
{
	HANDLE token = NULL;
	if (m_cb.acquireUserToken && !m_cb.acquireUserToken(&token)) {
		Log(0, "file clipboard: no user token (no user logged on?), files not offered");
		return false;
	}
	bool overflow = false;
	{
		Impersonation imp(token);
		if (imp.Failed(token)) {
			CloseHandle(token);
			Log(0, "file clipboard: impersonation failed, error " + std::to_string(GetLastError()) + ", files not offered");
			return false;
		}

		struct Walker {
			Offer& offer;
			std::vector<LocalFile>& files;
			bool& overflow;

			void Add(const std::wstring& abs, const std::wstring& rel, DWORD attrs,
			         uint64_t size, const FILETIME& mtime, int depth)
			{
				if (overflow)
					return;
				if (offer.entries.size() >= kMaxEntries) {
					overflow = true;
					return;
				}
				const bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
				Entry e;
				std::wstring wire = rel;
				std::replace(wire.begin(), wire.end(), L'\\', L'/');
				e.path = WideToUtf8(wire);
				e.attributes = attrs & kAttrMask;
				e.size = dir ? 0 : size;
				e.mtime = FileTimeToU64(mtime);
				offer.entries.push_back(std::move(e));
				LocalFile lf;
				lf.path = abs;
				lf.directory = dir;
				files.push_back(std::move(lf));

				// Do not follow junctions and symbolic links to directories:
				// they can loop or leave the copied tree.
				if (!dir || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) || depth >= kMaxDepth)
					return;
				WIN32_FIND_DATAW fd;
				HANDLE h = FindFirstFileExW(LongPath(abs + L"\\*").c_str(), FindExInfoBasic, &fd,
					FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
				if (h == INVALID_HANDLE_VALUE)
					return;
				do {
					const std::wstring name = fd.cFileName;
					if (name == L"." || name == L"..")
						continue;
					const uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
					Add(abs + L"\\" + name, rel + L"\\" + name, fd.dwFileAttributes, sz,
						fd.ftLastWriteTime, depth + 1);
				} while (!overflow && FindNextFileW(h, &fd));
				FindClose(h);
			}
		} walker{ offer, files, overflow };

		for (std::wstring p : paths) {
			while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
				p.pop_back();
			const size_t slash = p.find_last_of(L"\\/");
			const std::wstring name = slash == std::wstring::npos ? p : p.substr(slash + 1);
			if (name.empty() || name.find(L':') != std::wstring::npos)
				continue; // a drive root such as D:\ cannot be offered
			WIN32_FILE_ATTRIBUTE_DATA fad;
			if (!GetFileAttributesExW(LongPath(p).c_str(), GetFileExInfoStandard, &fad)) {
				Log(0, "file clipboard: cannot access " + WideToUtf8(p) + ", error " +
					std::to_string(GetLastError()));
				continue;
			}
			const uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
			walker.Add(p, name, fad.dwFileAttributes, sz, fad.ftLastWriteTime, 0);
		}
	}
	if (token)
		CloseHandle(token);

	if (overflow) {
		Log(0, "file clipboard: more than " + std::to_string(kMaxEntries) + " files, not offered");
		return false;
	}
	return !offer.entries.empty();
}

// --- worker (source role) ----------------------------------------------------

void FileClipboard::WorkerMain()
{
	for (;;) {
		ReadJob job;
		{
			std::unique_lock<std::mutex> lk(m_jobMutex);
			m_jobCond.wait(lk, [&] { return m_workerStop || m_dropOpenFiles || !m_jobs.empty(); });
			if (m_workerStop)
				break;
			if (m_dropOpenFiles) {
				m_dropOpenFiles = false;
				lk.unlock();
				CloseAllStreams();
				continue;
			}
			job = m_jobs.front();
			m_jobs.pop_front();
		}
		if (job.cancel)
			CloseStream(job.streamId);
		else
			ServeRead(job.read);
	}
	CloseAllStreams();
}

void FileClipboard::CloseStream(uint32_t streamId)
{
	auto it = m_open.find(streamId);
	if (it != m_open.end()) {
		CloseHandle(it->second.handle);
		m_open.erase(it);
	}
	std::lock_guard<std::mutex> lk(m_jobMutex);
	// Cancel is processed after every read queued before it, so the id can
	// be forgotten now.
	m_cancelled.erase(streamId);
}

void FileClipboard::CloseAllStreams()
{
	for (auto& kv : m_open)
		CloseHandle(kv.second.handle);
	m_open.clear();
}

void FileClipboard::ServeRead(const Read& rd)
{
	const uint32_t id = rd.streamId;
	auto cancelled = [&] {
		std::lock_guard<std::mutex> lk(m_jobMutex);
		return m_workerStop || m_dropOpenFiles || m_cancelled.count(id) > 0;
	};
	auto fail = [&](uint32_t code) {
		Error e;
		e.streamId = id;
		e.code = code;
		Send(kPrioError, kError, EncodeError(e));
		auto it = m_open.find(id);
		if (it != m_open.end()) {
			CloseHandle(it->second.handle);
			m_open.erase(it);
		}
	};
	if (cancelled())
		return;

	auto it = m_open.find(id);
	if (it == m_open.end()) {
		std::wstring path;
		uint32_t err = 0;
		{
			std::lock_guard<std::mutex> lk(m_srcMutex);
			if (m_localSeq == 0 || rd.seq != m_localSeq) {
				err = kErrStaleOffer;
				Log(0, "file clipboard: read for offer " + std::to_string(rd.seq) +
					" but the current offer is " + std::to_string(m_localSeq));
			}
			else if (rd.fileId >= m_localFiles.size() || m_localFiles[rd.fileId].directory)
				err = kErrBadFile;
			else
				path = m_localFiles[rd.fileId].path;
		}
		if (err == 0 && m_open.size() >= kMaxOpenFiles)
			err = kErrBusy;
		if (err != 0) {
			fail(err);
			return;
		}
		HANDLE token = NULL;
		if (m_cb.acquireUserToken && !m_cb.acquireUserToken(&token)) {
			Log(0, "file clipboard: no user token, cannot open " + WideToUtf8(path));
			fail(kErrOpen);
			return;
		}
		HANDLE h = INVALID_HANDLE_VALUE;
		DWORD openError = 0;
		{
			Impersonation imp(token);
			if (imp.Failed(token)) {
				openError = GetLastError();
			} else {
				h = CreateFileW(LongPath(path).c_str(), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
					FILE_FLAG_SEQUENTIAL_SCAN, NULL);
				if (h == INVALID_HANDLE_VALUE)
					openError = GetLastError();
			}
		}
		if (token)
			CloseHandle(token);
		if (h == INVALID_HANDLE_VALUE) {
			Log(0, "file clipboard: cannot open " + WideToUtf8(path) + ", error " + std::to_string(openError));
			fail(kErrOpen);
			return;
		}
		Log(2, "file clipboard: serving " + WideToUtf8(path));
		OpenFile of;
		of.handle = h;
		it = m_open.emplace(id, of).first;
	}

	HANDLE h = it->second.handle;
	LARGE_INTEGER li;
	li.QuadPart = static_cast<LONGLONG>(rd.offset);
	if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
		fail(kErrRead);
		return;
	}
	std::vector<uint8_t> buf(kDataPieceBytes);
	uint64_t pos = rd.offset;
	uint32_t remaining = rd.length;
	while (remaining > 0) {
		if (cancelled())
			return;
		const DWORD want = static_cast<DWORD>(std::min<size_t>(remaining, buf.size()));
		DWORD got = 0;
		if (!ReadFile(h, buf.data(), want, &got, NULL)) {
			Log(0, "file clipboard: read failed, error " + std::to_string(GetLastError()));
			fail(kErrRead);
			return;
		}
		DataHeader dh;
		dh.streamId = id;
		dh.offset = pos;
		dh.flags = got == 0 ? kDataEof : 0;
		if (!Send(kPrioData, kData, EncodeData(dh, buf.data(), got)))
			return; // channel closed; OnDisconnected cleans up
		if (got == 0) {
			// End of file: the target needs nothing more from this handle.
			CloseHandle(h);
			m_open.erase(id);
			return;
		}
		pos += got;
		remaining -= got;
	}
}

// --- COM security ------------------------------------------------------------

bool FileClipboard::InitComSecurityForService(std::string* error)
{
	// Everyone may call (execute + execute local); callers below medium
	// integrity are refused by the mandatory label.
	PSECURITY_DESCRIPTOR rel = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			L"O:BAG:BAD:(A;;0x3;;;WD)S:(ML;;NX;;;ME)", SDDL_REVISION_1, &rel, NULL)) {
		if (error)
			*error = "ConvertStringSecurityDescriptorToSecurityDescriptor failed";
		return false;
	}
	// CoInitializeSecurity wants an absolute security descriptor. The parts
	// must stay allocated for the lifetime of the process.
	DWORD absSize = 0, daclSize = 0, saclSize = 0, ownerSize = 0, groupSize = 0;
	MakeAbsoluteSD(rel, NULL, &absSize, NULL, &daclSize, NULL, &saclSize, NULL, &ownerSize, NULL, &groupSize);
	PSECURITY_DESCRIPTOR abs = LocalAlloc(LPTR, absSize);
	PACL dacl = static_cast<PACL>(LocalAlloc(LPTR, daclSize ? daclSize : 1));
	PACL sacl = static_cast<PACL>(LocalAlloc(LPTR, saclSize ? saclSize : 1));
	PSID owner = LocalAlloc(LPTR, ownerSize ? ownerSize : 1);
	PSID group = LocalAlloc(LPTR, groupSize ? groupSize : 1);
	const BOOL ok = abs && dacl && sacl && owner && group &&
		MakeAbsoluteSD(rel, abs, &absSize, dacl, &daclSize, sacl, &saclSize, owner, &ownerSize, group, &groupSize);
	LocalFree(rel);
	if (!ok) {
		if (error)
			*error = "MakeAbsoluteSD failed";
		return false;
	}
	const HRESULT hr = CoInitializeSecurity(abs, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
		RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
	if (FAILED(hr)) {
		if (error)
			*error = hr == RPC_E_TOO_LATE ? "CoInitializeSecurity: too late" : "CoInitializeSecurity failed";
		return false;
	}
	return true;
}

} // namespace fileclip

#endif // _WIN32
