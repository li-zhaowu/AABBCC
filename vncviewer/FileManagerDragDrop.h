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

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>

#include <functional>
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

// CF_HDROP for a list of absolute paths.
HGLOBAL MakeHDrop(const std::vector<std::wstring>& paths);
// Paths from a CF_HDROP data object (empty if it has none).
std::vector<std::wstring> ReadHDrop(IDataObject* data);
bool HasFormat(IDataObject* data, CLIPFORMAT cf);
// The bytes of an HGLOBAL format.
std::vector<unsigned char> ReadBytes(IDataObject* data, CLIPFORMAT cf);
HGLOBAL MakeBytes(const void* p, size_t n);

} // namespace fmdnd
