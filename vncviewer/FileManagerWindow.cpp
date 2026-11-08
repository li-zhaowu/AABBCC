// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#include "FileManagerWindow.h"
#include "FileManagerDragDrop.h"
#include "common/FileManServer.h" // string and time helpers

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

using namespace fileman;

namespace {

const wchar_t kClassName[] = L"UltraVNCFileManager";
const wchar_t kSettingsKey[] = L"Software\\UltraVNC\\Viewer\\FileTransferWindow";

enum : UINT {
	WM_FM_REMOTE_LIST = WM_APP + 10,  // wParam: pane seq, lParam: ListResult*
	WM_FM_REMOTE_ROOTS,               // lParam: RootsResult*
	WM_FM_REMOTE_OP,                  // lParam: OpResult*
	WM_FM_CONFLICT,                   // sent; wParam: Conflict*, lParam: ConflictAnswer*
	WM_FM_QUEUE,                      // the queue changed
	WM_FM_FINISHED,                   // lParam: TransferItem*
};

enum : int {
	kIdTitle = 1, kIdPlaces, kIdAddress, kIdList, kIdStatus, kIdAction, kIdUp, kIdRefresh, kIdNew, kIdDelete,
	kPaneBase = 1000,     // + pane * 100 + id
	kIdQueueList = 1301, kIdQueueCancel, kIdQueueClear, kIdQueueTitle, kIdQueueSummary, kIdQueueRetry,
	kMenuOpen = 2001, kMenuTransfer, kMenuRename, kMenuDelete, kMenuNewFolder, kMenuRefresh, kMenuCopyPath,
	kTimerQueue = 1,
};

// Segoe MDL2 Assets (Windows 10 and 11).
const wchar_t kGlyphUp[] = L"\xE74A";
const wchar_t kGlyphRefresh[] = L"\xE72C";
const wchar_t kGlyphNewFolder[] = L"\xE8F4";
const wchar_t kGlyphDelete[] = L"\xE74D";

struct ListResult {
	Result result;
	std::vector<Entry> entries;
	std::string path;
	std::wstring select;
};
struct RootsResult {
	Result result;
	std::vector<Root> roots;
};
struct OpResult {
	enum Kind { kMkDir, kRename, kDelete } kind;
	Result result;
	std::wstring name;  // new folder / new name, to select afterwards
};
struct ConflictAnswer {
	ConflictChoice choice = ConflictChoice::Cancel;
	bool all = false;
};

int CtlId(int pane, int id)
{
	return kPaneBase + pane * 100 + id;
}

std::wstring W(const std::string& s)
{
	return Utf8ToWide(s);
}

std::string U(const std::wstring& s)
{
	return WideToUtf8(s);
}

std::wstring FormatSize(uint64_t bytes)
{
	wchar_t buf[64];
	StrFormatByteSizeW(static_cast<LONGLONG>(bytes), buf, 64);
	return buf;
}

std::wstring FormatTime(uint64_t ft)
{
	if (!ft)
		return std::wstring();
	FILETIME utc = U64ToFileTime(ft);
	SYSTEMTIME st, local;
	if (!FileTimeToSystemTime(&utc, &st) || !SystemTimeToTzSpecificLocalTime(NULL, &st, &local))
		return std::wstring();
	wchar_t date[64] = {}, time[64] = {};
	GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, NULL, date, 64, NULL);
	GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, NULL, time, 64);
	return std::wstring(date) + L" " + time;
}

std::wstring FormatDuration(uint64_t seconds)
{
	wchar_t buf[32];
	if (seconds >= 3600)
		swprintf(buf, 32, L"%llu:%02llu:%02llu", seconds / 3600, (seconds / 60) % 60, seconds % 60);
	else
		swprintf(buf, 32, L"%llu:%02llu", seconds / 60, seconds % 60);
	return buf;
}

bool SamePath(const std::string& a, const std::string& b)
{
	return CompareStringOrdinal(W(a).c_str(), -1, W(b).c_str(), -1, TRUE) == CSTR_EQUAL;
}

UINT DpiOf(HWND hwnd)
{
	typedef UINT(WINAPI * PFN)(HWND);
	static PFN fn = reinterpret_cast<PFN>(reinterpret_cast<void*>(
		GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
	if (fn && hwnd)
		return fn(hwnd);
	HDC dc = GetDC(NULL);
	const UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY));
	ReleaseDC(NULL, dc);
	return dpi;
}

LOGFONTW MessageFont(UINT dpi)
{
	NONCLIENTMETRICSW ncm = {};
	ncm.cbSize = sizeof(ncm);
	typedef BOOL(WINAPI * PFN)(UINT, UINT, PVOID, UINT, UINT);
	static PFN fn = reinterpret_cast<PFN>(reinterpret_cast<void*>(
		GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi")));
	if (!(fn && fn(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi))) {
		SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
		ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, static_cast<int>(dpi), 96);
	}
	return ncm.lfMessageFont;
}

void SetExplorerTheme(HWND hwnd)
{
	typedef HRESULT(WINAPI * PFN)(HWND, LPCWSTR, LPCWSTR);
	static PFN fn = [] {
		HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		return ux ? reinterpret_cast<PFN>(reinterpret_cast<void*>(GetProcAddress(ux, "SetWindowTheme"))) : nullptr;
	}();
	if (fn)
		fn(hwnd, L"Explorer", NULL);
}

std::string KnownFolder(REFKNOWNFOLDERID id)
{
	PWSTR p = nullptr;
	std::string out;
	if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p)) && p)
		out = U(p);
	CoTaskMemFree(p);
	return out;
}

const UINT kCfRemoteItems = RegisterClipboardFormatW(L"UltraVNC.FileManager.RemoteItems");

struct RemoteDragCookie {
	DWORD pid;
	uint32_t cookie;
};

} // namespace

// --- creation ------------------------------------------------------------------

FileManagerWindow* FileManagerWindow::Open(const Options& options)
{
	static bool registered = false;
	HINSTANCE inst = GetModuleHandleW(NULL);
	if (!registered) {
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &FileManagerWindow::WndProc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		wc.lpszClassName = kClassName;
		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return nullptr;
		registered = true;
	}
	auto* self = new FileManagerWindow(options);
	const std::wstring title = options.fileMode ? options.host + L" - UltraVNC File Transfer"
	                                            : L"File Transfer - " + options.host;
	HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		CW_USEDEFAULT, CW_USEDEFAULT, 1000, 680, NULL, NULL, inst, self);
	if (!hwnd) {
		delete self;
		return nullptr;
	}
	return self;
}

FileManagerWindow::FileManagerWindow(const Options& options) : m_options(options), m_client(options.client)
{
	m_panes[kLocal].id = kLocal;
	m_panes[kRemote].id = kRemote;
	m_cookie = static_cast<uint32_t>(GetTickCount()) ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
	// Drag and drop needs OLE on this (the UI) thread.
	const HRESULT hr = OleInitialize(NULL);
	m_ole = SUCCEEDED(hr);
	m_session = std::make_shared<fmdnd::RemoteSession>(m_client);
}

FileManagerWindow::~FileManagerWindow()
{
	if (m_font)
		DeleteObject(m_font);
	if (m_boldFont)
		DeleteObject(m_boldFont);
	if (m_iconFont)
		DeleteObject(m_iconFont);
	// OLE stays initialised for the thread (other windows may use it).
}

void FileManagerWindow::Activate()
{
	if (IsIconic(m_hwnd))
		ShowWindow(m_hwnd, SW_RESTORE);
	SetForegroundWindow(m_hwnd);
}

void FileManagerWindow::Destroy()
{
	m_closing = true;
	DestroyWindow(m_hwnd);
}

LRESULT CALLBACK FileManagerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	FileManagerWindow* self;
	if (msg == WM_NCCREATE) {
		self = static_cast<FileManagerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
		self->m_hwnd = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	} else {
		self = reinterpret_cast<FileManagerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	}
	if (!self)
		return DefWindowProcW(hwnd, msg, wp, lp);
	if (msg == WM_NCDESTROY) {
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		auto onClosed = self->m_options.onClosed;
		delete self;
		if (onClosed)
			onClosed();
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
	return self->Handle(msg, wp, lp);
}

void FileManagerWindow::CreateChildren()
{
	HINSTANCE inst = GetModuleHandleW(NULL);
	m_dpi = DpiOf(m_hwnd);
	m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0,
		0, 0, m_hwnd, NULL, inst, NULL);
	SHFILEINFOW sfi = {};
	m_smallIcons = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi),
		SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
	CreatePane(m_panes[kLocal]);
	CreatePane(m_panes[kRemote]);

	m_queueTitle = CreateWindowExW(0, L"STATIC", L"Transfers", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
		0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdQueueTitle)), inst, NULL);
	m_queueSummary = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE |
		SS_ENDELLIPSIS, 0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdQueueSummary)), inst, NULL);
	m_btnCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdQueueCancel)), inst, NULL);
	m_btnRetry = CreateWindowExW(0, L"BUTTON", L"Retry", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdQueueRetry)), inst, NULL);
	m_btnClear = CreateWindowExW(0, L"BUTTON", L"Clear finished", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdQueueClear)), inst, NULL);
	m_queueList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
		LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, 0, 0, 0, 0, m_hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdQueueList)), inst, NULL);
	ListView_SetExtendedListViewStyle(m_queueList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	SetExplorerTheme(m_queueList);
	const struct { const wchar_t* name; int width; int fmt; } qcols[] = {
		{ L"Name", 260, LVCFMT_LEFT }, { L"Progress", 160, LVCFMT_LEFT }, { L"Size", 90, LVCFMT_RIGHT },
		{ L"Speed", 90, LVCFMT_RIGHT }, { L"Time left", 80, LVCFMT_RIGHT }, { L"Status", 300, LVCFMT_LEFT },
	};
	for (int i = 0; i < 6; ++i) {
		LVCOLUMNW col = {};
		col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
		col.pszText = const_cast<wchar_t*>(qcols[i].name);
		col.cx = Scale(qcols[i].width);
		col.fmt = qcols[i].fmt;
		ListView_InsertColumn(m_queueList, i, &col);
	}
	ApplyFonts();
}

