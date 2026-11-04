// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// End-to-end test of common/FileManServer, FileManClient and
// FileTransferQueue on Windows or Wine. The two ends talk through an
// in-process transport that, like the real extension channel, sends
// control messages before queued bulk data.

#include "../../common/FileManClient.h"
#include "../../common/FileManServer.h"
#include "../../common/FileTransferQueue.h"

#include <cstdio>
#include <cstring>
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

// One direction of the transport: a thread delivering messages, control
// before bulk.
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
	bool Send(ExtPriority prio, uint8_t subtype, const std::vector<uint8_t>& payload)
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		if (m_closed)
			return false;
		(prio == extmsg::kPrioBulk ? m_bulk : m_control).push_back({ subtype, payload });
		m_cond.notify_all();
		return true;
	}
	void Close()
	{
		std::lock_guard<std::mutex> lk(m_mutex);
		m_closed = true;
		m_control.clear();
		m_bulk.clear();
	}
	size_t Delivered() const { return m_delivered; }

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
			++m_delivered;
		}
	}
	Deliver m_deliver;
	std::mutex m_mutex;
	std::condition_variable m_cond;
	std::deque<std::pair<uint8_t, std::vector<uint8_t>>> m_control, m_bulk;
	bool m_stop = false;
	bool m_closed = false;
	std::atomic<size_t> m_delivered{0};
	std::thread m_thread;
};

static std::string g_base; // test folder, UTF-8 (contains non-ASCII)

static std::wstring W(const std::string& s) { return Utf8ToWide(s); }

static void WriteBytes(const std::string& path, const std::vector<uint8_t>& data)
{
	HANDLE h = CreateFileW(W(path).c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	DWORD done = 0;
	if (!data.empty())
		WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &done, NULL);
	CloseHandle(h);
}

static bool ReadBytes(const std::string& path, std::vector<uint8_t>& out)
{
	HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size;
	GetFileSizeEx(h, &size);
	out.resize(static_cast<size_t>(size.QuadPart));
	DWORD done = 0;
	bool ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &done, NULL) && done == out.size());
	CloseHandle(h);
	return ok;
}

static bool Exists(const std::string& path)
{
	return GetFileAttributesW(W(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

static uint64_t MTime(const std::string& path)
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &a))
		return 0;
	return FileTimeToU64(a.ftLastWriteTime);
}

static std::vector<uint8_t> RandomBytes(size_t n, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<uint8_t> v(n);
	for (auto& b : v)
		b = static_cast<uint8_t>(rng());
	return v;
}

