// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// UI smoke test of vncviewer/FileManagerWindow (see run.sh).

#include "../../common/FileManClient.h"
#include "../../common/FileManServer.h"
#include "FileManagerWindow.h"
#include "MediaSettings.h"

#include <cstdio>
#include <deque>

using namespace fileman;
using extmsg::ExtPriority;

// Transport with a slow bulk lane, so that the queue shows progress.
class Pipe {
public:
	using Deliver = std::function<void(uint8_t, std::vector<uint8_t>&)>;
	explicit Pipe(Deliver d) : m_deliver(std::move(d)), m_thread([this] { Main(); }) {}
	~Pipe()
	{
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_stop = true;
		}
		m_cond.notify_all();
		m_thread.join();
	}
	bool Send(ExtPriority prio, uint8_t st, const std::vector<uint8_t>& p)
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		(prio == extmsg::kPrioBulk ? m_bulk : m_control).push_back({ st, p });
		m_cond.notify_all();
		return true;
	}

private:
	void Main()
	{
		for (;;) {
			std::pair<uint8_t, std::vector<uint8_t>> m;
			bool bulk = false;
			{
				std::unique_lock<std::mutex> lk(m_mutex);
				m_cond.wait(lk, [&] { return m_stop || !m_control.empty() || !m_bulk.empty(); });
				if (m_stop)
					return;
				bulk = m_control.empty();
				auto& q = bulk ? m_bulk : m_control;
				m = std::move(q.front());
				q.pop_front();
			}
			if (bulk)
				Sleep(40); // ~6 MB/s
			m_deliver(m.first, m.second);
		}
	}
	Deliver m_deliver;
	std::mutex m_mutex;
	std::condition_variable m_cond;
	std::deque<std::pair<uint8_t, std::vector<uint8_t>>> m_control, m_bulk;
	bool m_stop = false;
	std::thread m_thread;
};

HINSTANCE m_hInstResDLL = NULL; // the viewer's resource module (UiText)
static std::wstring g_shot, g_dialogShot;
static HWND g_window;

static void WriteFileBytes(const std::wstring& path, size_t n)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	std::vector<char> buf(n, 'x');
	DWORD done = 0;
	if (n)
		WriteFile(h, buf.data(), static_cast<DWORD>(n), &done, NULL);
	CloseHandle(h);
}