void FileManagerWindow::CreatePane(Pane& p)
{
	HINSTANCE inst = GetModuleHandleW(NULL);
	const int pane = p.id;
	auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
		return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, m_hwnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(CtlId(pane, id))), inst, NULL);
	};
	const std::wstring title = pane == kLocal ? L"This computer" : L"Remote: " + m_options.host;
	p.title = make(0, L"STATIC", title.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, kIdTitle);
	p.places = make(0, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kIdPlaces);
	p.address = make(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kIdAddress);
	SetWindowSubclass(p.address, &FileManagerWindow::EditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
	SendMessageW(p.address, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Type a folder and press Enter"));
	p.btnUp = make(0, L"BUTTON", kGlyphUp, WS_TABSTOP | BS_PUSHBUTTON, kIdUp);
	p.btnRefresh = make(0, L"BUTTON", kGlyphRefresh, WS_TABSTOP | BS_PUSHBUTTON, kIdRefresh);
	p.btnNew = make(0, L"BUTTON", kGlyphNewFolder, WS_TABSTOP | BS_PUSHBUTTON, kIdNew);
	p.btnDelete = make(0, L"BUTTON", kGlyphDelete, WS_TABSTOP | BS_PUSHBUTTON, kIdDelete);
	p.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
		LVS_EDITLABELS | LVS_SHAREIMAGELISTS, kIdList);
	p.status = make(0, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, kIdStatus);
	p.action = make(0, L"BUTTON", pane == kLocal ? L"Upload  \x2192" : L"\x2190  Download",
		WS_TABSTOP | BS_PUSHBUTTON, kIdAction);

	ListView_SetExtendedListViewStyle(p.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
	SetExplorerTheme(p.list);
	if (m_smallIcons)
		ListView_SetImageList(p.list, m_smallIcons, LVSIL_SMALL);
	const struct { const wchar_t* name; int width; int fmt; } cols[] = {
		{ L"Name", 230, LVCFMT_LEFT }, { L"Size", 80, LVCFMT_RIGHT }, { L"Type", 110, LVCFMT_LEFT },
		{ L"Modified", 125, LVCFMT_LEFT },
	};
	for (int i = 0; i < 4; ++i) {
		LVCOLUMNW col = {};
		col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
		col.pszText = const_cast<wchar_t*>(cols[i].name);
		col.cx = Scale(cols[i].width);
		col.fmt = cols[i].fmt;
		ListView_InsertColumn(p.list, i, &col);
	}

	auto tip = [&](HWND ctl, const wchar_t* text) {
		TOOLINFOW ti = {};
		ti.cbSize = sizeof(ti);
		ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		ti.hwnd = m_hwnd;
		ti.uId = reinterpret_cast<UINT_PTR>(ctl);
		ti.lpszText = const_cast<wchar_t*>(text);
		SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
	};
	tip(p.btnUp, L"Up one level (Backspace)");
	tip(p.btnRefresh, L"Refresh (F5)");
	tip(p.btnNew, L"New folder (F7)");
	tip(p.btnDelete, L"Delete (Del)");
	tip(p.action, pane == kLocal ? L"Copy the selected items to the remote computer"
	                             : L"Copy the selected items to this computer");

	if (m_ole) {
		const int index = pane;
		p.dropTarget = new fmdnd::DropTarget([this, index](IDataObject* data, POINTL pt, DWORD, bool drop) {
			return OnDrop(m_panes[index], data, pt, drop);
		});
		RegisterDragDrop(p.list, p.dropTarget);
	}
}

void FileManagerWindow::ApplyFonts()
{
	if (m_font)
		DeleteObject(m_font);
	if (m_boldFont)
		DeleteObject(m_boldFont);
	if (m_iconFont)
		DeleteObject(m_iconFont);
	LOGFONTW lf = MessageFont(m_dpi);
	m_font = CreateFontIndirectW(&lf);
	LOGFONTW bold = lf;
	bold.lfWeight = FW_SEMIBOLD;
	m_boldFont = CreateFontIndirectW(&bold);
	LOGFONTW icon = {};
	icon.lfHeight = lf.lfHeight;
	icon.lfCharSet = DEFAULT_CHARSET;
	icon.lfQuality = CLEARTYPE_QUALITY;
	wcscpy_s(icon.lfFaceName, L"Segoe MDL2 Assets");
	m_iconFont = CreateFontIndirectW(&icon);

	auto set = [](HWND h, HFONT f) {
		if (h)
			SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
	};
	for (Pane& p : m_panes) {
		set(p.title, m_boldFont);
		for (HWND h : { p.places, p.address, p.list, p.status, p.action })
			set(h, m_font);
		for (HWND h : { p.btnUp, p.btnRefresh, p.btnNew, p.btnDelete })
			set(h, m_iconFont);
	}
	set(m_queueTitle, m_boldFont);
	for (HWND h : { m_queueSummary, m_queueList, m_btnCancel, m_btnRetry, m_btnClear })
		set(h, m_font);
	set(m_tooltip, m_font);
}

void FileManagerWindow::Layout()
{
	RECT rc;
	GetClientRect(m_hwnd, &rc);
	const int W = rc.right, H = rc.bottom;
	if (W <= 0 || H <= 0)
		return;
	const int m = Scale(8), gap = Scale(6);
	const int rowH = Scale(26), btnW = Scale(30), titleH = Scale(22);

	const int splitY = (std::max)(Scale(200), (std::min)(H - Scale(140), static_cast<int>(H * m_queueSplit)));
	const int splitX = (std::max)(Scale(260), (std::min)(W - Scale(260), static_cast<int>(W * m_split)));
	m_vSplitRect = { splitX - gap / 2, m, splitX + gap / 2 + 1, splitY };
	m_hSplitRect = { m, splitY, W - m, splitY + gap };

	HDWP dwp = BeginDeferWindowPos(40);
	auto put = [&](HWND h, int x, int y, int w, int hh) {
		if (h)
			dwp = DeferWindowPos(dwp, h, NULL, x, y, (std::max)(0, w), (std::max)(0, hh), SWP_NOZORDER | SWP_NOACTIVATE);
	};
	for (Pane& p : m_panes) {
		const int x0 = p.id == kLocal ? m : splitX + gap / 2 + 1;
		const int x1 = p.id == kLocal ? splitX - gap / 2 : W - m;
		const int w = x1 - x0;
		int y = m;
		put(p.title, x0, y, w, titleH);
		y += titleH + Scale(2);
		const int placesW = (std::min)(Scale(170), w / 3);
		put(p.places, x0, y, placesW, rowH * 10);
		const int buttons = 4 * btnW + 3 * Scale(2);
		const int addrX = x0 + placesW + Scale(4);
		const int addrW = w - placesW - Scale(4) - buttons - Scale(4);
		put(p.address, addrX, y, addrW, rowH);
		int bx = addrX + addrW + Scale(4);
		for (HWND b : { p.btnUp, p.btnRefresh, p.btnNew, p.btnDelete }) {
			put(b, bx, y, btnW, rowH);
			bx += btnW + Scale(2);
		}
		y += rowH + Scale(4);
		const int bottomY = splitY - rowH;
		put(p.list, x0, y, w, bottomY - Scale(4) - y);
		const int actionW = Scale(130);
		put(p.status, x0, bottomY, w - actionW - Scale(6), rowH);
		put(p.action, x1 - actionW, bottomY, actionW, rowH);
	}
	int y = splitY + gap;
	const int qbw = Scale(110);
	put(m_queueTitle, m, y, Scale(90), rowH);
	put(m_queueSummary, m + Scale(94), y, W - 2 * m - Scale(94) - 3 * qbw - Scale(12), rowH);
	put(m_btnCancel, W - m - 3 * qbw - Scale(8), y, qbw, rowH);
	put(m_btnRetry, W - m - 2 * qbw - Scale(4), y, qbw, rowH);
	put(m_btnClear, W - m - qbw, y, qbw, rowH);
	y += rowH + Scale(4);
	put(m_queueList, m, y, W - 2 * m, H - m - y);
	EndDeferWindowPos(dwp);
}

// --- window procedure ----------------------------------------------------------

LRESULT CALLBACK FileManagerWindow::EditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<FileManagerWindow*>(ref);
	if (msg == WM_GETDLGCODE && wp == VK_RETURN)
		return DLGC_WANTALLKEYS;
	if (msg == WM_KEYDOWN && wp == VK_RETURN) {
		Pane* p = self->PaneOf(hwnd);
		if (p) {
			const int n = GetWindowTextLengthW(hwnd);
			std::wstring text(static_cast<size_t>(n) + 1, L'\0');
			GetWindowTextW(hwnd, &text[0], n + 1);
			text.resize(static_cast<size_t>(n));
			// Environment variables in local paths (%USERPROFILE%).
			if (p->id == kLocal && text.find(L'%') != std::wstring::npos) {
				wchar_t buf[4096];
				if (ExpandEnvironmentStringsW(text.c_str(), buf, 4096))
					text = buf;
			}
			while (text.size() > 3 && (text.back() == L'\\' || text.back() == L' '))
				text.pop_back();
			if (text.size() == 2 && text[1] == L':')
				text += L'\\';
			self->Navigate(*p, U(text));
			SetFocus(p->list);
		}
		return 0;
	}
	if (msg == WM_CHAR && wp == VK_RETURN)
		return 0;
	if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
		Pane* p = self->PaneOf(hwnd);
		if (p)
			SetWindowTextW(hwnd, W(p->path).c_str());
		return 0;
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT FileManagerWindow::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE: {
		CreateChildren();
		if (m_options.icon) {
			SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_options.icon));
			SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_options.icon));
		}
		QueueCallbacks qc;
		HWND hwnd = m_hwnd;
		std::atomic<bool>* dirty = &m_queueDirty;
		qc.changed = [hwnd, dirty] {
			if (!dirty->exchange(true))
				PostMessageW(hwnd, WM_FM_QUEUE, 0, 0);
		};
		qc.finished = [hwnd](const TransferItem& item) {
			auto* copy = new TransferItem(item);
			if (!PostMessageW(hwnd, WM_FM_FINISHED, 0, reinterpret_cast<LPARAM>(copy)))
				delete copy;
		};
		qc.askConflict = [hwnd](const Conflict& c, bool& all) {
			ConflictAnswer answer;
			DWORD_PTR result = 0;
			if (!SendMessageTimeoutW(hwnd, WM_FM_CONFLICT, reinterpret_cast<WPARAM>(&c), reinterpret_cast<LPARAM>(&answer),
				    SMTO_NORMAL, INFINITE, &result))
				return ConflictChoice::Cancel;
			all = answer.all;
			return answer.choice;
		};
		m_queue.reset(new FileTransferQueue(*m_client, qc));
		LoadSettings();
		Pane& local = m_panes[kLocal];
		FillLocalPlaces(local);
		std::string start = local.path;
		local.path.clear();
		if (start.empty() || GetFileAttributesW(LocalPath(start).c_str()) == INVALID_FILE_ATTRIBUTES)
			start = KnownFolder(FOLDERID_Documents);
		Navigate(local, start);
		SetConnected(m_client->Connected());
		UpdateButtons();
		ShowWindow(m_hwnd, SW_SHOW);
		SetFocus(local.list);
		return 0;
	}
	case WM_SIZE:
		Layout();
		return 0;
	case WM_GETMINMAXINFO: {
		auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
		mmi->ptMinTrackSize.x = Scale(720);
		mmi->ptMinTrackSize.y = Scale(480);
		return 0;
	}
	case WM_DPICHANGED: {
		m_dpi = HIWORD(wp);
		ApplyFonts();
		const RECT* r = reinterpret_cast<const RECT*>(lp);
		SetWindowPos(m_hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		Layout();
		return 0;
	}
	case WM_SETCURSOR: {
		POINT pt;
		GetCursorPos(&pt);
		ScreenToClient(m_hwnd, &pt);
		if (reinterpret_cast<HWND>(wp) == m_hwnd && LOWORD(lp) == HTCLIENT) {
			if (PtInRect(&m_vSplitRect, pt)) {
				SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
				return TRUE;
			}
			if (PtInRect(&m_hSplitRect, pt)) {
				SetCursor(LoadCursorW(NULL, IDC_SIZENS));
				return TRUE;
			}
		}
		break;
	}
	case WM_LBUTTONDOWN: {
		POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		m_dragSplitter = PtInRect(&m_vSplitRect, pt) ? 1 : PtInRect(&m_hSplitRect, pt) ? 2 : 0;
		if (m_dragSplitter)
			SetCapture(m_hwnd);
		return 0;
	}
	case WM_MOUSEMOVE:
		if (m_dragSplitter && (wp & MK_LBUTTON)) {
			RECT rc;
			GetClientRect(m_hwnd, &rc);
			if (m_dragSplitter == 1 && rc.right > 0)
				m_split = (std::max)(0.2, (std::min)(0.8, GET_X_LPARAM(lp) / double(rc.right)));
			else if (m_dragSplitter == 2 && rc.bottom > 0)
				m_queueSplit = (std::max)(0.35, (std::min)(0.85, GET_Y_LPARAM(lp) / double(rc.bottom)));
			Layout();
		}
		return 0;
	case WM_LBUTTONUP:
		if (m_dragSplitter) {
			m_dragSplitter = 0;
			ReleaseCapture();
		}
		return 0;
	case WM_COMMAND:
		OnCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
		return 0;
	case WM_NOTIFY:
		return OnNotify(reinterpret_cast<NMHDR*>(lp));
	case WM_CONTEXTMENU: {
		Pane* p = PaneOf(reinterpret_cast<HWND>(wp));
		if (p && reinterpret_cast<HWND>(wp) == p->list) {
			POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
			if (pt.x == -1 && pt.y == -1) { // keyboard
				RECT r;
				const int i = ListView_GetNextItem(p->list, -1, LVNI_FOCUSED);
				if (i >= 0 && ListView_GetItemRect(p->list, i, &r, LVIR_LABEL)) {
					pt = { r.left, r.bottom };
					ClientToScreen(p->list, &pt);
				} else {
					GetCursorPos(&pt);
				}
			}
			ContextMenu(*p, pt);
			return 0;
		}
		break;
	}
	case WM_TIMER:
		if (wp == kTimerQueue) {
			RefreshQueue();
			if (!m_queue->Busy())
				KillTimer(m_hwnd, kTimerQueue);
		}
		return 0;
	case WM_FM_QUEUE:
		m_queueDirty = false;
		RefreshQueue();
		if (m_queue->Busy())
			SetTimer(m_hwnd, kTimerQueue, 500, NULL); // speeds and times change with no event
		return 0;
	case WM_FM_FINISHED: {
		std::unique_ptr<TransferItem> item(reinterpret_cast<TransferItem*>(lp));
		// Show the result in the target folder if it is open.
		Pane& target = item->direction == Direction::Upload ? m_panes[kRemote] : m_panes[kLocal];
		if (SamePath(target.path, item->targetDir))
			Navigate(target, target.path, W(item->name));
		if (item->state == ItemState::Done && item->errors == 0)
			MessageBeep(MB_OK);
		return 0;
	}
	case WM_FM_CONFLICT: {
		auto* answer = reinterpret_cast<ConflictAnswer*>(lp);
		if (m_closing) {
			answer->choice = ConflictChoice::Cancel;
			return 0;
		}
		answer->choice = AskConflict(*reinterpret_cast<const Conflict*>(wp), answer->all);
		return 0;
	}
	case WM_FM_REMOTE_LIST: {
		std::unique_ptr<ListResult> res(reinterpret_cast<ListResult*>(lp));
		Pane& p = m_panes[kRemote];
		if (static_cast<uint32_t>(wp) != p.loadSeq)
			return 0; // an older request
		p.loading = false;
		if (!res->result.Ok()) {
			p.error = res->result.Describe();
			if (p.path.empty() || res->result.status == kErrDisconnected) {
				SetItems(p, {}, std::string());
			} else {
				// Stay where we were; tell why.
				MessageBoxW(m_hwnd, (L"Cannot open " + W(res->path) + L"\n\n" + W(p.error)).c_str(),
					L"UltraVNC File Transfer", MB_OK | MB_ICONWARNING);
				SetWindowTextW(p.address, W(p.path).c_str());
				p.error.clear();
				UpdatePaneStatus(p);
			}
			return 0;
		}
		p.error.clear();
		std::vector<Item> items;
		items.reserve(res->entries.size());
		for (const Entry& e : res->entries) {
			if ((e.attributes & kAttrHidden) && (e.attributes & kAttrSystem))
				continue; // protected system files, hidden like Explorer does
			if (!IsValidName(e.name))
				continue;
			Item it;
			it.name = W(e.name);
			it.dir = e.IsDirectory();
			it.size = e.size;
			it.mtime = e.mtime;
			it.attrs = e.attributes;
			items.push_back(std::move(it));
		}
		if (!res->select.empty())
			p.selectAfterLoad = res->select;
		SetItems(p, std::move(items), res->path);
		return 0;
	}
	case WM_FM_REMOTE_ROOTS: {
		std::unique_ptr<RootsResult> res(reinterpret_cast<RootsResult*>(lp));
		Pane& p = m_panes[kRemote];
		if (!res->result.Ok()) {
			if (res->result.status == kErrDisconnected)
				return 0;
			// Say why the remote side stays empty (e.g. nobody logged on).
			p.error = res->result.Describe();
			UpdatePaneStatus(p);
			MessageBoxW(m_hwnd, (L"The remote files cannot be shown.\n\n" + W(p.error)).c_str(),
				L"UltraVNC File Transfer", MB_OK | MB_ICONWARNING);
			return 0;
		}
		p.error.clear();
		p.places_.clear();
		for (const Root& r : res->roots) {
			Place pl;
			pl.path = r.path;
			pl.total = r.totalBytes;
			pl.free = r.freeBytes;
			pl.drive = r.type != kRootFolder;
			if (r.type == kRootFolder) {
				pl.label = W(r.label);
			} else {
				std::wstring kind = r.type == kRootNetwork ? L"Network drive" : r.type == kRootRemovable ? L"Removable disk"
					: r.type == kRootCdrom ? L"CD/DVD drive" : L"Local disk";
				pl.label = (r.label.empty() ? kind : W(r.label)) + L" (" + W(r.path.substr(0, 2)) + L")";
			}
			p.places_.push_back(pl);
		}
		FillPlacesCombo(p);
		if (p.path.empty()) {
			std::string start = m_initialRemoteDir;
			if (start.empty()) {
				for (const Place& pl : p.places_)
					if (!pl.drive && pl.label == L"Desktop")
						start = pl.path;
				if (start.empty() && !p.places_.empty())
					start = p.places_.front().path;
			}
			if (!start.empty())
				Navigate(p, start);
		} else {
			UpdatePaneStatus(p);
		}
		return 0;
	}
	case WM_FM_REMOTE_OP: {
		std::unique_ptr<OpResult> res(reinterpret_cast<OpResult*>(lp));
		Pane& p = m_panes[kRemote];
		if (!res->result.Ok()) {
			const wchar_t* what = res->kind == OpResult::kMkDir ? L"Cannot create the folder"
				: res->kind == OpResult::kRename ? L"Cannot rename" : L"Cannot delete";
			if (res->kind == OpResult::kDelete) {
				m_remoteOpErrors += U(res->name) + ": " + res->result.Describe() + "\n";
			} else {
				MessageBoxW(m_hwnd, (std::wstring(what) + L" \x201C" + res->name + L"\x201D.\n\n" +
					W(res->result.Describe())).c_str(), L"UltraVNC File Transfer", MB_OK | MB_ICONWARNING);
			}
		}
		if (res->kind == OpResult::kDelete) {
			if (--m_pendingRemoteOps > 0)
				return 0;
			if (!m_remoteOpErrors.empty()) {
				MessageBoxW(m_hwnd, (L"Some items could not be deleted:\n\n" + W(m_remoteOpErrors)).c_str(),
					L"UltraVNC File Transfer", MB_OK | MB_ICONWARNING);
				m_remoteOpErrors.clear();
			}
			Refresh(p);
			return 0;
		}
		if (res->result.Ok()) {
			p.editAfterLoad = res->kind == OpResult::kMkDir;
			Navigate(p, p.path, res->name);
		}
		return 0;
	}
	case WM_CLOSE:
		if (!m_closing && m_queue && m_queue->Busy()) {
			if (MessageBoxW(m_hwnd, L"Files are still being transferred.\n\nStop the transfers and close?",
				    L"UltraVNC File Transfer", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
				return 0;
		}
		m_closing = true;
		DestroyWindow(m_hwnd);
		return 0;
	case WM_DESTROY: {
		m_closing = true;
		SaveSettings();
		KillTimer(m_hwnd, kTimerQueue);
		if (m_queue) {
			// The worker may be waiting for us (a conflict question): keep
			// answering sent messages until it has stopped.
			m_queue->CancelAll();
			const ULONGLONG until = GetTickCount64() + 15000;
			while (m_queue->Busy() && GetTickCount64() < until) {
				MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_SENDMESSAGE);
				MSG m;
				PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
			}
			m_queue->Shutdown();
		}
		// Explorer may still be copying dragged remote files: stop that
		// cleanly (it reports an error) before the connection goes away.
		if (m_session)
			m_session->Shutdown();
		for (Pane& p : m_panes) {
			if (p.dropTarget) {
				RevokeDragDrop(p.list);
				p.dropTarget->Release();
				p.dropTarget = nullptr;
			}
		}
		return 0;
	}
	default:
		break;
	}
	return DefWindowProcW(m_hwnd, msg, wp, lp);
}

