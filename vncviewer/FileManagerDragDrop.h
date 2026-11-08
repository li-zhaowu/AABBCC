// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// Minimal OLE drag and drop objects for the file transfer window:
//  - DataObject: holds HGLOBAL formats (CF_HDROP for local files, a private
//    format for remote items);
//  - DropSource: the standard behaviour;
//  - DropTarget: forwards to a callback that decides the effect.

#pragma once

#include <winsock2.h>
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shldisp.h>

#include "common/FileManClient.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fmdnd {

class DataObject final : public IDataObject {
public:
	DataObject() = default;
	// Takes ownership of h.
	void Add(CLIPFORMAT cf, HGLOBAL h);

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;
	// IDataObject
	STDMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* medium) override;
	STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
	STDMETHODIMP QueryGetData(FORMATETC* fmt) override;
	STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override;
	STDMETHODIMP SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release) override;
	STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** e) override;
	STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
	STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
	~DataObject();
	LONG m_refs = 1;
	std::vector<std::pair<CLIPFORMAT, HGLOBAL>> m_items;
};

class DropSource final : public IDropSource {
public:
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;
	STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override;
	STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
	LONG m_refs = 1;
};

class DropTarget final : public IDropTarget {
public:
	// Called on enter/over (drop = false) and on drop (drop = true); returns
	// the effect. data is null when the drag leaves.
	using Handler = std::function<DWORD(IDataObject* data, POINTL pt, DWORD keys, bool drop)>;
	explicit DropTarget(Handler h) : m_handler(std::move(h)) {}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;
	STDMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
	STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
	STDMETHODIMP DragLeave() override;
	STDMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

private:
	~DropTarget();
	LONG m_refs = 1;
	Handler m_handler;
	IDataObject* m_data = nullptr;
};

// --- remote files dragged to Explorer ("virtual files") ----------------------

// Shared by the window and the streams it hands out: when the window goes
// away, open streams are closed and stop using the client. UI thread only
// (Explorer's calls arrive on the thread that owns the data object).
class RemoteSession {
public:
	explicit RemoteSession(fileman::FileManClient* client) : m_client(client) {}
	fileman::FileManClient* Client() const { return m_client; }
	void Shutdown();
	void Add(class RemoteFileStream* s);
	void Remove(class RemoteFileStream* s);

private:
	fileman::FileManClient* m_client;
	std::vector<class RemoteFileStream*> m_streams;
};

// One remote file, read from start to end (FileContents).
class RemoteFileStream final : public IStream {
public:
	RemoteFileStream(std::shared_ptr<RemoteSession> session, std::unique_ptr<fileman::RemoteReader> reader,
	                 const std::wstring& name);
	void Abandon(); // the session ends

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;
	// ISequentialStream
	STDMETHODIMP Read(void* pv, ULONG cb, ULONG* pcbRead) override;
	STDMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
	// IStream
	STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override;
	STDMETHODIMP SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
	STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
	STDMETHODIMP Commit(DWORD) override { return S_OK; }
	STDMETHODIMP Revert() override { return E_NOTIMPL; }
	STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
	STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
	STDMETHODIMP Stat(STATSTG* st, DWORD flags) override;
	STDMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

private:
	~RemoteFileStream();
	LONG m_refs = 1;
	std::shared_ptr<RemoteSession> m_session;
	std::unique_ptr<fileman::RemoteReader> m_reader;
	std::wstring m_name;
	uint64_t m_size = 0, m_mtime = 0;
};

// Data object for remote items: FileGroupDescriptorW (folders expanded
// when Explorer first asks) + FileContents streams, plus extra HGLOBAL
// formats (our private one for drops on the local side). Supports the
// asynchronous mode, so Explorer copies in the background after the drop.
class VirtualDataObject final : public IDataObject, public IDataObjectAsyncCapability {
public:
	struct Source {
		std::string path; // remote, UTF-8
		bool directory = false;
		uint64_t size = 0, mtime = 0;
	};
	VirtualDataObject(std::shared_ptr<RemoteSession> session, std::vector<Source> sources);
	void Add(CLIPFORMAT cf, HGLOBAL h); // takes ownership

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;
	// IDataObject
	STDMETHODIMP GetData(FORMATETC* fmt, STGMEDIUM* medium) override;
	STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
	STDMETHODIMP QueryGetData(FORMATETC* fmt) override;
	STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override;
	STDMETHODIMP SetData(FORMATETC* fmt, STGMEDIUM* medium, BOOL release) override;
	STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** e) override;
	STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
	STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
	STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
	// IDataObjectAsyncCapability
	STDMETHODIMP SetAsyncMode(BOOL async) override { m_async = async; return S_OK; }
	STDMETHODIMP GetAsyncMode(BOOL* async) override { *async = m_async; return S_OK; }
	STDMETHODIMP StartOperation(IBindCtx*) override { m_inOperation = TRUE; return S_OK; }
	STDMETHODIMP InOperation(BOOL* in) override { *in = m_inOperation; return S_OK; }
	STDMETHODIMP EndOperation(HRESULT, IBindCtx*, DWORD) override { m_inOperation = FALSE; return S_OK; }

private:
	struct Item {
		std::wstring relative;  // "folder\\sub\\file"
		std::string remote;
		bool directory = false;
		uint64_t size = 0, mtime = 0;
	};
	~VirtualDataObject();
	bool Expand();  // fills m_items (once)
	bool ExpandFolder(const std::string& remote, const std::wstring& relative, int depth);

	LONG m_refs = 1;
	std::shared_ptr<RemoteSession> m_session;
	std::vector<Source> m_sources;
	std::vector<Item> m_items;
	bool m_expanded = false;
	bool m_expandFailed = false;
	std::vector<std::pair<CLIPFORMAT, HGLOBAL>> m_extra;
	BOOL m_async = TRUE;
	BOOL m_inOperation = FALSE;
};

// CF_HDROP for a list of absolute paths.
HGLOBAL MakeHDrop(const std::vector<std::wstring>& paths);
// Paths from a CF_HDROP data object (empty if it has none).
std::vector<std::wstring> ReadHDrop(IDataObject* data);
bool HasFormat(IDataObject* data, CLIPFORMAT cf);
// The bytes of an HGLOBAL format.
std::vector<unsigned char> ReadBytes(IDataObject* data, CLIPFORMAT cf);
HGLOBAL MakeBytes(const void* p, size_t n);

} // namespace fmdnd