static bool SaveBitmap(HWND hwnd, const std::wstring& path)
{
	RECT r;
	GetWindowRect(hwnd, &r);
	const int w = r.right - r.left, h = r.bottom - r.top;
	HDC screen = GetDC(NULL);
	HDC mem = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
	HGDIOBJ old = SelectObject(mem, bmp);
	BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
	SelectObject(mem, old);
	BITMAPINFOHEADER bi = {};
	bi.biSize = sizeof(bi);
	bi.biWidth = w;
	bi.biHeight = h;
	bi.biPlanes = 1;
	bi.biBitCount = 24;
	const DWORD stride = ((w * 3 + 3) & ~3);
	std::vector<BYTE> pixels(stride * h);
	GetDIBits(mem, bmp, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
	BITMAPFILEHEADER bf = {};
	bf.bfType = 0x4D42;
	bf.bfOffBits = sizeof(bf) + sizeof(bi);
	bf.bfSize = bf.bfOffBits + static_cast<DWORD>(pixels.size());
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD done;
	WriteFile(f, &bf, sizeof(bf), &done, NULL);
	WriteFile(f, &bi, sizeof(bi), &done, NULL);
	WriteFile(f, pixels.data(), static_cast<DWORD>(pixels.size()), &done, NULL);
	CloseHandle(f);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(NULL, screen);
	return true;
}

// Photographs the media settings dialog (from inside its modal loop) and
// closes it.
static void CALLBACK DialogTimer(HWND, UINT, UINT_PTR id, DWORD)
{
	KillTimer(NULL, id);
	HWND dlg = GetActiveWindow();
	if (dlg) {
		SaveBitmap(dlg, g_dialogShot);
		std::printf("media settings dialog saved\n");
		PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
	}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	g_shot = argc > 1 ? argv[1] : L"fm_window.bmp";
	g_dialogShot = argc > 2 ? argv[2] : L"media_settings.bmp";
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
	InitCommonControlsEx(&icc);

	// Two folders with some content.
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring base = std::wstring(tmp) + L"fm window test";
	const std::wstring local = base + L"\\local", remote = base + L"\\remote \x8FDC\x7A0B"; // 远程
	CreateDirectoryW(base.c_str(), NULL);
	CreateDirectoryW(local.c_str(), NULL);
	CreateDirectoryW(remote.c_str(), NULL);
	CreateDirectoryW((local + L"\\Projects").c_str(), NULL);
	CreateDirectoryW((remote + L"\\Reports 2025").c_str(), NULL);
	WriteFileBytes(local + L"\\video.mp4", 30 * 1024 * 1024);
	WriteFileBytes(local + L"\\notes.txt", 1234);
	WriteFileBytes(local + L"\\photo.jpg", 250000);
	WriteFileBytes(remote + L"\\budget.xlsx", 45678);
	WriteFileBytes(remote + L"\\\x5408\x540C.docx", 98765); // 合同.docx
	DeleteFileW((remote + L"\\video.mp4").c_str());
	HKEY key;
	RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\UltraVNC\\Viewer\\FileTransferWindow", 0, NULL, 0, KEY_WRITE, NULL,
		&key, NULL);
	RegSetValueExW(key, L"LocalFolder", 0, REG_SZ, reinterpret_cast<const BYTE*>(local.c_str()),
		static_cast<DWORD>((local.size() + 1) * 2));
	const std::wstring rv = L"Remote:test-server";
	RegSetValueExW(key, rv.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(remote.c_str()),
		static_cast<DWORD>((remote.size() + 1) * 2));
	RegDeleteValueW(key, L"Placement");
	RegCloseKey(key);

	std::unique_ptr<FileManServer> server;
	std::unique_ptr<FileManClient> client;
	std::unique_ptr<Pipe> toServer, toClient;
	ServerCallbacks sc;
	sc.send = [&](ExtPriority p, uint8_t st, const std::vector<uint8_t>& d) { return toClient->Send(p, st, d); };
	server.reset(new FileManServer(sc));
	ClientCallbacks cc;
	cc.send = [&](ExtPriority p, uint8_t st, const std::vector<uint8_t>& d) { return toServer->Send(p, st, d); };
	client.reset(new FileManClient(cc));
	toServer.reset(new Pipe([&](uint8_t st, std::vector<uint8_t>& d) { server->OnMessage(st, d); }));
	toClient.reset(new Pipe([&](uint8_t st, std::vector<uint8_t>& d) { client->OnMessage(st, d); }));
	server->Start();
	client->OnConnected();

	bool closed = false;
	FileManagerWindow::Options o;
	o.client = client.get();
	o.host = L"test-server";
	o.onClosed = [&] { closed = true; PostQuitMessage(0); };
	FileManagerWindow* win = FileManagerWindow::Open(o);
	if (!win) {
		std::fprintf(stderr, "window creation failed\n");
		return 1;
	}
	g_window = win->Hwnd();
	SetWindowPos(g_window, NULL, 20, 20, 1200, 760, SWP_NOZORDER);

	// Script: select the video in the local pane and upload it, then take
	// the picture while it is being sent, then close.
	SetTimer(NULL, 1, 1500, NULL);
	int step = 0;
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0)) {
		if (msg.message == WM_TIMER && msg.hwnd == NULL) {
			++step;
			if (step == 1) {
				HWND list = GetDlgItem(g_window, 1004);
				const int n = ListView_GetItemCount(list);
				std::printf("local items: %d, remote items: %d\n", n, ListView_GetItemCount(GetDlgItem(g_window, 1104)));
				for (int i = 0; i < n; ++i) {
					wchar_t text[260] = {};
					ListView_GetItemText(list, i, 0, text, 260);
					if (wcscmp(text, L"video.mp4") == 0 || wcscmp(text, L"Projects") == 0)
						ListView_SetItemState(list, i, LVIS_SELECTED, LVIS_SELECTED);
				}
				SendMessageW(g_window, WM_COMMAND, MAKEWPARAM(1006, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(g_window, 1006)));
				SetTimer(NULL, 1, 1200, NULL);
			} else if (step == 2) {
				SaveBitmap(g_window, g_shot);
				std::printf("screenshot saved\n");
				std::printf("queue items: %d\n", ListView_GetItemCount(GetDlgItem(g_window, 1301)));
				SetTimer(NULL, 1, 3000, NULL);
			} else if (step == 3) {
				SetTimer(NULL, 0, 800, &DialogTimer);
				const bool ok = ShowMediaSettingsDialog(g_window);
				std::printf("media settings dialog closed (%s)\n", ok ? "OK" : "cancel");
				SetTimer(NULL, 1, 100, NULL);
			} else if (step == 4) {
				// Close without the "transfers running" question.
				win->Destroy();
			}
			continue;
		}
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	std::printf("closed: %s\n", closed ? "yes" : "no");
	DWORD attrs = GetFileAttributesW((remote + L"\\video.mp4").c_str());
	std::printf("uploaded video present: %s\n", attrs != INVALID_FILE_ATTRIBUTES ? "yes" : "no");
	client->OnDisconnected();
	server->Stop();
	toServer.reset();
	toClient.reset();
	return closed ? 0 : 1;
}