FileManagerWindow::Pane* FileManagerWindow::PaneOf(HWND ctl)
{
	if (!ctl)
		return nullptr;
	const int id = GetDlgCtrlID(ctl);
	if (id >= kPaneBase && id < kPaneBase + 200)
		return &m_panes[(id - kPaneBase) / 100];
	return nullptr;
}

void FileManagerWindow::OnCommand(int id, int code, HWND ctl)
{
	if (id == kIdQueueCancel) {
		const int n = ListView_GetItemCount(m_queueList);
		bool any = false;
		for (int i = 0; i < n; ++i) {
			if (ListView_GetItemState(m_queueList, i, LVIS_SELECTED) && i < static_cast<int>(m_queueItems.size())) {
				m_queue->Cancel(m_queueItems[static_cast<size_t>(i)].id);
				any = true;
			}
		}
		if (!any)
			m_queue->CancelAll();
		return;
	}
	if (id == kIdQueueClear) {
		m_queue->ClearFinished();
		return;
	}
	if (id == kIdQueueRetry) {
		m_queue->Retry(false);
		return;
	}
	Pane* pp = PaneOf(ctl);
	if (!pp)
		return;
	Pane& p = *pp;
	switch ((id - kPaneBase) % 100) {
	case kIdPlaces:
		if (code == CBN_SELCHANGE) {
			const int sel = static_cast<int>(SendMessageW(p.places, CB_GETCURSEL, 0, 0));
			if (sel >= 0 && sel < static_cast<int>(p.places_.size()))
				Navigate(p, p.places_[static_cast<size_t>(sel)].path);
			SetFocus(p.list);
		}
		break;
	case kIdUp:
		GoUp(p);
		break;
	case kIdRefresh:
		Refresh(p);
		break;
	case kIdNew:
		NewFolder(p);
		break;
	case kIdDelete:
		DeleteSelected(p);
		break;
	case kIdAction:
		TransferSelected(p);
		break;
	default:
		break;
	}
}

