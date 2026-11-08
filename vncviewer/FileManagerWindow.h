// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

// The new file transfer window (docs/design.md section 6): this computer on
// the left, the remote computer on the right, the transfer queue below.
//
//  - address bar (type a path), drop-down with drives and the user's
//    folders, Up / Refresh / New folder / Delete buttons;
//  - Explorer icons, sortable Name / Size / Type / Modified columns,
//    multi-select, ".." to go up;
//  - keys: Enter / double-click open, Backspace up, F2 rename, F5 refresh,
//    Del delete, F7 or Ctrl+Shift+N new folder, Ctrl+A select all, Tab
//    switches sides;
//  - right-click menu; drag between the two sides, and from Explorer onto
//    the remote side; local deletes go to the recycle bin;
//  - queue with progress, speed and time left per item; cancel; name
//    conflicts ask Replace / Skip / Keep both (optionally for all);
//  - per-monitor DPI aware; remembers its size, the split and the last
//    folders (the remote one per server).
//
// Runs on the viewer's UI thread. The window deletes itself when closed.

#pragma once

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/FileManClient.h"
#include "common/FileTransferQueue.h"

namespace fmdnd { class DropTarget; class RemoteSession; }

class FileManagerWindow {
public:
	struct Options {
		fileman::FileManClient* client = nullptr;
		std::wstring host;               // title; key of the remembered remote folder
		bool fileMode = false;           // the window is the whole connection
		HICON icon = NULL;
		std::function<void()> onClosed;  // the window is gone (UI thread)
	};

	// Create and show. Returns null on failure.
	static FileManagerWindow* Open(const Options& options);

	void Activate();
	// The file channel came up (true) or went away (false).
	void SetConnected(bool connected);
	// Close without asking (the connection goes away).
	void Destroy();
	HWND Hwnd() const { return m_hwnd; }

private:
	enum PaneId { kLocal = 0, kRemote = 1 };

	struct Item {
		std::wstring name;
		bool dir = false;
		bool up = false;       // the ".." entry
		uint64_t size = 0;
		uint64_t mtime = 0;
		uint32_t attrs = 0;
		int icon = 0;
		std::wstring type;
	};
	struct Place {
		std::wstring label;
		std::string path;      // UTF-8
		uint64_t total = 0, free = 0;
		bool drive = false;
	};
	struct Pane {
		PaneId id = kLocal;
		HWND title = NULL, places = NULL, address = NULL, list = NULL, status = NULL, action = NULL;
		HWND btnUp = NULL, btnRefresh = NULL, btnNew = NULL, btnDelete = NULL;
		std::string path;      // current folder, UTF-8; empty = none yet
		std::vector<Item> items;
		std::vector<Place> places_;
		int sortColumn = 0;
		bool sortAscending = true;
		uint32_t loadSeq = 0;  // remote: newest listing request
		bool loading = false;
		std::string error;     // why the folder could not be shown
		std::wstring selectAfterLoad;
		bool editAfterLoad = false;
		fmdnd::DropTarget* dropTarget = nullptr;
		int dropHighlight = -1;
	};

	explicit FileManagerWindow(const Options& options);
	~FileManagerWindow();

	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
	static LRESULT CALLBACK EditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

	void CreateChildren();
	void CreatePane(Pane& p);
	void ApplyFonts();
	void Layout();
	int Scale(int v) const { return MulDiv(v, static_cast<int>(m_dpi), 96); }

	// Navigation and listing.
	void Navigate(Pane& p, const std::string& path, const std::wstring& select = std::wstring());
	void LoadLocal(Pane& p, const std::string& path, const std::wstring& select);
	void RequestRemote(Pane& p, const std::string& path, const std::wstring& select);
	void SetItems(Pane& p, std::vector<Item>&& items, const std::string& path);
	void Refresh(Pane& p);
	void GoUp(Pane& p);
	void OpenItem(Pane& p, int index);
	void SortItems(Pane& p);
	void UpdatePaneStatus(Pane& p);
	void UpdateButtons();
	void FillLocalPlaces(Pane& p);
	void FillPlacesCombo(Pane& p);
	void SelectPlaceFor(Pane& p);
	void RequestRemoteRoots();

	// Operations.
	std::vector<int> Selected(const Pane& p) const;
	void TransferSelected(const Pane& from);
	void TransferPaths(PaneId from, const std::vector<std::pair<std::string, bool>>& sources,
	                   const std::string& targetDir);
	void DeleteSelected(Pane& p);
	void NewFolder(Pane& p);
	void StartRename(Pane& p);
	void FinishRename(Pane& p, int index, const std::wstring& newName);
	void CopyPath(Pane& p);
	void ContextMenu(Pane& p, POINT screen);

	// Drag and drop.
	void BeginDrag(Pane& p);
	DWORD OnDrop(Pane& p, IDataObject* data, POINTL pt, bool drop);
	void SetDropHighlight(Pane& p, int index);

	// Queue list.
	void RefreshQueue();
	void QueueCustomDraw(NMLVCUSTOMDRAW* cd, LRESULT& result);
	fileman::ConflictChoice AskConflict(const fileman::Conflict& c, bool& applyToAll);

	LRESULT OnNotify(NMHDR* hdr);
	void OnListDispInfo(Pane& p, NMLVDISPINFOW* di);
	void OnCommand(int id, int code, HWND ctl);
	Pane* PaneOf(HWND ctl);
	Pane& Other(const Pane& p) { return p.id == kLocal ? m_panes[kRemote] : m_panes[kLocal]; }

	void LoadSettings();
	void SaveSettings();

	Options m_options;
	fileman::FileManClient* m_client;
	std::unique_ptr<fileman::FileTransferQueue> m_queue;
	HWND m_hwnd = NULL;
	HWND m_tooltip = NULL;
	HWND m_queueTitle = NULL, m_queueSummary = NULL, m_queueList = NULL, m_btnCancel = NULL, m_btnClear = NULL, m_btnRetry = NULL;
	Pane m_panes[2];
	bool m_connected = false;
	bool m_closing = false;
	std::vector<fileman::TransferItem> m_queueItems;  // snapshot shown in the list
	std::atomic<bool> m_queueDirty{false};

	UINT m_dpi = 96;
	HFONT m_font = NULL, m_boldFont = NULL, m_iconFont = NULL;
	double m_split = 0.5;       // left pane share of the width
	double m_queueSplit = 0.68; // panes share of the height
	int m_dragSplitter = 0;     // 0 none, 1 vertical, 2 horizontal
	RECT m_vSplitRect = {}, m_hSplitRect = {};
	std::map<std::wstring, std::pair<int, std::wstring>> m_typeCache; // ".ext" / "<dir>" -> icon, type
	HIMAGELIST m_smallIcons = NULL;
	std::string m_initialRemoteDir;
	uint32_t m_cookie;          // identifies our remote drags
	std::vector<std::pair<std::string, bool>> m_remoteDrag; // items being dragged from the remote side
	int m_pendingRemoteOps = 0;
	std::string m_remoteOpErrors;
	bool m_ole = false;
	// Streams of remote files dragged to Explorer; closed with the window.
	std::shared_ptr<fmdnd::RemoteSession> m_session;
};
