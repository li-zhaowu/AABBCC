// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "FileManagerDragDrop.h"

#include <cstring>

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