LRESULT FileManagerWindow::OnNotify(NMHDR* hdr)
{
	if (hdr->hwndFrom == m_queueList) {
		if (hdr->code == LVN_GETDISPINFOW) {
			auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
			const size_t i = static_cast<size_t>(di->item.iItem);
			if (i >= m_queueItems.size() || !(di->item.mask & LVIF_TEXT))
				return 0;
			const TransferItem& t = m_queueItems[i];
			std::wstring text;
			const uint64_t now = GetTickCount64();
			const uint64_t elapsedMs = t.startedMs ? ((t.finishedMs ? t.finishedMs : now) - t.startedMs) : 0;
			const double speed = elapsedMs > 500 ? t.doneBytes * 1000.0 / elapsedMs : 0;
			switch (di->item.iSubItem) {
			case 0:
				text = (t.direction == Direction::Upload ? L"\x2191 " : L"\x2193 ") + W(t.name);
				break;
			case 1: // drawn in custom draw; text for accessibility
				if (t.totalBytes)
					text = std::to_wstring(t.doneBytes * 100 / t.totalBytes) + L"%";
				break;
			case 2:
				if (t.totalBytes || t.state == ItemState::Done)
					text = FormatSize(t.totalBytes);
				if (t.directory && t.files)
					text += L", " + std::to_wstring(t.files) + L" files";
				break;
			case 3:
				if (t.state == ItemState::Running && speed > 0)
					text = FormatSize(static_cast<uint64_t>(speed)) + L"/s";
				break;
			case 4:
				if (t.state == ItemState::Running && speed > 0 && t.totalBytes > t.doneBytes)
					text = FormatDuration(static_cast<uint64_t>((t.totalBytes - t.doneBytes) / speed));
				break;
			case 5:
				switch (t.state) {
				case ItemState::Queued: text = L"Waiting"; break;
				case ItemState::Running:
					text = t.totalBytes == 0 && t.directory && t.files == 0 ? L"Scanning\x2026"
						: (t.currentFile.empty() ? L"Transferring" : W(t.currentFile));
					break;
				case ItemState::Done: {
					text = L"Done";
					std::wstring notes;
					if (t.filesSkipped)
						notes = std::to_wstring(t.filesSkipped) + L" skipped";
					if (t.filesIdentical)
						notes += (notes.empty() ? L"" : L", ") + std::to_wstring(t.filesIdentical) + L" already there";
					if (!notes.empty())
						text += L" (" + notes + L")";
					break;
				}
				case ItemState::Failed:
					if (t.interrupted && !m_connected)
						text = L"Interrupted \x2014 continues when the connection is back";
					else if (t.interrupted)
						text = L"Interrupted: " + W(t.lastError) + L" (Retry continues where it stopped)";
					else
						text = t.errors ? std::to_wstring(t.errors) + L" failed: " + W(t.lastError) : L"Failed: " + W(t.lastError);
					break;
				case ItemState::Cancelled: text = L"Cancelled"; break;
				}
				break;
			}
			wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
			return 0;
		}
		if (hdr->code == NM_CUSTOMDRAW) {
			LRESULT r = CDRF_DODEFAULT;
			QueueCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(hdr), r);
			return r;
		}
		if (hdr->code == NM_DBLCLK) {
			auto* ia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
			if (ia->iItem >= 0 && static_cast<size_t>(ia->iItem) < m_queueItems.size()) {
				const TransferItem& t = m_queueItems[static_cast<size_t>(ia->iItem)];
				Pane& target = t.direction == Direction::Upload ? m_panes[kRemote] : m_panes[kLocal];
				Navigate(target, t.targetDir, W(t.name));
			}
			return 0;
		}
		if (hdr->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(hdr)->wVKey == VK_DELETE) {
			OnCommand(kIdQueueCancel, 0, m_btnCancel);
			return 0;
		}
		return 0;
	}
	Pane* pp = PaneOf(hdr->hwndFrom);
	if (!pp || hdr->hwndFrom != pp->list)
		return 0;
	Pane& p = *pp;
	switch (hdr->code) {
	case LVN_GETDISPINFOW:
		OnListDispInfo(p, reinterpret_cast<NMLVDISPINFOW*>(hdr));
		return 0;
	case LVN_COLUMNCLICK: {
		const int col = reinterpret_cast<NMLISTVIEW*>(hdr)->iSubItem;
		if (col == p.sortColumn)
			p.sortAscending = !p.sortAscending;
		else {
			p.sortColumn = col;
			p.sortAscending = col == 0 || col == 2;
		}
		// Keep the selection by name.
		std::vector<std::wstring> sel;
		for (int i : Selected(p))
			sel.push_back(p.items[static_cast<size_t>(i)].name);
		SortItems(p);
		ListView_SetItemState(p.list, -1, 0, LVIS_SELECTED);
		for (size_t i = 0; i < p.items.size(); ++i)
			if (std::find(sel.begin(), sel.end(), p.items[i].name) != sel.end())
				ListView_SetItemState(p.list, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
		InvalidateRect(p.list, NULL, FALSE);
		return 0;
	}
	case NM_DBLCLK: {
		auto* ia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
		if (ia->iItem >= 0)
			OpenItem(p, ia->iItem);
		return 0;
	}
	case NM_RETURN: {
		const auto sel = Selected(p);
		if (sel.size() == 1 && (p.items[static_cast<size_t>(sel[0])].dir || p.items[static_cast<size_t>(sel[0])].up))
			OpenItem(p, sel[0]);
		else if (!sel.empty())
			TransferSelected(p);
		return 0;
	}
	case LVN_ITEMCHANGED:
		UpdatePaneStatus(p);
		UpdateButtons();
		return 0;
	case NM_SETFOCUS:
		UpdateButtons();
		return 0;
	case LVN_KEYDOWN: {
		const WORD key = reinterpret_cast<NMLVKEYDOWN*>(hdr)->wVKey;
		const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
		if (key == VK_BACK || (key == VK_UP && GetKeyState(VK_MENU) < 0))
			GoUp(p);
		else if (key == VK_DELETE)
			DeleteSelected(p);
		else if (key == VK_F2)
			StartRename(p);
		else if (key == VK_F5)
			Refresh(p);
		else if (key == VK_F7 || (ctrl && shift && key == 'N'))
			NewFolder(p);
		else if (ctrl && key == 'A') {
			ListView_SetItemState(p.list, -1, LVIS_SELECTED, LVIS_SELECTED);
		} else if (key == VK_TAB)
			SetFocus(Other(p).list);
		return 0;
	}
	case LVN_BEGINDRAG:
		BeginDrag(p);
		return 0;
	case LVN_BEGINLABELEDITW: {
		auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
		const size_t i = static_cast<size_t>(di->item.iItem);
		if (i >= p.items.size() || p.items[i].up || (p.id == kRemote && !m_connected))
			return TRUE; // no editing
		return FALSE;
	}
	case LVN_ENDLABELEDITW: {
		auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
		if (di->item.pszText)
			FinishRename(p, di->item.iItem, di->item.pszText);
		return FALSE; // we refresh ourselves
	}
	default:
		break;
	}
	return 0;
}

