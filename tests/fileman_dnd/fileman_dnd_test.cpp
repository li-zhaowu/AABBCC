// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Test of the "virtual files" data object (vncviewer/FileManagerDragDrop):
// reads it the way Explorer does after a drop, against a real
// FileManServer through an in-process transport.

#include "../../common/FileManClient.h"
#include "../../common/FileManServer.h"
#include "FileManagerDragDrop.h"

#include <cstdio>
#include <deque>
#include <random>

using namespace fileman;
using extmsg::ExtPriority;

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

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
			{
				std::unique_lock<std::mutex> lk(m_mutex);
				m_cond.wait(lk, [&] { return m_stop || !m_control.empty() || !m_bulk.empty(); });
				if (m_stop)
					return;
				auto& q = !m_control.empty() ? m_control : m_bulk;
				m = std::move(q.front());
				q.pop_front();
			}
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

static std::wstring W(const std::string& s) { return Utf8ToWide(s); }

static void WriteBytes(const std::string& path, const std::vector<uint8_t>& data)
{
	HANDLE h = CreateFileW(W(path).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD done = 0;
	if (!data.empty())
		WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &done, NULL);
	CloseHandle(h);
}

static std::vector<uint8_t> RandomBytes(size_t n, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<uint8_t> v(n);
	for (auto& b : v)
		b = static_cast<uint8_t>(rng());
	return v;
}

static std::vector<uint8_t> ReadAll(IStream* s, ULONG chunk)
{
	std::vector<uint8_t> out, buf(chunk);
	for (;;) {
		ULONG got = 0;
		const HRESULT hr = s->Read(buf.data(), chunk, &got);
		out.insert(out.end(), buf.begin(), buf.begin() + got);
		if (FAILED(hr) || got == 0)
			break;
	}
	return out;
}

