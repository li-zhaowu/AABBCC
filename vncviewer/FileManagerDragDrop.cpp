// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "FileManagerDragDrop.h"

#include <algorithm>
#include <cstring>

#include "common/FileManServer.h" // Utf8ToWide

namespace fmdnd {

namespace {

HGLOBAL CopyGlobal(HGLOBAL src)
{
	const SIZE_T n = GlobalSize(src);
	HGLOBAL dst = GlobalAlloc(GMEM_MOVEABLE, n);
	if (!dst)
		return NULL;
	void* s = GlobalLock(src);
	void* d = GlobalLock(dst);
	if (s && d)
		memcpy(d, s, n);
	if (d)
		GlobalUnlock(dst);
	if (s)
		GlobalUnlock(src);
	return dst;
}

} // namespace

// --- DataObject -----------------------------------------------------------------

DataObject::~DataObject()
{
	for (auto& it : m_items)
		GlobalFree(it.second);
}

void DataObject::Add(CLIPFORMAT cf, HGLOBAL h)
{
	if (h)
		m_items.emplace_back(cf, h);
}

STDMETHODIMP DataObject::QueryInterface(REFIID riid, void** ppv)
{
	if (riid == IID_IUnknown || riid == IID_IDataObject) {
		*ppv = static_cast<IDataObject*>(this);
		AddRef();
		return S_OK;
	}
	*ppv = nullptr;
	return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DataObject::AddRef()
{
	return InterlockedIncrement(&m_refs);
}

STDMETHODIMP_(ULONG) DataObject::Release()
{
	const LONG n = InterlockedDecrement(&m_refs);
	if (n == 0)
		delete this;
	return n;
}

STDMETHODIMP DataObject::GetData(FORMATETC* fmt, STGMEDIUM* medium)
{
	if (!fmt || !medium)
		return E_INVALIDARG;
	if (!(fmt->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	for (auto& it : m_items) {
		if (it.first != fmt->cfFormat)
			continue;
		medium->tymed = TYMED_HGLOBAL;
		medium->hGlobal = CopyGlobal(it.second);
		medium->pUnkForRelease = nullptr;
		return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
	}
	return DV_E_FORMATETC;
}

STDMETHODIMP DataObject::QueryGetData(FORMATETC* fmt)
{
	if (!fmt)
		return E_INVALIDARG;
	if (!(fmt->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	for (auto& it : m_items)
		if (it.first == fmt->cfFormat)
			return S_OK;
	return DV_E_FORMATETC;
}

STDMETHODIMP DataObject::GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out)
{
	if (out)
		out->ptd = nullptr;
	return E_NOTIMPL;
}

// The shell stores things like the drag image or "performed drop effect"
// here; keep HGLOBAL ones, refuse the rest.
STDMETHODIMP DataObject::SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release)
{
	if (!fmt || !medium || medium->tymed != TYMED_HGLOBAL)
		return E_NOTIMPL;
	HGLOBAL h = release ? medium->hGlobal : CopyGlobal(medium->hGlobal);
	if (!h)
		return E_OUTOFMEMORY;
	for (auto& it : m_items) {
		if (it.first == fmt->cfFormat) {
			GlobalFree(it.second);
			it.second = h;
			return S_OK;
		}
	}
	m_items.emplace_back(fmt->cfFormat, h);
	return S_OK;
}

STDMETHODIMP DataObject::EnumFormatEtc(DWORD direction, IEnumFORMATETC** e)
{
	if (direction != DATADIR_GET)
		return E_NOTIMPL;
	std::vector<FORMATETC> fmts;
	for (auto& it : m_items) {
		FORMATETC f = { it.first, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
		fmts.push_back(f);
	}
	return SHCreateStdEnumFmtEtc(static_cast<UINT>(fmts.size()), fmts.data(), e);
}

// --- DropSource -----------------------------------------------------------------

STDMETHODIMP DropSource::QueryInterface(REFIID riid, void** ppv)
{
	if (riid == IID_IUnknown || riid == IID_IDropSource) {
		*ppv = static_cast<IDropSource*>(this);
		AddRef();
		return S_OK;
	}
	*ppv = nullptr;
	return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DropSource::AddRef()
{
	return InterlockedIncrement(&m_refs);
}

STDMETHODIMP_(ULONG) DropSource::Release()
{
	const LONG n = InterlockedDecrement(&m_refs);
	if (n == 0)
		delete this;
	return n;
}

STDMETHODIMP DropSource::QueryContinueDrag(BOOL escape, DWORD keys)
{
	if (escape)
		return DRAGDROP_S_CANCEL;
	if (!(keys & (MK_LBUTTON | MK_RBUTTON)))
		return DRAGDROP_S_DROP;
	return S_OK;
}

// --- DropTarget -----------------------------------------------------------------

DropTarget::~DropTarget()
{
	if (m_data)
		m_data->Release();
}

STDMETHODIMP DropTarget::QueryInterface(REFIID riid, void** ppv)
{
	if (riid == IID_IUnknown || riid == IID_IDropTarget) {
		*ppv = static_cast<IDropTarget*>(this);
		AddRef();
		return S_OK;
	}
	*ppv = nullptr;
	return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) DropTarget::AddRef()
{
	return InterlockedIncrement(&m_refs);
}

STDMETHODIMP_(ULONG) DropTarget::Release()
{
	const LONG n = InterlockedDecrement(&m_refs);
	if (n == 0)
		delete this;
	return n;
}

STDMETHODIMP DropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
	if (m_data)
		m_data->Release();
	m_data = data;
	if (m_data)
		m_data->AddRef();
	*effect &= m_handler(m_data, pt, keys, false);
	return S_OK;
}

STDMETHODIMP DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect)
{
	*effect &= m_handler(m_data, pt, keys, false);
	return S_OK;
}

STDMETHODIMP DropTarget::DragLeave()
{
	POINTL pt = { 0, 0 };
	m_handler(nullptr, pt, 0, false);
	if (m_data) {
		m_data->Release();
		m_data = nullptr;
	}
	return S_OK;
}

STDMETHODIMP DropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect)
{
	*effect &= m_handler(data, pt, keys, true);
	POINTL none = { 0, 0 };
	m_handler(nullptr, none, 0, false);
	if (m_data) {
		m_data->Release();
		m_data = nullptr;
	}
	return S_OK;
}

// --- remote files dragged to Explorer ------------------------------------------------

namespace {

const UINT kCfDescriptor = RegisterClipboardFormatW(L"FileGroupDescriptorW");
const UINT kCfContents = RegisterClipboardFormatW(L"FileContents");
const UINT kCfPreferredEffect = RegisterClipboardFormatW(L"Preferred DropEffect");
constexpr size_t kMaxVirtualItems = 50000;
constexpr int kMaxDepth = 64;

} // namespace

void RemoteSession::Shutdown()
{
	std::vector<RemoteFileStream*> streams;
	streams.swap(m_streams);
	for (RemoteFileStream* s : streams)
		s->Abandon();
	m_client = nullptr;
}

void RemoteSession::Add(RemoteFileStream* s)
{
	m_streams.push_back(s);
}

void RemoteSession::Remove(RemoteFileStream* s)
{
	m_streams.erase(std::remove(m_streams.begin(), m_streams.end(), s), m_streams.end());
}

RemoteFileStream::RemoteFileStream(std::shared_ptr<RemoteSession> session, std::unique_ptr<fileman::RemoteReader> reader,
                                   const std::wstring& name)
	: m_session(std::move(session)), m_reader(std::move(reader)), m_name(name)
{
	m_size = m_reader->Size();
	m_mtime = m_reader->MTime();
	m_session->Add(this);
}

RemoteFileStream::~RemoteFileStream()
{
	m_session->Remove(this);
	m_reader.reset(); // closes the remote file (if the session is still there)
}

void RemoteFileStream::Abandon()
{
	m_reader.reset();
}

STDMETHODIMP RemoteFileStream::QueryInterface(REFIID riid, void** ppv)
{
	if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream) {
		*ppv = static_cast<IStream*>(this);
		AddRef();
		return S_OK;
	}
	*ppv = nullptr;
	return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) RemoteFileStream::AddRef()
{
	return InterlockedIncrement(&m_refs);
}

STDMETHODIMP_(ULONG) RemoteFileStream::Release()
{
	const LONG n = InterlockedDecrement(&m_refs);
	if (n == 0)
		delete this;
	return n;
}

STDMETHODIMP RemoteFileStream::Read(void* pv, ULONG cb, ULONG* pcbRead)
{
	if (pcbRead)
		*pcbRead = 0;
	if (!m_reader)
		return STG_E_READFAULT; // the connection or the window is gone
	size_t got = 0;
	const fileman::Result r = m_reader->Read(pv, cb, got);
	if (pcbRead)
		*pcbRead = static_cast<ULONG>(got);
	if (!r.Ok())
		return STG_E_READFAULT;
	return got == cb ? S_OK : S_FALSE;
}

// Forward only; Explorer asks for the current position or the start.
STDMETHODIMP RemoteFileStream::Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos)
{
	const uint64_t pos = m_reader ? m_reader->Position() : 0;
	int64_t target;
	switch (origin) {
	case STREAM_SEEK_SET: target = move.QuadPart; break;
	case STREAM_SEEK_CUR: target = static_cast<int64_t>(pos) + move.QuadPart; break;
	case STREAM_SEEK_END: target = static_cast<int64_t>(m_size) + move.QuadPart; break;
	default: return STG_E_INVALIDFUNCTION;
	}
	if (target != static_cast<int64_t>(pos))
		return STG_E_INVALIDFUNCTION;
	if (newPos)
		newPos->QuadPart = pos;
	return S_OK;
}

STDMETHODIMP RemoteFileStream::Stat(STATSTG* st, DWORD flags)
{
	if (!st)
		return STG_E_INVALIDPOINTER;
	memset(st, 0, sizeof(*st));
	st->type = STGTY_STREAM;
	st->cbSize.QuadPart = m_size;
	st->mtime.dwLowDateTime = static_cast<DWORD>(m_mtime);
	st->mtime.dwHighDateTime = static_cast<DWORD>(m_mtime >> 32);
	st->grfMode = STGM_READ;
	if (!(flags & STATFLAG_NONAME)) {
		const size_t bytes = (m_name.size() + 1) * sizeof(wchar_t);
		st->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
		if (!st->pwcsName)
			return E_OUTOFMEMORY;
		memcpy(st->pwcsName, m_name.c_str(), bytes);
	}
	return S_OK;
}

VirtualDataObject::VirtualDataObject(std::shared_ptr<RemoteSession> session, std::vector<Source> sources)
	: m_session(std::move(session)), m_sources(std::move(sources))
{
}

VirtualDataObject::~VirtualDataObject()
{
	for (auto& it : m_extra)
		GlobalFree(it.second);
}

void VirtualDataObject::Add(CLIPFORMAT cf, HGLOBAL h)
{
	if (h)
		m_extra.emplace_back(cf, h);
}

STDMETHODIMP VirtualDataObject::QueryInterface(REFIID riid, void** ppv)
{
	if (riid == IID_IUnknown || riid == IID_IDataObject) {
		*ppv = static_cast<IDataObject*>(this);
	} else if (riid == __uuidof(IDataObjectAsyncCapability)) {
		*ppv = static_cast<IDataObjectAsyncCapability*>(this);
	} else {
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) VirtualDataObject::AddRef()
{
	return InterlockedIncrement(&m_refs);
}

STDMETHODIMP_(ULONG) VirtualDataObject::Release()
{
	const LONG n = InterlockedDecrement(&m_refs);
	if (n == 0)
		delete this;
	return n;
}

bool VirtualDataObject::ExpandFolder(const std::string& remote, const std::wstring& relative, int depth)
{
	fileman::FileManClient* client = m_session->Client();
	if (!client || depth > kMaxDepth || m_items.size() >= kMaxVirtualItems)
		return false;
	std::vector<fileman::Entry> entries;
	if (!client->ListSync(remote, entries).Ok())
		return false;
	for (const fileman::Entry& e : entries) {
		if (!fileman::IsValidName(e.name))
			continue;
		Item it;
		it.relative = relative + L"\\" + fileman::Utf8ToWide(e.name);
		it.remote = fileman::JoinPath(remote, e.name);
		it.directory = e.IsDirectory();
		it.size = e.size;
		it.mtime = e.mtime;
		if (it.relative.size() >= MAX_PATH)
			continue; // does not fit FILEDESCRIPTORW
		m_items.push_back(it);
		if (it.directory && !ExpandFolder(it.remote, it.relative, depth + 1))
			return false;
	}
	return true;
}

bool VirtualDataObject::Expand()
{
	if (m_expanded)
		return !m_expandFailed;
	m_expanded = true;
	HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
	for (const Source& s : m_sources) {
		Item it;
		it.relative = fileman::Utf8ToWide(fileman::FileNamePart(s.path));
		it.remote = s.path;
		it.directory = s.directory;
		it.size = s.size;
		it.mtime = s.mtime;
		m_items.push_back(it);
		if (s.directory && !ExpandFolder(s.path, it.relative, 0)) {
			m_expandFailed = true;
			break;
		}
	}
	SetCursor(old);
	return !m_expandFailed;
}

STDMETHODIMP VirtualDataObject::GetData(FORMATETC* fmt, STGMEDIUM* medium)
{
	if (!fmt || !medium)
		return E_INVALIDARG;
	memset(medium, 0, sizeof(*medium));
	if (fmt->cfFormat == kCfDescriptor && (fmt->tymed & TYMED_HGLOBAL)) {
		if (!Expand())
			return E_FAIL;
		const size_t bytes = sizeof(FILEGROUPDESCRIPTORW) + (m_items.empty() ? 0 : m_items.size() - 1) * sizeof(FILEDESCRIPTORW);
		HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
		if (!h)
			return E_OUTOFMEMORY;
		auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(h));
		group->cItems = static_cast<UINT>(m_items.size());
		for (size_t i = 0; i < m_items.size(); ++i) {
			FILEDESCRIPTORW& fd = group->fgd[i];
			const Item& it = m_items[i];
			fd.dwFlags = FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI | (it.directory ? 0 : FD_FILESIZE | FD_WRITESTIME);
			fd.dwFileAttributes = it.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
			fd.nFileSizeLow = static_cast<DWORD>(it.size);
			fd.nFileSizeHigh = static_cast<DWORD>(it.size >> 32);
			fd.ftLastWriteTime.dwLowDateTime = static_cast<DWORD>(it.mtime);
			fd.ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(it.mtime >> 32);
			wcsncpy_s(fd.cFileName, it.relative.c_str(), _TRUNCATE);
		}
		GlobalUnlock(h);
		medium->tymed = TYMED_HGLOBAL;
		medium->hGlobal = h;
		return S_OK;
	}
	if (fmt->cfFormat == kCfContents && (fmt->tymed & TYMED_ISTREAM)) {
		if (!Expand() || fmt->lindex < 0 || static_cast<size_t>(fmt->lindex) >= m_items.size() ||
		    m_items[static_cast<size_t>(fmt->lindex)].directory)
			return DV_E_LINDEX;
		fileman::FileManClient* client = m_session->Client();
		if (!client)
			return E_FAIL;
		const Item& it = m_items[static_cast<size_t>(fmt->lindex)];
		fileman::Result r;
		std::unique_ptr<fileman::RemoteReader> reader = client->OpenReader(it.remote, r);
		if (!reader)
			return E_FAIL;
		const size_t slash = it.relative.find_last_of(L'\\');
		auto* stream = new RemoteFileStream(m_session, std::move(reader),
			slash == std::wstring::npos ? it.relative : it.relative.substr(slash + 1));
		medium->tymed = TYMED_ISTREAM;
		medium->pstm = stream;
		return S_OK;
	}
	if (fmt->cfFormat == kCfPreferredEffect && (fmt->tymed & TYMED_HGLOBAL)) {
		const DWORD effect = DROPEFFECT_COPY;
		medium->tymed = TYMED_HGLOBAL;
		medium->hGlobal = MakeBytes(&effect, sizeof(effect));
		return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
	}
	if (fmt->tymed & TYMED_HGLOBAL) {
		for (auto& it : m_extra) {
			if (it.first != fmt->cfFormat)
				continue;
			medium->tymed = TYMED_HGLOBAL;
			medium->hGlobal = CopyGlobal(it.second);
			return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
		}
	}
	return DV_E_FORMATETC;
}

STDMETHODIMP VirtualDataObject::QueryGetData(FORMATETC* fmt)
{
	if (!fmt)
		return E_INVALIDARG;
	if (fmt->cfFormat == kCfContents)
		return (fmt->tymed & TYMED_ISTREAM) ? S_OK : DV_E_TYMED;
	if (!(fmt->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	if (fmt->cfFormat == kCfDescriptor || fmt->cfFormat == kCfPreferredEffect)
		return S_OK;
	for (auto& it : m_extra)
		if (it.first == fmt->cfFormat)
			return S_OK;
	return DV_E_FORMATETC;
}

STDMETHODIMP VirtualDataObject::GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out)
{
	if (out)
		out->ptd = nullptr;
	return E_NOTIMPL;
}

STDMETHODIMP VirtualDataObject::SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release)
{
	if (!fmt || !medium || medium->tymed != TYMED_HGLOBAL)
		return E_NOTIMPL;
	HGLOBAL h = release ? medium->hGlobal : CopyGlobal(medium->hGlobal);
	if (!h)
		return E_OUTOFMEMORY;
	for (auto& it : m_extra) {
		if (it.first == fmt->cfFormat) {
			GlobalFree(it.second);
			it.second = h;
			return S_OK;
		}
	}
	m_extra.emplace_back(fmt->cfFormat, h);
	return S_OK;
}

STDMETHODIMP VirtualDataObject::EnumFormatEtc(DWORD direction, IEnumFORMATETC** e)
{
	if (direction != DATADIR_GET)
		return E_NOTIMPL;
	std::vector<FORMATETC> fmts;
	fmts.push_back({ static_cast<CLIPFORMAT>(kCfDescriptor), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL });
	fmts.push_back({ static_cast<CLIPFORMAT>(kCfContents), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM });
	fmts.push_back({ static_cast<CLIPFORMAT>(kCfPreferredEffect), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL });
	for (auto& it : m_extra)
		fmts.push_back({ it.first, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL });
	return SHCreateStdEnumFmtEtc(static_cast<UINT>(fmts.size()), fmts.data(), e);
}

// --- helpers ---------------------------------------------------------------------

HGLOBAL MakeHDrop(const std::vector<std::wstring>& paths)
{
	size_t chars = 1;
	for (const auto& p : paths)
		chars += p.size() + 1;
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
	if (!h)
		return NULL;
	auto* df = static_cast<DROPFILES*>(GlobalLock(h));
	df->pFiles = sizeof(DROPFILES);
	df->fWide = TRUE;
	wchar_t* out = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
	for (const auto& p : paths) {
		memcpy(out, p.c_str(), (p.size() + 1) * sizeof(wchar_t));
		out += p.size() + 1;
	}
	*out = 0;
	GlobalUnlock(h);
	return h;
}

std::vector<std::wstring> ReadHDrop(IDataObject* data)
{
	std::vector<std::wstring> out;
	FORMATETC fmt = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM medium = {};
	if (!data || FAILED(data->GetData(&fmt, &medium)))
		return out;
	HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
	if (drop) {
		const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		for (UINT i = 0; i < n; ++i) {
			const UINT len = DragQueryFileW(drop, i, nullptr, 0);
			std::wstring p(len, L'\0');
			DragQueryFileW(drop, i, &p[0], len + 1);
			out.push_back(p);
		}
		GlobalUnlock(medium.hGlobal);
	}
	ReleaseStgMedium(&medium);
	return out;
}

bool HasFormat(IDataObject* data, CLIPFORMAT cf)
{
	FORMATETC fmt = { cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	return data && data->QueryGetData(&fmt) == S_OK;
}

std::vector<unsigned char> ReadBytes(IDataObject* data, CLIPFORMAT cf)
{
	std::vector<unsigned char> out;
	FORMATETC fmt = { cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM medium = {};
	if (!data || FAILED(data->GetData(&fmt, &medium)))
		return out;
	const SIZE_T n = GlobalSize(medium.hGlobal);
	const void* p = GlobalLock(medium.hGlobal);
	if (p) {
		out.assign(static_cast<const unsigned char*>(p), static_cast<const unsigned char*>(p) + n);
		GlobalUnlock(medium.hGlobal);
	}
	ReleaseStgMedium(&medium);
	return out;
}

HGLOBAL MakeBytes(const void* p, size_t n)
{
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
	if (!h)
		return NULL;
	void* d = GlobalLock(h);
	if (n)
		memcpy(d, p, n);
	GlobalUnlock(h);
	return h;
}

} // namespace fmdnd