void FileManagerWindow::OnListDispInfo(Pane& p, NMLVDISPINFOW* di)
{
	const size_t i = static_cast<size_t>(di->item.iItem);
	if (i >= p.items.size())
		return;
	Item& it = p.items[i];
	if (di->item.mask & LVIF_IMAGE)
		di->item.iImage = it.icon;
	if (!(di->item.mask & LVIF_TEXT))
		return;
	std::wstring text;
	switch (di->item.iSubItem) {
	case 0: text = it.name; break;
	case 1: if (!it.dir && !it.up) text = FormatSize(it.size); break;
	case 2: text = it.type; break;
	case 3: if (!it.up) text = FormatTime(it.mtime); break;
	}
	wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
}

// --- navigation ------------------------------------------------------------------

void FileManagerWindow::Navigate(Pane& p, const std::string& path, const std::wstring& select)
{
	if (path.empty())
		return;
	if (p.id == kLocal)
		LoadLocal(p, path, select);
	else
		RequestRemote(p, path, select);
}

void FileManagerWindow::Refresh(Pane& p)
{
	std::wstring keep;
	const auto sel = Selected(p);
	if (!sel.empty())
		keep = p.items[static_cast<size_t>(sel[0])].name;
	if (p.id == kLocal)
		FillLocalPlaces(p);
	else if (m_connected)
		RequestRemoteRoots();
	Navigate(p, p.path, keep);
}

void FileManagerWindow::GoUp(Pane& p)
{
	const std::string parent = ParentPath(p.path);
	if (!parent.empty())
		Navigate(p, parent, W(FileNamePart(p.path)));
}

void FileManagerWindow::OpenItem(Pane& p, int index)
{
	const Item& it = p.items[static_cast<size_t>(index)];
	if (it.up)
		GoUp(p);
	else if (it.dir)
		Navigate(p, JoinPath(p.path, U(it.name)));
	else
		TransferSelected(p);
}

void FileManagerWindow::LoadLocal(Pane& p, const std::string& path, const std::wstring& select)
{
	if (!IsAcceptablePath(path)) {
		MessageBoxW(m_hwnd, (L"\x201C" + W(path) + L"\x201D is not a folder path.").c_str(), L"UltraVNC File Transfer",
			MB_OK | MB_ICONWARNING);
		SetWindowTextW(p.address, W(p.path).c_str());
		return;
	}
	std::wstring pattern = LocalPath(path);
	pattern += pattern.back() == L'\\' ? L"*" : L"\\*";
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
		FIND_FIRST_EX_LARGE_FETCH);
	std::vector<Item> items;
	if (find == INVALID_HANDLE_VALUE) {
		const DWORD e = GetLastError();
		if (e != ERROR_FILE_NOT_FOUND) {
			MessageBoxW(m_hwnd, (L"Cannot open " + W(path) + L"\n\n" + W(Win32Message(e))).c_str(),
				L"UltraVNC File Transfer", MB_OK | MB_ICONWARNING);
			SetWindowTextW(p.address, W(p.path).c_str());
			return;
		}
	} else {
		do {
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
				continue;
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM))
				continue;
			Item it;
			it.name = fd.cFileName;
			it.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			it.size = it.dir ? 0 : (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
			it.mtime = FileTimeToU64(fd.ftLastWriteTime);
			it.attrs = AttributesToWire(fd.dwFileAttributes);
			items.push_back(std::move(it));
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}
	if (!select.empty())
		p.selectAfterLoad = select;
	SetItems(p, std::move(items), path);
}

void FileManagerWindow::RequestRemote(Pane& p, const std::string& path, const std::wstring& select)
{
	if (!m_connected)
		return;
	if (!IsAcceptablePath(path)) {
		MessageBoxW(m_hwnd, (L"\x201C" + W(path) + L"\x201D is not a folder path.").c_str(), L"UltraVNC File Transfer",
			MB_OK | MB_ICONWARNING);
		SetWindowTextW(p.address, W(p.path).c_str());
		return;
	}
	const uint32_t seq = ++p.loadSeq;
	p.loading = true;
	UpdatePaneStatus(p);
	HWND hwnd = m_hwnd;
	m_client->List(path, [hwnd, seq, path, select](const Result& r, std::vector<Entry>& entries) {
		auto* res = new ListResult;
		res->result = r;
		res->entries.swap(entries);
		res->path = path;
		res->select = select;
		if (!PostMessageW(hwnd, WM_FM_REMOTE_LIST, seq, reinterpret_cast<LPARAM>(res)))
			delete res;
	});
}

void FileManagerWindow::RequestRemoteRoots()
{
	HWND hwnd = m_hwnd;
	m_client->Roots([hwnd](const Result& r, std::vector<Root>& roots) {
		auto* res = new RootsResult;
		res->result = r;
		res->roots.swap(roots);
		if (!PostMessageW(hwnd, WM_FM_REMOTE_ROOTS, 0, reinterpret_cast<LPARAM>(res)))
			delete res;
	});
}

