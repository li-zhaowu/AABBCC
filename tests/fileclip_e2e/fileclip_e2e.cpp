// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// End-to-end test of common/FileClipboard on Windows (or Wine): two engines in
// one process, wired back to back. Engine A is the source: the test puts real
// files on the clipboard (CF_HDROP), A offers them, B puts virtual files on
// the clipboard, and the test reads them back through OLE exactly like
// Explorer would (FILEGROUPDESCRIPTORW + FILECONTENTS IStreams).
//
// Build: tests/fileclip_e2e/run.sh (MinGW + Wine) or with MSVC:
//   cl /std:c++17 /EHsc /DUNICODE /D_UNICODE fileclip_e2e.cpp ..\..\common\FileClipboard.cpp
//      ..\..\common\FileClipProto.cpp ..\..\common\ExtChannel.cpp

#include "../../common/FileClipboard.h"

#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

using namespace fileclip;

static int g_failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
			             #cond);                                                 \
			++g_failures;                                                        \
		}                                                                        \
	} while (0)

static bool g_verbose = false;

struct Link {
	FileClipboard* peer = nullptr;
	bool dropOffers = false;          // keep this side from offering
	std::atomic<bool> up{true};
	std::atomic<uint64_t> dataBytes{0};
};

static Callbacks MakeCallbacks(const char* name, Link& link)
{
	Callbacks cb;
	cb.send = [&link](extmsg::ExtPriority, uint8_t subtype, const std::vector<uint8_t>& payload) {
		if (!link.up)
			return false;
		if (link.dropOffers && (subtype == kOffer || subtype == kRevoke))
			return true;
		if (subtype == kData)
			link.dataBytes += payload.size();
		std::vector<uint8_t> copy(payload);
		link.peer->OnMessage(subtype, copy);
		return true;
	};
	cb.log = [name](int level, const std::string& msg) {
		if (g_verbose || level == 0)
			std::fprintf(stderr, "[%s] %s\n", name, msg.c_str());
	};
	return cb;
}

template <class Pred>
static bool WaitFor(Pred p, int ms = 5000)
{
	for (int i = 0; i < ms / 10; ++i) {
		if (p())
			return true;
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);
		Sleep(10);
	}
	return p();
}

static std::vector<uint8_t> RandomBytes(size_t n, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<uint8_t> v(n);
	for (auto& b : v)
		b = static_cast<uint8_t>(rng());
	return v;
}

static bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD w = 0;
	BOOL ok = data.empty() || WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, NULL);
	CloseHandle(h);
	return ok && w == data.size();
}

static bool PutFilesOnClipboard(const std::vector<std::wstring>& files)
{
	size_t chars = 1;
	for (auto& f : files)
		chars += f.size() + 1;
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
	DROPFILES* df = static_cast<DROPFILES*>(GlobalLock(h));
	df->pFiles = sizeof(DROPFILES);
	df->fWide = TRUE;
	wchar_t* p = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
	for (auto& f : files) {
		memcpy(p, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
		p += f.size() + 1;
	}
	GlobalUnlock(h);
	if (!OpenClipboard(NULL))
		return false;
	EmptyClipboard();
	SetClipboardData(CF_HDROP, h);
	CloseClipboard();
	return true;
}

static bool PutTextOnClipboard(const wchar_t* text)
{
	size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
	memcpy(GlobalLock(h), text, bytes);
	GlobalUnlock(h);
	if (!OpenClipboard(NULL))
		return false;
	EmptyClipboard();
	SetClipboardData(CF_UNICODETEXT, h);
	CloseClipboard();
	return true;
}

static CLIPFORMAT Cf(const wchar_t* name)
{
	return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name));
}