// No "~uvnc-*.part" left anywhere below dir.
static int CountTempFiles(const std::string& dir)
{
	int n = 0;
	WIN32_FIND_DATAW fd;
	HANDLE f = FindFirstFileW(W(JoinPath(dir, "*")).c_str(), &fd);
	if (f == INVALID_HANDLE_VALUE)
		return 0;
	do {
		const std::string name = WideToUtf8(fd.cFileName);
		if (name == "." || name == "..")
			continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			n += CountTempFiles(JoinPath(dir, name));
		else if (name.rfind("~uvnc-", 0) == 0)
			++n;
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return n;
}

struct Rig {
	std::unique_ptr<FileManServer> server;
	std::unique_ptr<FileManClient> client;
	std::unique_ptr<Pipe> toServer, toClient;

	Rig()
	{
		ServerCallbacks sc;
		sc.send = [this](ExtPriority prio, uint8_t st, const std::vector<uint8_t>& p) { return toClient->Send(prio, st, p); };
		sc.log = [](int level, const std::string& m) { if (level < 1) std::printf("[server] %s\n", m.c_str()); };
		server.reset(new FileManServer(sc));
		ClientCallbacks cc;
		cc.send = [this](ExtPriority prio, uint8_t st, const std::vector<uint8_t>& p) { return toServer->Send(prio, st, p); };
		client.reset(new FileManClient(cc));
		// Small pieces of work so that windows and ordering are exercised.
		client->readChunk = 300 * 1024;
		client->readWindow = 3;
		client->writeWindow = 700 * 1024;
		toServer.reset(new Pipe([this](uint8_t st, std::vector<uint8_t>& p) { server->OnMessage(st, p); }));
		toClient.reset(new Pipe([this](uint8_t st, std::vector<uint8_t>& p) { client->OnMessage(st, p); }));
		server->Start();
		client->OnConnected();
	}
	~Rig()
	{
		client->OnDisconnected();
		server->Stop();
		toServer.reset();
		toClient.reset();
	}
	void Disconnect()
	{
		toServer->Close();
		toClient->Close();
		client->OnDisconnected();
	}
};

static void TestBasics()
{
	Rig rig;
	auto& c = *rig.client;

	// Roots.
	std::vector<Root> roots;
	auto w = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
	bool rootsDone = false;
	Result rootsResult;
	c.Roots([&](const Result& r, std::vector<Root>& rs) {
		std::lock_guard<std::mutex> lk(w->first);
		rootsResult = r;
		roots = rs;
		rootsDone = true;
		w->second.notify_all();
	});
	{
		std::unique_lock<std::mutex> lk(w->first);
		w->second.wait_for(lk, std::chrono::seconds(10), [&] { return rootsDone; });
	}
	CHECK(rootsResult.Ok());
	bool haveC = false;
	for (const Root& r : roots) {
		if (r.path == "C:\\")
			haveC = true;
		std::printf("root %s [%s] type %u\n", r.path.c_str(), r.label.c_str(), r.type);
	}
	CHECK(haveC);

	// Folders.
	const std::string dir = JoinPath(g_base, "sub");
	CHECK(c.MkDirSync(dir).Ok());
	CHECK(c.MkDirSync(dir).status == kErrExists);
	CHECK(c.MkDirSync(JoinPath(g_base, "no\\such\\parent")).status == kErrNotFound);
	CHECK(c.MkDirSync("relative\\path").status == kErrBadPath);
	CHECK(c.MkDirSync(g_base + "\\..\\x").status == kErrBadPath);
	CHECK(c.MkDirSync("C:\\").status == kErrBadPath);

	WriteBytes(JoinPath(dir, "a.txt"), { 'h', 'i' });
	std::vector<Entry> list;
	CHECK(c.ListSync(dir, list).Ok() && list.size() == 1 && list[0].name == "a.txt" && list[0].size == 2);
	CHECK(c.ListSync(JoinPath(g_base, "missing"), list).status == kErrNotFound);

	Entry st;
	CHECK(c.StatSync(JoinPath(dir, "a.txt"), st).Ok() && st.size == 2 && !st.IsDirectory());
	CHECK(c.StatSync(dir, st).Ok() && st.IsDirectory());
	CHECK(c.StatSync(JoinPath(dir, "nope"), st).status == kErrNotFound);

	// Rename.
	{
		auto done = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
		bool finished = false;
		Result rr;
		c.Rename(JoinPath(dir, "a.txt"), JoinPath(dir, "b.txt"), [&](const Result& r) {
			std::lock_guard<std::mutex> lk(done->first);
			rr = r;
			finished = true;
			done->second.notify_all();
		});
		std::unique_lock<std::mutex> lk(done->first);
		done->second.wait_for(lk, std::chrono::seconds(10), [&] { return finished; });
		CHECK(rr.Ok());
		CHECK(Exists(JoinPath(dir, "b.txt")) && !Exists(JoinPath(dir, "a.txt")));
	}

	// Paging: more entries than fit in one reply.
	const std::string many = JoinPath(g_base, "many");
	CreateDirectoryW(W(many).c_str(), NULL);
	for (int i = 0; i < 2500; ++i)
		WriteBytes(JoinPath(many, "f" + std::to_string(i)), {});
	CHECK(c.ListSync(many, list).Ok());
	CHECK(list.size() == 2500);

	// Recursive delete (read-only file inside).
	CreateDirectoryW(W(JoinPath(dir, "deep")).c_str(), NULL);
	CreateDirectoryW(W(JoinPath(dir, "deep\\deeper")).c_str(), NULL);
	WriteBytes(JoinPath(dir, "deep\\deeper\\ro.txt"), { 1 });
	SetFileAttributesW(W(JoinPath(dir, "deep\\deeper\\ro.txt")).c_str(), FILE_ATTRIBUTE_READONLY);
	CHECK(c.DeleteSync(dir).Ok());
	CHECK(!Exists(dir));
	CHECK(c.DeleteSync(many).Ok());
	CHECK(c.DeleteSync("C:\\").status == kErrBadPath);
	CHECK(c.DeleteSync(JoinPath(g_base, "gone")).status == kErrNotFound);
}

static void TestTransfers()
{
	Rig rig;
	auto& c = *rig.client;
	std::atomic<bool> noCancel{false};
	const std::string up = JoinPath(g_base, "up");
	const std::string down = JoinPath(g_base, "down");
	CreateDirectoryW(W(up).c_str(), NULL);
	CreateDirectoryW(W(down).c_str(), NULL);

	// Upload ~3 MB (not a multiple of any chunk size).
	const auto data = RandomBytes(3 * 1024 * 1024 + 12345, 1);
	const std::string local = JoinPath(down, "src.bin");
	WriteBytes(local, data);
	FILETIME old = U64ToFileTime(131000000000000000ULL);
	{
		HANDLE h = CreateFileW(W(local).c_str(), FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
		SetFileTime(h, NULL, NULL, &old);
		CloseHandle(h);
	}
	uint64_t lastProgress = 0;
	Result r = c.Upload(W(local), JoinPath(up, "copy.bin"), false, [&](uint64_t d) { lastProgress = d; }, noCancel);
	CHECK(r.Ok());
	if (!r.Ok())
		std::printf("upload: %s\n", r.Describe().c_str());
	CHECK(lastProgress == data.size());
	std::vector<uint8_t> got;
	CHECK(ReadBytes(JoinPath(up, "copy.bin"), got) && got == data);
	CHECK(MTime(JoinPath(up, "copy.bin")) == 131000000000000000ULL);
	CHECK(!(GetFileAttributesW(W(JoinPath(up, "copy.bin")).c_str()) & FILE_ATTRIBUTE_HIDDEN));

	// Exists without overwrite; with overwrite.
	CHECK(c.Upload(W(local), JoinPath(up, "copy.bin"), false, nullptr, noCancel).status == kErrExists);
	const auto data2 = RandomBytes(1000, 2);
	WriteBytes(local, data2);
	CHECK(c.Upload(W(local), JoinPath(up, "copy.bin"), true, nullptr, noCancel).Ok());
	CHECK(ReadBytes(JoinPath(up, "copy.bin"), got) && got == data2);
	WriteBytes(local, data);

	// Download back.
	r = c.Download(JoinPath(up, "copy.bin"), W(JoinPath(down, "back.bin")), false, nullptr, noCancel);
	CHECK(r.Ok());
	CHECK(ReadBytes(JoinPath(down, "back.bin"), got) && got == data2);
	WriteBytes(JoinPath(up, "big.bin"), data);
	r = c.Download(JoinPath(up, "big.bin"), W(JoinPath(down, "big.bin")), false, nullptr, noCancel);
	CHECK(r.Ok() && ReadBytes(JoinPath(down, "big.bin"), got) && got == data);
	CHECK(c.Download(JoinPath(up, "big.bin"), W(JoinPath(down, "big.bin")), false, nullptr, noCancel).status == kErrExists);
	CHECK(c.Download(JoinPath(up, "missing.bin"), W(JoinPath(down, "m.bin")), false, nullptr, noCancel).status == kErrNotFound);
	CHECK(c.Download(up, W(JoinPath(down, "dir.bin")), false, nullptr, noCancel).status != kOk);

	// Empty file both ways.
	WriteBytes(JoinPath(down, "empty"), {});
	CHECK(c.Upload(W(JoinPath(down, "empty")), JoinPath(up, "empty"), false, nullptr, noCancel).Ok());
	CHECK(Exists(JoinPath(up, "empty")));
	CHECK(c.Download(JoinPath(up, "empty"), W(JoinPath(down, "empty2")), false, nullptr, noCancel).Ok());
	CHECK(ReadBytes(JoinPath(down, "empty2"), got) && got.empty());

	// Cancel in the middle: nothing half-written stays behind.
	std::atomic<bool> cancel{false};
	r = c.Download(JoinPath(up, "big.bin"), W(JoinPath(down, "cancelled.bin")), false,
		[&](uint64_t d) { if (d > 500000) cancel = true; }, cancel);
	CHECK(r.status == kErrCancelled);
	CHECK(!Exists(JoinPath(down, "cancelled.bin")));
	cancel = false;
	r = c.Upload(W(local), JoinPath(up, "cancelled.bin"), false, [&](uint64_t d) { if (d > 500000) cancel = true; }, cancel);
	CHECK(r.status == kErrCancelled);
	Sleep(300); // the server deletes the temporary file
	CHECK(!Exists(JoinPath(up, "cancelled.bin")));
	CHECK(CountTempFiles(g_base) == 0);

	// Still usable after cancels (no stale handles).
	CHECK(c.Download(JoinPath(up, "big.bin"), W(JoinPath(down, "again.bin")), false, nullptr, noCancel).Ok());

	// Disconnect during a transfer.
	r = c.Upload(W(local), JoinPath(up, "lost.bin"), false, [&](uint64_t d) { if (d > 400000) rig.Disconnect(); }, noCancel);
	CHECK(r.status == kErrDisconnected);
	CHECK(c.MkDirSync(JoinPath(g_base, "after")).status == kErrDisconnected);
}

static void TestQueue()
{
	Rig rig;
	const std::string src = JoinPath(g_base, "tree");
	const std::string dst = JoinPath(g_base, "remote");
	const std::string back = JoinPath(g_base, "back");
	CreateDirectoryW(W(src).c_str(), NULL);
	CreateDirectoryW(W(JoinPath(src, "x")).c_str(), NULL);
	CreateDirectoryW(W(JoinPath(src, "x\\y")).c_str(), NULL);
	CreateDirectoryW(W(JoinPath(src, "empty")).c_str(), NULL);
	WriteBytes(JoinPath(src, "1.txt"), RandomBytes(100, 3));
	WriteBytes(JoinPath(src, "x\\2.txt"), RandomBytes(400000, 4));
	WriteBytes(JoinPath(src, "x\\y\\3.txt"), RandomBytes(5, 5));
	CreateDirectoryW(W(dst).c_str(), NULL);
	CreateDirectoryW(W(back).c_str(), NULL);

	std::atomic<int> asked{0};
	ConflictChoice answer = ConflictChoice::Skip;
	bool answerAll = false;
	std::mutex fm;
	std::condition_variable fc;
	int finished = 0;
	QueueCallbacks qc;
	qc.askConflict = [&](const Conflict& c, bool& all) {
		++asked;
		std::printf("conflict: %s (%llu -> %llu bytes)\n", c.targetPath.c_str(),
			(unsigned long long)c.sourceSize, (unsigned long long)c.targetSize);
		all = answerAll;
		return answer;
	};
	qc.finished = [&](const TransferItem&) {
		std::lock_guard<std::mutex> lk(fm);
		++finished;
		fc.notify_all();
	};
	FileTransferQueue q(*rig.client, qc);
	auto waitFinished = [&](int n) {
		std::unique_lock<std::mutex> lk(fm);
		fc.wait_for(lk, std::chrono::seconds(60), [&] { return finished >= n; });
	};

	// Folder upload.
	q.Add(Direction::Upload, src, true, 0, dst);
	waitFinished(1);
	auto snap = q.Snapshot();
	CHECK(snap.size() == 1 && snap[0].state == ItemState::Done && snap[0].files == 3 && snap[0].filesDone == 3 &&
	      snap[0].totalBytes == 400105 && snap[0].doneBytes == 400105);
	std::vector<uint8_t> a, b;
	CHECK(ReadBytes(JoinPath(src, "x\\2.txt"), a) && ReadBytes(JoinPath(dst, "tree\\x\\2.txt"), b) && a == b);
	CHECK(Exists(JoinPath(dst, "tree\\empty")));
	CHECK(asked == 0);

	// Again: every file conflicts. Skip all.
	answer = ConflictChoice::Skip;
	answerAll = true;
	q.Add(Direction::Upload, src, true, 0, dst);
	waitFinished(2);
	snap = q.Snapshot();
	CHECK(snap[1].state == ItemState::Done && snap[1].filesSkipped == 3 && snap[1].filesDone == 0);
	CHECK(asked == 1); // "apply to all"

	// Keep both, asked each time.
	answer = ConflictChoice::KeepBoth;
	answerAll = false;
	asked = 0;
	q.Add(Direction::Upload, JoinPath(src, "1.txt"), false, 100, JoinPath(dst, "tree"));
	waitFinished(3);
	CHECK(asked == 1 && Exists(JoinPath(dst, "tree\\1 (2).txt")));

	// Folder download, then overwrite.
	q.Add(Direction::Download, JoinPath(dst, "tree"), true, 0, back);
	waitFinished(4);
	snap = q.Snapshot();
	CHECK(snap[3].state == ItemState::Done && snap[3].files == 4);
	CHECK(ReadBytes(JoinPath(back, "tree\\x\\y\\3.txt"), a) && ReadBytes(JoinPath(src, "x\\y\\3.txt"), b) && a == b);
	answer = ConflictChoice::Overwrite;
	answerAll = true;
	asked = 0;
	q.Add(Direction::Download, JoinPath(dst, "tree"), true, 0, back);
	waitFinished(5);
	snap = q.Snapshot();
	CHECK(snap[4].state == ItemState::Done && snap[4].filesDone == 4 && asked == 1);

	// A missing source fails cleanly.
	q.Add(Direction::Download, JoinPath(dst, "nothing"), false, 0, back);
	waitFinished(6);
	snap = q.Snapshot();
	CHECK(snap[5].state == ItemState::Failed && !snap[5].lastError.empty());

	// Cancel a queued item.
	q.Add(Direction::Upload, src, true, 0, dst);
	q.Add(Direction::Upload, src, true, 0, dst);
	snap = q.Snapshot();
	q.Cancel(snap.back().id);
	waitFinished(8);
	snap = q.Snapshot();
	CHECK(snap.back().state == ItemState::Cancelled);
	q.ClearFinished();
	CHECK(q.Snapshot().empty() && !q.Busy());
	CHECK(CountTempFiles(g_base) == 0);
	q.Shutdown();
}

int main()
{
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	g_base = JoinPath(WideToUtf8(tmp), "fileman e2e \xE6\xB5\x8B\xE8\xAF\x95"); // "测试"
	if (g_base.size() > 1 && g_base[g_base.size() - 1] == '\\')
		g_base.pop_back();
	{
		// Fresh test folder (left over from an aborted run).
		Rig rig;
		rig.client->DeleteSync(g_base);
	}
	CreateDirectoryW(W(g_base).c_str(), NULL);
	std::printf("test folder: %s\n", g_base.c_str());

	TestBasics();
	TestTransfers();
	TestQueue();

	// Clean up with our own delete.
	{
		Rig rig;
		rig.client->DeleteSync(g_base);
	}
	if (g_failures) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all fileman_e2e tests passed\n");
	return 0;
}