void FileManagerWindow::SetItems(Pane& p, std::vector<Item>&& items, const std::string& path)
{
	// Icons and type names, cached per extension.
	for (Item& it : items) {
		std::wstring key = it.dir ? L"<dir>" : std::wstring(PathFindExtensionW(it.name.c_str()));
		CharLowerW(&key[0]);
		auto c = m_typeCache.find(key);
		if (c == m_typeCache.end()) {
			SHFILEINFOW sfi = {};
			const std::wstring probe = it.dir ? L"folder" : L"file" + key;
			SHGetFileInfoW(probe.c_str(), it.dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
				SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME);
			c = m_typeCache.emplace(key, std::make_pair(sfi.iIcon, std::wstring(sfi.szTypeName))).first;
		}
		it.icon = c->second.first;
		it.type = c->second.second;
	}
	if (!path.empty() && !ParentPath(path).empty()) {
		Item up;
		up.name = L"..";
		up.up = true;
		up.dir = true;
		auto c = m_typeCache.find(L"<dir>");
		if (c != m_typeCache.end())
			up.icon = c->second.first;
		items.insert(items.begin(), std::move(up));
	}
	const bool samePath = SamePath(path, p.path);
	const int top = samePath ? ListView_GetTopIndex(p.list) : 0;
	p.path = path;
	p.items = std::move(items);
	SortItems(p);
	SetWindowTextW(p.address, W(path).c_str());
	ListView_SetItemState(p.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	ListView_SetItemCountEx(p.list, static_cast<int>(p.items.size()), 0);
	int focus = -1;
	if (!p.selectAfterLoad.empty()) {
		for (size_t i = 0; i < p.items.size(); ++i) {
			if (CompareStringOrdinal(p.items[i].name.c_str(), -1, p.selectAfterLoad.c_str(), -1, TRUE) == CSTR_EQUAL) {
				focus = static_cast<int>(i);
				break;
			}
		}
	}
	if (focus >= 0) {
		ListView_SetItemState(p.list, focus, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(p.list, focus, FALSE);
	} else if (!p.items.empty()) {
		ListView_SetItemState(p.list, 0, LVIS_FOCUSED, LVIS_FOCUSED);
		if (samePath)
			ListView_EnsureVisible(p.list, (std::min)(top + ListView_GetCountPerPage(p.list) - 1,
				static_cast<int>(p.items.size()) - 1), FALSE);
	}
	InvalidateRect(p.list, NULL, FALSE);
	const bool edit = p.editAfterLoad && focus >= 0;
	p.selectAfterLoad.clear();
	p.editAfterLoad = false;
	SelectPlaceFor(p);
	UpdatePaneStatus(p);
	UpdateButtons();
	if (edit) {
		SetFocus(p.list);
		ListView_EditLabel(p.list, focus);
	}
}

void FileManagerWindow::SortItems(Pane& p)
{
	const int col = p.sortColumn;
	const bool asc = p.sortAscending;
	std::stable_sort(p.items.begin(), p.items.end(), [col, asc](const Item& a, const Item& b) {
		if (a.up != b.up)
			return a.up;
		if (a.dir != b.dir)
			return a.dir; // folders first
		int c = 0;
		switch (col) {
		case 1: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
		case 2: c = StrCmpLogicalW(a.type.c_str(), b.type.c_str()); break;
		case 3: c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
		default: break;
		}
		if (c == 0)
			c = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
		return asc ? c < 0 : c > 0;
	});
	// Sort arrow in the header.
	HWND header = ListView_GetHeader(p.list);
	for (int i = 0; i < 4; ++i) {
		HDITEMW hd = {};
		hd.mask = HDI_FORMAT;
		Header_GetItem(header, i, &hd);
		hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
		if (i == col)
			hd.fmt |= asc ? HDF_SORTUP : HDF_SORTDOWN;
		Header_SetItem(header, i, &hd);
	}
}

std::vector<int> FileManagerWindow::Selected(const Pane& p) const
{
	std::vector<int> out;
	int i = -1;
	while ((i = ListView_GetNextItem(p.list, i, LVNI_SELECTED)) >= 0) {
		if (static_cast<size_t>(i) < p.items.size() && !p.items[static_cast<size_t>(i)].up)
			out.push_back(i);
	}
	return out;
}

void FileManagerWindow::UpdatePaneStatus(Pane& p)
{
	std::wstring text;
	if (p.id == kRemote && !m_connected) {
		text = L"Not connected";
	} else if (p.loading) {
		text = L"Loading\x2026";
	} else if (!p.error.empty()) {
		text = W(p.error);
	} else {
		size_t files = 0, dirs = 0;
		for (const Item& it : p.items) {
			if (it.up)
				continue;
			(it.dir ? dirs : files)++;
		}
		text = std::to_wstring(dirs) + L" folders, " + std::to_wstring(files) + L" files";
		const auto sel = Selected(p);
		if (!sel.empty()) {
			uint64_t bytes = 0;
			for (int i : sel)
				bytes += p.items[static_cast<size_t>(i)].size;
			text += L"  \x2022  " + std::to_wstring(sel.size()) + L" selected (" + FormatSize(bytes) + L")";
		}
		// Free space of the drive we are on.
		for (const Place& pl : p.places_) {
			if (pl.drive && pl.total && p.path.size() >= 2 && pl.path.size() >= 2 &&
			    CompareStringOrdinal(W(pl.path.substr(0, 2)).c_str(), 2, W(p.path.substr(0, 2)).c_str(), 2, TRUE) == CSTR_EQUAL) {
				text += L"  \x2022  " + FormatSize(pl.free) + L" free";
				break;
			}
		}
	}
	SetWindowTextW(p.status, text.c_str());
}

void FileManagerWindow::UpdateButtons()
{
	Pane& local = m_panes[kLocal];
	Pane& remote = m_panes[kRemote];
	const bool remoteReady = m_connected && !remote.path.empty();
	EnableWindow(local.action, remoteReady && !Selected(local).empty());
	EnableWindow(remote.action, remoteReady && !local.path.empty() && !Selected(remote).empty());
	for (Pane* p : { &local, &remote }) {
		const bool ready = p->id == kLocal || remoteReady;
		EnableWindow(p->btnUp, ready && !ParentPath(p->path).empty());
		EnableWindow(p->btnRefresh, p->id == kLocal || m_connected);
		EnableWindow(p->btnNew, ready && !p->path.empty());
		EnableWindow(p->btnDelete, ready && !Selected(*p).empty());
		EnableWindow(p->address, p->id == kLocal || m_connected);
		EnableWindow(p->places, p->id == kLocal || m_connected);
	}
}

void FileManagerWindow::FillLocalPlaces(Pane& p)
{
	p.places_.clear();
	const struct { const KNOWNFOLDERID* id; const wchar_t* label; } folders[] = {
		{ &FOLDERID_Desktop, L"Desktop" }, { &FOLDERID_Documents, L"Documents" }, { &FOLDERID_Downloads, L"Downloads" },
		{ &FOLDERID_Profile, L"Home" },
	};
	for (const auto& f : folders) {
		Place pl;
		pl.path = KnownFolder(*f.id);
		pl.label = f.label;
		if (!pl.path.empty())
			p.places_.push_back(pl);
	}
	const DWORD drives = GetLogicalDrives();
	for (int i = 0; i < 26; ++i) {
		if (!(drives & (1u << i)))
			continue;
		wchar_t root[] = { wchar_t(L'A' + i), L':', L'\\', 0 };
		const UINT type = GetDriveTypeW(root);
		if (type == DRIVE_NO_ROOT_DIR)
			continue;
		Place pl;
		pl.path = U(root);
		pl.drive = true;
		std::wstring label;
		if (type == DRIVE_FIXED || type == DRIVE_REMOTE || type == DRIVE_RAMDISK) {
			wchar_t name[MAX_PATH + 1] = {};
			UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS);
			if (GetVolumeInformationW(root, name, MAX_PATH, NULL, NULL, NULL, NULL, 0))
				label = name;
			ULARGE_INTEGER freeBytes, total;
			if (GetDiskFreeSpaceExW(root, &freeBytes, &total, NULL)) {
				pl.total = total.QuadPart;
				pl.free = freeBytes.QuadPart;
			}
			SetErrorMode(oldMode);
		}
		const wchar_t* kind = type == DRIVE_REMOTE ? L"Network drive" : type == DRIVE_REMOVABLE ? L"Removable disk"
			: type == DRIVE_CDROM ? L"CD/DVD drive" : L"Local disk";
		pl.label = (label.empty() ? std::wstring(kind) : label) + L" (" + std::wstring(root, 2) + L")";
		p.places_.push_back(pl);
	}
	FillPlacesCombo(p);
}

void FileManagerWindow::FillPlacesCombo(Pane& p)
{
	SendMessageW(p.places, CB_RESETCONTENT, 0, 0);
	for (const Place& pl : p.places_)
		SendMessageW(p.places, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pl.label.c_str()));
	SelectPlaceFor(p);
}

// Show the place that contains the current folder (the longest match).
void FileManagerWindow::SelectPlaceFor(Pane& p)
{
	int best = -1;
	size_t bestLen = 0;
	const std::wstring cur = W(p.path);
	for (size_t i = 0; i < p.places_.size(); ++i) {
		const std::wstring pl = W(p.places_[i].path);
		if (pl.size() > cur.size() || pl.size() <= bestLen)
			continue;
		if (CompareStringOrdinal(cur.c_str(), static_cast<int>(pl.size()), pl.c_str(), static_cast<int>(pl.size()), TRUE) !=
		    CSTR_EQUAL)
			continue;
		if (cur.size() > pl.size() && pl.back() != L'\\' && cur[pl.size()] != L'\\')
			continue;
		best = static_cast<int>(i);
		bestLen = pl.size();
	}
	SendMessageW(p.places, CB_SETCURSEL, static_cast<WPARAM>(best), 0);
}

void FileManagerWindow::SetConnected(bool connected)
{
	m_connected = connected;
	Pane& p = m_panes[kRemote];
	if (connected) {
		// Back where we were (reconnect), else the remembered folder.
		if (!p.path.empty())
			m_initialRemoteDir = p.path;
		p.path.clear();
		p.items.clear();
		ListView_SetItemCountEx(p.list, 0, 0);
		RequestRemoteRoots();
		// Transfers cut off by the drop continue where they stopped.
		if (m_queue)
			m_queue->Retry(true);
	} else {
		++p.loadSeq;
		p.loading = false;
		ListView_SetItemCountEx(p.list, 0, 0);
		p.items.clear();
	}
	UpdatePaneStatus(p);
	UpdateButtons();
	if (m_queue)
		RefreshQueue(); // status texts and Retry depend on the connection
}

// --- operations -------------------------------------------------------------------

void FileManagerWindow::TransferSelected(const Pane& from)
{
	const Pane& to = from.id == kLocal ? m_panes[kRemote] : m_panes[kLocal];
	if (!m_connected || to.path.empty() || from.path.empty())
		return;
	std::vector<std::pair<std::string, bool>> sources;
	for (int i : Selected(from)) {
		const Item& it = from.items[static_cast<size_t>(i)];
		sources.emplace_back(JoinPath(from.path, U(it.name)), it.dir);
	}
	TransferPaths(from.id, sources, to.path);
}

void FileManagerWindow::TransferPaths(PaneId from, const std::vector<std::pair<std::string, bool>>& sources,
                                      const std::string& targetDir)
{
	for (const auto& s : sources) {
		uint64_t size = 0;
		const Pane& p = m_panes[from];
		for (const Item& it : p.items)
			if (!it.dir && SamePath(JoinPath(p.path, U(it.name)), s.first))
				size = it.size;
		m_queue->Add(from == kLocal ? Direction::Upload : Direction::Download, s.first, s.second, size, targetDir);
	}
}