int main()
{
	OleInitialize(NULL);
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const std::string base = JoinPath(WideToUtf8(tmp), "fileman dnd \xE6\x8B\x96");
	CreateDirectoryW(W(base).c_str(), NULL);
	const std::string folder = JoinPath(base, "Reports");
	CreateDirectoryW(W(folder).c_str(), NULL);
	CreateDirectoryW(W(JoinPath(folder, "2025")).c_str(), NULL);
	CreateDirectoryW(W(JoinPath(folder, "empty")).c_str(), NULL);
	const auto big = RandomBytes(2 * 1024 * 1024 + 4321, 1);
	const auto small = RandomBytes(10, 2);
	WriteBytes(JoinPath(folder, "2025\\big.bin"), big);
	WriteBytes(JoinPath(folder, "a.txt"), small);
	WriteBytes(JoinPath(base, "single.txt"), small);

	std::unique_ptr<FileManServer> server;
	std::unique_ptr<FileManClient> client;
	std::unique_ptr<Pipe> toServer, toClient;
	ServerCallbacks sc;
	sc.send = [&](ExtPriority p, uint8_t st, const std::vector<uint8_t>& d) { return toClient->Send(p, st, d); };
	server.reset(new FileManServer(sc));
	ClientCallbacks cc;
	cc.send = [&](ExtPriority p, uint8_t st, const std::vector<uint8_t>& d) { return toServer->Send(p, st, d); };
	client.reset(new FileManClient(cc));
	client->readChunk = 300 * 1024;
	toServer.reset(new Pipe([&](uint8_t st, std::vector<uint8_t>& d) { server->OnMessage(st, d); }));
	toClient.reset(new Pipe([&](uint8_t st, std::vector<uint8_t>& d) { client->OnMessage(st, d); }));
	server->Start();
	client->OnConnected();

	// The reader on its own.
	{
		Result r;
		auto reader = client->OpenReader(JoinPath(folder, "2025\\big.bin"), r);
		CHECK(reader && reader->Size() == big.size());
		std::vector<uint8_t> got(big.size() + 100);
		size_t total = 0, n = 0;
		while (reader && reader->Read(got.data() + total, 70000, n).Ok() && n > 0)
			total += n;
		CHECK(total == big.size() && std::equal(big.begin(), big.end(), got.begin()));
		CHECK(!client->OpenReader(JoinPath(folder, "missing"), r) && r.status == kErrNotFound);
	}

	auto session = std::make_shared<fmdnd::RemoteSession>(client.get());
	std::vector<fmdnd::VirtualDataObject::Source> sources(2);
	sources[0].path = folder;
	sources[0].directory = true;
	sources[1].path = JoinPath(base, "single.txt");
	sources[1].size = small.size();
	auto* obj = new fmdnd::VirtualDataObject(session, sources);
	IDataObject* data = obj;

	const CLIPFORMAT cfDesc = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
	const CLIPFORMAT cfContents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
	FORMATETC fd = { cfDesc, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	CHECK(data->QueryGetData(&fd) == S_OK);
	STGMEDIUM m = {};
	CHECK(SUCCEEDED(data->GetData(&fd, &m)));
	std::vector<std::wstring> names;
	std::vector<bool> dirs;
	{
		auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(m.hGlobal));
		for (UINT i = 0; i < group->cItems; ++i) {
			names.push_back(group->fgd[i].cFileName);
			dirs.push_back((group->fgd[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
			std::printf("%u: %ls%s\n", i, group->fgd[i].cFileName, dirs.back() ? " (folder)" : "");
		}
		GlobalUnlock(m.hGlobal);
		ReleaseStgMedium(&m);
	}
	auto find = [&](const wchar_t* n) {
		for (size_t i = 0; i < names.size(); ++i)
			if (names[i] == n)
				return static_cast<int>(i);
		return -1;
	};
	CHECK(names.size() == 6);
	CHECK(find(L"Reports") >= 0 && dirs[find(L"Reports")]);
	CHECK(find(L"Reports\\empty") >= 0 && find(L"Reports\\2025") >= 0);
	const int bigIndex = find(L"Reports\\2025\\big.bin");
	const int singleIndex = find(L"single.txt");
	CHECK(bigIndex >= 0 && singleIndex >= 0 && find(L"Reports\\a.txt") >= 0);

	// File contents through IStream.
	FORMATETC fc = { cfContents, nullptr, DVASPECT_CONTENT, bigIndex, TYMED_ISTREAM };
	CHECK(SUCCEEDED(data->GetData(&fc, &m)) && m.tymed == TYMED_ISTREAM);
	{
		STATSTG st = {};
		CHECK(SUCCEEDED(m.pstm->Stat(&st, STATFLAG_DEFAULT)) && st.cbSize.QuadPart == big.size() &&
		      st.pwcsName && wcscmp(st.pwcsName, L"big.bin") == 0);
		CoTaskMemFree(st.pwcsName);
		LARGE_INTEGER zero = {};
		CHECK(m.pstm->Seek(zero, STREAM_SEEK_SET, nullptr) == S_OK);
		CHECK(ReadAll(m.pstm, 64 * 1024) == big);
		LARGE_INTEGER back;
		back.QuadPart = -10;
		CHECK(FAILED(m.pstm->Seek(back, STREAM_SEEK_CUR, nullptr)));
		ReleaseStgMedium(&m);
	}
	fc.lindex = singleIndex;
	CHECK(SUCCEEDED(data->GetData(&fc, &m)));
	CHECK(ReadAll(m.pstm, 3) == small);
	ReleaseStgMedium(&m);
	fc.lindex = find(L"Reports");
	CHECK(data->GetData(&fc, &m) == DV_E_LINDEX); // a folder has no contents
	fc.lindex = 99;
	CHECK(data->GetData(&fc, &m) == DV_E_LINDEX);

	// The window closes while Explorer still reads: errors, no crash.
	fc.lindex = bigIndex;
	CHECK(SUCCEEDED(data->GetData(&fc, &m)));
	BYTE buf[1000];
	ULONG got = 0;
	CHECK(m.pstm->Read(buf, sizeof(buf), &got) == S_OK && got == sizeof(buf));
	session->Shutdown();
	CHECK(m.pstm->Read(buf, sizeof(buf), &got) == STG_E_READFAULT && got == 0);
	ReleaseStgMedium(&m);
	CHECK(FAILED(data->GetData(&fc, &m)));
	data->Release();

	client->OnDisconnected();
	server->Stop();
	toServer.reset();
	toClient.reset();
	{
		// Clean up.
		SHFILEOPSTRUCTW op = {};
		std::wstring from = W(base) + L'\0';
		op.wFunc = FO_DELETE;
		op.pFrom = from.c_str();
		op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
		SHFileOperationW(&op);
	}
	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all fileman_dnd tests passed\n");
	return 0;
}