static std::vector<FILEDESCRIPTORW> GetDescriptors(IDataObject* obj)
{
	std::vector<FILEDESCRIPTORW> out;
	FORMATETC fe = { Cf(CFSTR_FILEDESCRIPTORW), NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM m = {};
	if (FAILED(obj->GetData(&fe, &m)))
		return out;
	FILEGROUPDESCRIPTORW* g = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(m.hGlobal));
	for (UINT i = 0; i < g->cItems; ++i)
		out.push_back(g->fgd[i]);
	GlobalUnlock(m.hGlobal);
	ReleaseStgMedium(&m);
	return out;
}

static IStream* GetStream(IDataObject* obj, LONG index)
{
	FORMATETC fe = { Cf(CFSTR_FILECONTENTS), NULL, DVASPECT_CONTENT, index, TYMED_ISTREAM };
	STGMEDIUM m = {};
	if (FAILED(obj->GetData(&fe, &m)) || m.tymed != TYMED_ISTREAM)
		return nullptr;
	return m.pstm; // the reference is ours
}

static HRESULT ReadAll(IStream* s, std::vector<uint8_t>& out, ULONG chunk = 65536)
{
	out.clear();
	std::vector<uint8_t> buf(chunk);
	for (;;) {
		ULONG got = 0;
		HRESULT hr = s->Read(buf.data(), chunk, &got);
		if (FAILED(hr))
			return hr;
		out.insert(out.end(), buf.begin(), buf.begin() + got);
		if (got == 0 || hr == S_FALSE)
			return S_OK;
	}
}

static int IndexOf(const std::vector<FILEDESCRIPTORW>& d, const wchar_t* name)
{
	for (size_t i = 0; i < d.size(); ++i)
		if (wcscmp(d[i].cFileName, name) == 0)
			return static_cast<int>(i);
	return -1;
}