void FileManagerWindow::DeleteSelected(Pane& p)
{
	const auto sel = Selected(p);
	if (sel.empty() || (p.id == kRemote && !m_connected))
		return;
	std::wstring question;
	if (sel.size() == 1) {
		const Item& it = p.items[static_cast<size_t>(sel[0])];
		question = (it.dir ? L"Delete the folder \x201C" : L"Delete \x201C") + it.name + L"\x201D" +
			(it.dir ? L" and everything in it?" : L"?");
	} else {
		question = L"Delete these " + std::to_wstring(sel.size()) + L" items?";
	}
	question += p.id == kRemote ? L"\n\nThey are deleted on the remote computer and cannot be restored."
	                            : L"\n\nThey are moved to the recycle bin.";
	if (MessageBoxW(m_hwnd, question.c_str(), L"UltraVNC File Transfer", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
		return;
	if (p.id == kLocal) {
		std::wstring from;
		for (int i : sel)
			from += W(JoinPath(p.path, U(p.items[static_cast<size_t>(i)].name))) + L'\0';
		from += L'\0';
		SHFILEOPSTRUCTW op = {};
		op.hwnd = m_hwnd;
		op.wFunc = FO_DELETE;
		op.pFrom = from.c_str();
		op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
		SHFileOperationW(&op);
		Refresh(p);
		return;
	}
	HWND hwnd = m_hwnd;
	for (int i : sel) {
		const std::wstring name = p.items[static_cast<size_t>(i)].name;
		++m_pendingRemoteOps;
		m_client->Delete(JoinPath(p.path, U(name)), [hwnd, name](const Result& r) {
			auto* res = new OpResult{ OpResult::kDelete, r, name };
			if (!PostMessageW(hwnd, WM_FM_REMOTE_OP, 0, reinterpret_cast<LPARAM>(res)))
				delete res;
		});
	}
	p.loading = true;
	UpdatePaneStatus(p);
}

void FileManagerWindow::NewFolder(Pane& p)
{
	if (p.path.empty() || (p.id == kRemote && !m_connected))
		return;
	std::wstring name = L"New folder";
	for (int n = 2;; ++n) {
		bool taken = false;
		for (const Item& it : p.items)
			if (CompareStringOrdinal(it.name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL)
				taken = true;
		if (!taken)
			break;
		name = L"New folder (" + std::to_wstring(n) + L")";
	}
	const std::string path = JoinPath(p.path, U(name));
	if (p.id == kLocal) {
		if (!CreateDirectoryW(LocalPath(path).c_str(), NULL)) {
			MessageBoxW(m_hwnd, (L"Cannot create the folder.\n\n" + W(Win32Message(GetLastError()))).c_str(),
				L"UltraVNC File Transfer", MB_OK | MB_ICONWARNING);
			return;
		}
		p.editAfterLoad = true;
		Navigate(p, p.path, name);
		return;
	}
	HWND hwnd = m_hwnd;
	m_client->MkDir(path, [hwnd, name](const Result& r) {
		auto* res = new OpResult{ OpResult::kMkDir, r, name };
		if (!PostMessageW(hwnd, WM_FM_REMOTE_OP, 0, reinterpret_cast<LPARAM>(res)))
			delete res;
	});
}

void FileManagerWindow::StartRename(Pane& p)
{
	const int i = ListView_GetNextItem(p.list, -1, LVNI_FOCUSED | LVNI_SELECTED);
	if (i >= 0) {
		SetFocus(p.list);
		ListView_EditLabel(p.list, i);
	}
}

void FileManagerWindow::FinishRename(Pane& p, int index, const std::wstring& newName)
{
	if (index < 0 || static_cast<size_t>(index) >= p.items.size())
		return;
	const Item& it = p.items[static_cast<size_t>(index)];
	if (newName == it.name)
		return;
	if (!IsValidName(U(newName))) {
		MessageBoxW(m_hwnd, L"A name cannot be empty, end with a dot or space, or contain any of these characters:\n\n"
			L"\\ / : * ? \" < > |", L"UltraVNC File Transfer", MB_OK | MB_ICONWARNING);
		return;
	}
	const std::string from = JoinPath(p.path, U(it.name));
	const std::string to = JoinPath(p.path, U(newName));
	if (p.id == kLocal) {
		if (!MoveFileW(LocalPath(from).c_str(), LocalPath(to).c_str())) {
			MessageBoxW(m_hwnd, (L"Cannot rename \x201C" + it.name + L"\x201D.\n\n" + W(Win32Message(GetLastError()))).c_str(),
				L"UltraVNC File Transfer", MB_OK | MB_ICONWARNING);
			return;
		}
		Navigate(p, p.path, newName);
		return;
	}
	HWND hwnd = m_hwnd;
	const std::wstring oldName = it.name;
	m_client->Rename(from, to, [hwnd, newName, oldName](const Result& r) {
		auto* res = new OpResult{ OpResult::kRename, r, r.Ok() ? newName : oldName };
		if (!PostMessageW(hwnd, WM_FM_REMOTE_OP, 0, reinterpret_cast<LPARAM>(res)))
			delete res;
	});
}

void FileManagerWindow::CopyPath(Pane& p)
{
	std::wstring text;
	const auto sel = Selected(p);
	if (sel.empty())
		text = W(p.path);
	for (int i : sel)
		text += (text.empty() ? L"" : L"\r\n") + W(JoinPath(p.path, U(p.items[static_cast<size_t>(i)].name)));
	if (!OpenClipboard(m_hwnd))
		return;
	EmptyClipboard();
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
	if (h) {
		memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
		GlobalUnlock(h);
		if (!SetClipboardData(CF_UNICODETEXT, h))
			GlobalFree(h);
	}
	CloseClipboard();
}

void FileManagerWindow::ContextMenu(Pane& p, POINT screen)
{
	const auto sel = Selected(p);
	const bool ready = p.id == kLocal || m_connected;
	const bool otherReady = m_connected && !Other(p).path.empty();
	HMENU menu = CreatePopupMenu();
	auto add = [&](int id, const wchar_t* text, bool enabled) {
		AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED), id, text);
	};
	bool singleDir = false;
	if (sel.size() == 1)
		singleDir = p.items[static_cast<size_t>(sel[0])].dir;
	if (singleDir)
		add(kMenuOpen, L"&Open\tEnter", true);
	add(kMenuTransfer, p.id == kLocal ? L"&Upload to the remote computer" : L"&Download to this computer",
		otherReady && !sel.empty());
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	add(kMenuRename, L"Re&name\tF2", ready && sel.size() == 1);
	add(kMenuDelete, L"&Delete\tDel", ready && !sel.empty());
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	add(kMenuNewFolder, L"New &folder\tF7", ready && !p.path.empty());
	add(kMenuRefresh, L"&Refresh\tF5", ready);
	add(kMenuCopyPath, L"Copy &path", !p.path.empty());
	if (singleDir)
		SetMenuDefaultItem(menu, kMenuOpen, FALSE);
	const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, m_hwnd, NULL);
	DestroyMenu(menu);
	switch (cmd) {
	case kMenuOpen: OpenItem(p, sel[0]); break;
	case kMenuTransfer: TransferSelected(p); break;
	case kMenuRename: StartRename(p); break;
	case kMenuDelete: DeleteSelected(p); break;
	case kMenuNewFolder: NewFolder(p); break;
	case kMenuRefresh: Refresh(p); break;
	case kMenuCopyPath: CopyPath(p); break;
	default: break;
	}
}

// --- drag and drop ------------------------------------------------------------------

void FileManagerWindow::BeginDrag(Pane& p)
{
	const auto sel = Selected(p);
	if (sel.empty() || !m_ole)
		return;
	IDataObject* data = nullptr;
	if (p.id == kLocal) {
		// Real files: Explorer and other programs accept them too.
		auto* local = new fmdnd::DataObject;
		std::vector<std::wstring> paths;
		for (int i : sel)
			paths.push_back(W(JoinPath(p.path, U(p.items[static_cast<size_t>(i)].name))));
		local->Add(CF_HDROP, fmdnd::MakeHDrop(paths));
		data = local;
	} else {
		if (!m_connected)
			return;
		// Remote items: our private format for the local side, and virtual
		// files (streamed from the server) for Explorer and other programs.
		m_remoteDrag.clear();
		std::vector<fmdnd::VirtualDataObject::Source> sources;
		for (int i : sel) {
			const Item& it = p.items[static_cast<size_t>(i)];
			m_remoteDrag.emplace_back(JoinPath(p.path, U(it.name)), it.dir);
			fmdnd::VirtualDataObject::Source src;
			src.path = JoinPath(p.path, U(it.name));
			src.directory = it.dir;
			src.size = it.size;
			src.mtime = it.mtime;
			sources.push_back(src);
		}
		auto* remote = new fmdnd::VirtualDataObject(m_session, std::move(sources));
		const RemoteDragCookie cookie = { GetCurrentProcessId(), m_cookie };
		remote->Add(static_cast<CLIPFORMAT>(kCfRemoteItems), fmdnd::MakeBytes(&cookie, sizeof(cookie)));
		data = remote;
	}
	auto* source = new fmdnd::DropSource;
	DWORD effect = 0;
	DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
	source->Release();
	data->Release();
}

void FileManagerWindow::SetDropHighlight(Pane& p, int index)
{
	if (p.dropHighlight == index)
		return;
	if (p.dropHighlight >= 0)
		ListView_SetItemState(p.list, p.dropHighlight, 0, LVIS_DROPHILITED);
	p.dropHighlight = index;
	if (index >= 0)
		ListView_SetItemState(p.list, index, LVIS_DROPHILITED, LVIS_DROPHILITED);
}

DWORD FileManagerWindow::OnDrop(Pane& p, IDataObject* data, POINTL pt, bool drop)
{
	if (!data) {
		SetDropHighlight(p, -1);
		return DROPEFFECT_NONE;
	}
	bool acceptable = false;
	if (p.id == kRemote) {
		// Files from our local side or from Explorer.
		acceptable = m_connected && !p.path.empty() && fmdnd::HasFormat(data, CF_HDROP) &&
			!fmdnd::HasFormat(data, static_cast<CLIPFORMAT>(kCfRemoteItems));
	} else if (fmdnd::HasFormat(data, static_cast<CLIPFORMAT>(kCfRemoteItems)) && m_connected) {
		const auto bytes = fmdnd::ReadBytes(data, static_cast<CLIPFORMAT>(kCfRemoteItems));
		RemoteDragCookie cookie = {};
		if (bytes.size() >= sizeof(cookie)) {
			memcpy(&cookie, bytes.data(), sizeof(cookie));
			acceptable = cookie.pid == GetCurrentProcessId() && cookie.cookie == m_cookie && !p.path.empty();
		}
	}
	if (!acceptable) {
		SetDropHighlight(p, -1);
		return DROPEFFECT_NONE;
	}
	// Into the folder under the pointer, else into the current folder.
	LVHITTESTINFO hit = {};
	hit.pt = { pt.x, pt.y };
	ScreenToClient(p.list, &hit.pt);
	const int index = ListView_HitTest(p.list, &hit);
	std::string target = p.path;
	int highlight = -1;
	if (index >= 0 && static_cast<size_t>(index) < p.items.size()) {
		const Item& it = p.items[static_cast<size_t>(index)];
		if (it.up) {
			target = ParentPath(p.path);
			highlight = index;
		} else if (it.dir) {
			target = JoinPath(p.path, U(it.name));
			highlight = index;
		}
	}
	SetDropHighlight(p, highlight);
	if (!drop)
		return DROPEFFECT_COPY;
	SetDropHighlight(p, -1);
	if (p.id == kRemote) {
		std::vector<std::pair<std::string, bool>> sources;
		for (const std::wstring& path : fmdnd::ReadHDrop(data)) {
			const DWORD attrs = GetFileAttributesW(path.c_str());
			if (attrs == INVALID_FILE_ATTRIBUTES)
				continue;
			sources.emplace_back(U(path), (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
		}
		for (const auto& s : sources) {
			WIN32_FILE_ATTRIBUTE_DATA a;
			uint64_t size = 0;
			if (!s.second && GetFileAttributesExW(LocalPath(s.first).c_str(), GetFileExInfoStandard, &a))
				size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
			m_queue->Add(Direction::Upload, s.first, s.second, size, target);
		}
	} else {
		TransferPaths(kRemote, m_remoteDrag, target);
		m_remoteDrag.clear();
	}
	return DROPEFFECT_COPY;
}

// --- queue -------------------------------------------------------------------------

void FileManagerWindow::RefreshQueue()
{
	const size_t before = m_queueItems.size();
	m_queueItems = m_queue->Snapshot();
	if (m_queueItems.size() != before)
		ListView_SetItemCountEx(m_queueList, static_cast<int>(m_queueItems.size()), LVSICF_NOSCROLL);
	InvalidateRect(m_queueList, NULL, FALSE);
	// Summary line.
	size_t running = 0, waiting = 0;
	uint64_t total = 0, done = 0;
	for (const auto& t : m_queueItems) {
		if (t.state == ItemState::Running)
			++running;
		else if (t.state == ItemState::Queued)
			++waiting;
		if (t.state == ItemState::Running || t.state == ItemState::Queued) {
			total += t.totalBytes;
			done += t.doneBytes;
		}
	}
	std::wstring text;
	if (running || waiting) {
		text = std::to_wstring(running + waiting) + L" active";
		if (total)
			text += L"  \x2022  " + FormatSize(done) + L" of " + FormatSize(total);
	} else if (!m_queueItems.empty()) {
		text = L"All transfers finished";
	}
	SetWindowTextW(m_queueSummary, text.c_str());
	EnableWindow(m_btnCancel, running + waiting > 0);
	bool anyFailed = false;
	for (const auto& t : m_queueItems)
		anyFailed = anyFailed || t.state == ItemState::Failed;
	EnableWindow(m_btnRetry, anyFailed && m_connected);
	EnableWindow(m_btnClear, m_queueItems.size() > running + waiting);
}

void FileManagerWindow::QueueCustomDraw(NMLVCUSTOMDRAW* cd, LRESULT& result)
{
	switch (cd->nmcd.dwDrawStage) {
	case CDDS_PREPAINT:
		result = CDRF_NOTIFYITEMDRAW;
		return;
	case CDDS_ITEMPREPAINT:
		result = CDRF_NOTIFYSUBITEMDRAW;
		return;
	case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
		const size_t i = static_cast<size_t>(cd->nmcd.dwItemSpec);
		if (cd->iSubItem != 1 || i >= m_queueItems.size()) {
			result = CDRF_DODEFAULT;
			return;
		}
		const TransferItem& t = m_queueItems[i];
		RECT r;
		ListView_GetSubItemRect(m_queueList, static_cast<int>(i), 1, LVIR_BOUNDS, &r);
		HDC dc = cd->nmcd.hdc;
		const bool selected = ListView_GetItemState(m_queueList, static_cast<int>(i), LVIS_SELECTED) != 0;
		FillRect(dc, &r, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
		RECT bar = r;
		InflateRect(&bar, -Scale(4), -Scale(4));
		const double frac = t.state == ItemState::Done ? 1.0
			: t.totalBytes ? (std::min)(1.0, double(t.doneBytes) / double(t.totalBytes)) : 0.0;
		HBRUSH border = CreateSolidBrush(RGB(190, 190, 190));
		FrameRect(dc, &bar, border);
		DeleteObject(border);
		RECT fill = bar;
		InflateRect(&fill, -1, -1);
		fill.right = fill.left + static_cast<int>((fill.right - fill.left) * frac);
		const COLORREF color = t.state == ItemState::Failed ? RGB(218, 59, 1)
			: t.state == ItemState::Cancelled ? RGB(160, 160, 160) : RGB(6, 176, 37);
		HBRUSH b = CreateSolidBrush(color);
		if (fill.right > fill.left)
			FillRect(dc, &fill, b);
		DeleteObject(b);
		const std::wstring text = t.totalBytes || t.state == ItemState::Done
			? std::to_wstring(static_cast<int>(frac * 100)) + L"%" : std::wstring();
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(0, 0, 0));
		HGDIOBJ old = SelectObject(dc, m_font);
		DrawTextW(dc, text.c_str(), -1, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, old);
		result = CDRF_SKIPDEFAULT;
		return;
	}
	default:
		result = CDRF_DODEFAULT;
		return;
	}
}

ConflictChoice FileManagerWindow::AskConflict(const Conflict& c, bool& applyToAll)
{
	const std::wstring name = W(FileNamePart(c.targetPath));
	const std::wstring where = W(ParentPath(c.targetPath));
	const std::wstring instruction = L"\x201C" + name + L"\x201D already exists";
	const std::wstring content = std::wstring(c.direction == Direction::Upload ? L"On the remote computer, in " : L"In ") +
		where + L"\n\nCopying:\t" + FormatSize(c.sourceSize) + L", " + FormatTime(c.sourceTime) + L"\nExisting:\t" +
		FormatSize(c.targetSize) + L", " + FormatTime(c.targetTime) +
		(c.sourceTime && c.targetTime ? (c.sourceTime > c.targetTime ? L"  (older)" : c.sourceTime < c.targetTime ? L"  (newer)" : L"")
		                              : L"");
	const TASKDIALOG_BUTTON buttons[] = {
		{ 100, L"&Replace the file" }, { 101, L"&Skip this file" }, { 102, L"&Keep both (rename the new one)" },
	};
	TASKDIALOGCONFIG tdc = {};
	tdc.cbSize = sizeof(tdc);
	tdc.hwndParent = m_hwnd;
	tdc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
	tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
	tdc.pszWindowTitle = L"UltraVNC File Transfer";
	tdc.pszMainIcon = TD_WARNING_ICON;
	tdc.pszMainInstruction = instruction.c_str();
	tdc.pszContent = content.c_str();
	tdc.cButtons = 3;
	tdc.pButtons = buttons;
	tdc.nDefaultButton = 100;
	tdc.pszVerificationText = L"Do this for &all remaining conflicts";
	int button = IDCANCEL;
	BOOL all = FALSE;
	if (FAILED(TaskDialogIndirect(&tdc, &button, NULL, &all)))
		return ConflictChoice::Cancel;
	applyToAll = all != FALSE;
	switch (button) {
	case 100: return ConflictChoice::Overwrite;
	case 101: return ConflictChoice::Skip;
	case 102: return ConflictChoice::KeepBoth;
	default: return ConflictChoice::Cancel;
	}
}

// --- settings ------------------------------------------------------------------------

void FileManagerWindow::LoadSettings()
{
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
		return;
	WINDOWPLACEMENT wp = {};
	DWORD size = sizeof(wp);
	DWORD type = 0;
	if (RegQueryValueExW(key, L"Placement", NULL, &type, reinterpret_cast<BYTE*>(&wp), &size) == ERROR_SUCCESS &&
	    type == REG_BINARY && size == sizeof(wp) && wp.length == sizeof(wp)) {
		// Only if it is still on a monitor.
		if (MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL)) {
			if (wp.showCmd != SW_SHOWMAXIMIZED)
				wp.showCmd = SW_SHOWNORMAL;
			SetWindowPlacement(m_hwnd, &wp);
		}
	}
	DWORD v = 0;
	size = sizeof(v);
	if (RegQueryValueExW(key, L"Split", NULL, &type, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS && type == REG_DWORD)
		m_split = (std::max)(0.2, (std::min)(0.8, v / 1000.0));
	size = sizeof(v);
	if (RegQueryValueExW(key, L"QueueSplit", NULL, &type, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS &&
	    type == REG_DWORD)
		m_queueSplit = (std::max)(0.35, (std::min)(0.85, v / 1000.0));
	auto readString = [&](const std::wstring& name) {
		wchar_t buf[4096] = {};
		DWORD bytes = sizeof(buf) - sizeof(wchar_t);
		if (RegQueryValueExW(key, name.c_str(), NULL, &type, reinterpret_cast<BYTE*>(buf), &bytes) == ERROR_SUCCESS &&
		    type == REG_SZ)
			return U(buf);
		return std::string();
	};
	m_panes[kLocal].path = readString(L"LocalFolder");
	m_initialRemoteDir = readString(L"Remote:" + m_options.host);
	RegCloseKey(key);
}

void FileManagerWindow::SaveSettings()
{
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS)
		return;
	WINDOWPLACEMENT wp = {};
	wp.length = sizeof(wp);
	if (GetWindowPlacement(m_hwnd, &wp))
		RegSetValueExW(key, L"Placement", 0, REG_BINARY, reinterpret_cast<const BYTE*>(&wp), sizeof(wp));
	DWORD v = static_cast<DWORD>(m_split * 1000);
	RegSetValueExW(key, L"Split", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
	v = static_cast<DWORD>(m_queueSplit * 1000);
	RegSetValueExW(key, L"QueueSplit", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
	auto writeString = [&](const std::wstring& name, const std::string& value) {
		if (value.empty())
			return;
		const std::wstring w = W(value);
		RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()),
			static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
	};
	writeString(L"LocalFolder", m_panes[kLocal].path);
	writeString(L"Remote:" + m_options.host, m_panes[kRemote].path.empty() ? m_initialRemoteDir : m_panes[kRemote].path);
	RegCloseKey(key);
}