int wmain(int argc, wchar_t** argv)
{
	g_verbose = argc > 1 && wcscmp(argv[1], L"-v") == 0;
	OleInitialize(NULL);

	// Test tree in %TEMP%.
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring root = std::wstring(tmp) + L"uvnc_fileclip_e2e";
	const std::wstring dir = root + L"\\dir";
	CreateDirectoryW(root.c_str(), NULL);
	CreateDirectoryW(dir.c_str(), NULL);
	CreateDirectoryW((dir + L"\\empty").c_str(), NULL);
	CreateDirectoryW((dir + L"\\sub").c_str(), NULL);
	const auto small = RandomBytes(1000, 1);
	const auto big = RandomBytes(20 * 1024 * 1024 + 123, 2); // several read windows
	const auto nested = RandomBytes(300000, 3);
	const std::wstring chinese = L"\x4E2D\x6587.txt";
	CHECK(WriteFileBytes(root + L"\\small.txt", small));
	CHECK(WriteFileBytes(root + L"\\big.bin", big));
	CHECK(WriteFileBytes(dir + L"\\sub\\" + chinese, nested));
	CHECK(WriteFileBytes(dir + L"\\zero.dat", {}));

	Link toB, toA;
	FileClipboard a(MakeCallbacks("A", toB));
	FileClipboard b(MakeCallbacks("B", toA));
	toB.peer = &b;
	toA.peer = &a;
	toA.dropOffers = true; // only A acts as source in this test
	CHECK(a.Start());
	CHECK(b.Start());
	a.OnConnected();
	b.OnConnected();

	const CLIPFORMAT marker = Cf(L"UltraVNC FileClip");

	// --- 1. copy files on "A", paste on "B" -----------------------------------
	CHECK(PutFilesOnClipboard({ root + L"\\small.txt", root + L"\\big.bin", dir }));
	CHECK(WaitFor([&] { return IsClipboardFormatAvailable(marker) != FALSE; }));

	IDataObject* obj = nullptr;
	CHECK(SUCCEEDED(OleGetClipboard(&obj)) && obj);
	if (obj) {
		auto d = GetDescriptors(obj);
		if (g_verbose)
			for (auto& fd : d)
				std::fwprintf(stderr, L"  %ls (attr %lx)\n", fd.cFileName, fd.dwFileAttributes);
		CHECK(d.size() == 7); // small, big, dir, dir\empty, dir\sub, dir\sub\中文.txt, dir\zero.dat
		const int iSmall = IndexOf(d, L"small.txt");
		const int iBig = IndexOf(d, L"big.bin");
		const int iDir = IndexOf(d, L"dir");
		const int iEmpty = IndexOf(d, L"dir\\empty");
		const int iNested = IndexOf(d, (L"dir\\sub\\" + chinese).c_str());
		const int iZero = IndexOf(d, L"dir\\zero.dat");
		CHECK(iSmall >= 0 && iBig >= 0 && iDir >= 0 && iEmpty >= 0 && iNested >= 0 && iZero >= 0);
		if (iDir >= 0)
			CHECK(d[iDir].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
		if (iBig >= 0)
			CHECK(d[iBig].nFileSizeLow == big.size() && d[iBig].nFileSizeHigh == 0);

		std::vector<uint8_t> got;
		struct Case { int idx; const std::vector<uint8_t>* want; ULONG chunk; };
		const std::vector<uint8_t> none;
		Case cases[] = { { iSmall, &small, 65536 }, { iBig, &big, 1000000 },
		                 { iNested, &nested, 4096 }, { iZero, &none, 65536 } };
		for (const Case& c : cases) {
			if (c.idx < 0)
				continue;
			IStream* s = GetStream(obj, c.idx);
			CHECK(s != nullptr);
			if (!s)
				continue;
			auto t0 = std::chrono::steady_clock::now();
			CHECK(SUCCEEDED(ReadAll(s, got, c.chunk)));
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			CHECK(got == *c.want);
			if (g_verbose)
				std::fprintf(stderr, "  read %zu bytes in %lld ms\n", got.size(), static_cast<long long>(ms));
			STATSTG st = {};
			CHECK(SUCCEEDED(s->Stat(&st, STATFLAG_DEFAULT)));
			CHECK(st.cbSize.QuadPart == c.want->size());
			CoTaskMemFree(st.pwcsName);
			s->Release();
		}

		// Directories have no contents.
		if (iDir >= 0)
			CHECK(GetStream(obj, iDir) == nullptr);

		// Seek: backwards (restart) and forwards within the buffer.
		if (iBig >= 0) {
			IStream* s = GetStream(obj, iBig);
			CHECK(s != nullptr);
			if (s) {
				std::vector<uint8_t> buf(5000);
				ULONG n = 0;
				CHECK(SUCCEEDED(s->Read(buf.data(), 5000, &n)) && n == 5000);
				LARGE_INTEGER li;
				ULARGE_INTEGER pos;
				li.QuadPart = 15 * 1024 * 1024;
				CHECK(SUCCEEDED(s->Seek(li, STREAM_SEEK_SET, &pos)) && pos.QuadPart == 15ull * 1024 * 1024);
				CHECK(SUCCEEDED(s->Read(buf.data(), 5000, &n)) && n == 5000);
				CHECK(memcmp(buf.data(), big.data() + 15 * 1024 * 1024, 5000) == 0);
				li.QuadPart = 100;
				CHECK(SUCCEEDED(s->Seek(li, STREAM_SEEK_SET, &pos)) && pos.QuadPart == 100);
				CHECK(SUCCEEDED(s->Read(buf.data(), 5000, &n)) && n == 5000);
				CHECK(memcmp(buf.data(), big.data() + 100, 5000) == 0);
				li.QuadPart = 1000; // forward, inside the read-ahead buffer
				CHECK(SUCCEEDED(s->Seek(li, STREAM_SEEK_CUR, &pos)) && pos.QuadPart == 6100);
				CHECK(SUCCEEDED(s->Read(buf.data(), 5000, &n)) && n == 5000);
				CHECK(memcmp(buf.data(), big.data() + 6100, 5000) == 0);
				li.QuadPart = -10;
				CHECK(SUCCEEDED(s->Seek(li, STREAM_SEEK_END, &pos)) && pos.QuadPart == big.size() - 10);
				CHECK(SUCCEEDED(s->Read(buf.data(), 5000, &n)) && n == 10);
				CHECK(memcmp(buf.data(), big.data() + big.size() - 10, 10) == 0);
				s->Release();
			}
		}

		// Abandon a stream after the first read: the source must stop.
		if (iBig >= 0) {
			IStream* s = GetStream(obj, iBig);
			std::vector<uint8_t> buf(1000);
			ULONG n = 0;
			if (s) {
				s->Read(buf.data(), 1000, &n);
				const uint64_t before = toB.dataBytes;
				s->Release();
				Sleep(300);
				const uint64_t after = toB.dataBytes;
				Sleep(300);
				CHECK(toB.dataBytes == after); // nothing more after the cancel
				CHECK(after - before <= 9ull * 1024 * 1024); // at most one window
			}
		}

		// A source file that disappeared: the read fails instead of hanging.
		if (iSmall >= 0) {
			MoveFileW((root + L"\\small.txt").c_str(), (root + L"\\small.moved").c_str());
			IStream* s = GetStream(obj, iSmall);
			if (s) {
				CHECK(FAILED(ReadAll(s, got)));
				s->Release();
			}
			MoveFileW((root + L"\\small.moved").c_str(), (root + L"\\small.txt").c_str());
		}

		// Connection lost in the middle of a read.
		if (iBig >= 0) {
			IStream* s = GetStream(obj, iBig);
			if (s) {
				std::vector<uint8_t> buf(1000);
				ULONG n = 0;
				CHECK(SUCCEEDED(s->Read(buf.data(), 1000, &n)));
				toB.up = false;
				b.OnDisconnected();
				a.OnDisconnected();
				CHECK(FAILED(ReadAll(s, got)));
				s->Release();
			}
		}
		obj->Release();
	}
	// After the disconnect the virtual files are gone from the clipboard.
	CHECK(WaitFor([&] { return IsClipboardFormatAvailable(marker) == FALSE; }));

	// --- 2. reconnect; a hostile offer is filtered --------------------------------
	toB.up = true;
	a.OnConnected();
	b.OnConnected();
	CHECK(PutTextOnClipboard(L"hello")); // nothing to offer
	{
		Offer evil;
		evil.seq = 1000;
		const char* names[] = { "ok.txt", "../evil.txt", "C:/Windows/evil.dll", "sub/../../x", "nul.txt" };
		for (const char* nm : names) {
			Entry e;
			e.path = nm;
			e.size = 1;
			evil.entries.push_back(e);
		}
		auto msg = EncodeOffer(evil);
		b.OnMessage(kOffer, msg);
		CHECK(WaitFor([&] { return IsClipboardFormatAvailable(marker) != FALSE; }));
		IDataObject* o2 = nullptr;
		if (SUCCEEDED(OleGetClipboard(&o2)) && o2) {
			auto d = GetDescriptors(o2);
			CHECK(d.size() == 1 && wcscmp(d[0].cFileName, L"ok.txt") == 0);
			o2->Release();
		}
		// Revoke of that offer removes it again.
		auto rv = EncodeRevoke(1000);
		b.OnMessage(kRevoke, rv);
		CHECK(WaitFor([&] { return IsClipboardFormatAvailable(marker) == FALSE; }));
	}

	// --- 3. our own virtual files are never offered back --------------------------
	{
		const uint64_t before = toB.dataBytes;
		CHECK(PutFilesOnClipboard({ root + L"\\small.txt" }));
		CHECK(WaitFor([&] { return IsClipboardFormatAvailable(marker) != FALSE; }));
		Sleep(300);
		// B received A's offer and published it; A saw B's object (marker)
		// and must not have produced a second offer or any data.
		CHECK(toB.dataBytes == before);
	}

	b.Stop();
	a.Stop();
	CHECK(IsClipboardFormatAvailable(marker) == FALSE);

	OleUninitialize();
	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all fileclip_e2e tests passed\n");
	return 0;
}
