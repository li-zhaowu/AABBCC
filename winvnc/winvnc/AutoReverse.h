#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <shellapi.h>

// Maintains one reverse connection (including while waiting at the repeater).
// The client owns the lease, so all disconnect and initialization-failure paths
// release it. This object must be destroyed before its vncServer.
class AutoReverseConnection {
public:
	explicit AutoReverseConnection(vncServer& server) : m_server(server)
	{
		bool enabled = settings->getAutoReverseEnabled();
		m_host = settings->getAutoReverseRepeaterHost();
		m_port = settings->getAutoReverseRepeaterPort();
		m_id = settings->getAutoReverseId();
		m_interval = settings->getAutoReverseReconnectInterval();
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (argv) {
			bool commandLine = false;
			for (int i = 1; i < argc; ++i)
				if (_wcsicmp(argv[i], L"-autoreverse") == 0)
					commandLine = true;
			if (commandLine) {
				enabled = true;
				for (int i = 1; i < argc; ++i) {
					if (_wcsicmp(argv[i], L"-repeater") == 0 && i + 1 < argc) {
						m_host = Narrow(argv[++i]);
						const size_t colon = m_host.rfind(':');
						if (colon != std::string::npos && m_host.find(':') == colon) {
							m_port = atoi(m_host.substr(colon + 1).c_str());
							m_host.resize(colon);
						}
					} else if (_wcsnicmp(argv[i], L"-id:", 4) == 0) {
						m_id = "ID:" + Narrow(argv[i] + 4);
					} else if (_wcsicmp(argv[i], L"-reconnect") == 0 && i + 1 < argc) {
						m_interval = _wtoi(argv[++i]);
					}
				}
			}
			LocalFree(argv);
		}
		if (!enabled)
			return;
		if (_strnicmp(m_id.c_str(), "ID:", 3) != 0)
			m_id = "ID:" + m_id;
		else
			m_id.replace(0, 3, "ID:");
		if (m_host.empty() || m_host.size() >= 255 || m_id.size() <= 3 ||
			m_id.size() >= 250 || m_port < 1 || m_port > 65535 ||
			m_interval < 1 || m_interval > 86400) {
			vnclog.Print(LL_INTERR, VNCLOG("auto reverse: invalid host, port, ID or retry interval\n"));
			return;
		}
		m_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (!m_stop)
			return;
		try {
			m_thread = std::thread([this] { Run(); });
		} catch (...) {
			CloseHandle(m_stop);
			m_stop = NULL;
			vnclog.Print(LL_INTERR, VNCLOG("auto reverse: cannot start worker\n"));
		}
	}

	~AutoReverseConnection()
	{
		if (m_stop) {
			SetEvent(m_stop);
			if (m_thread.joinable())
				m_thread.join();
			CloseHandle(m_stop);
		}
	}

	AutoReverseConnection(const AutoReverseConnection&) = delete;
	AutoReverseConnection& operator=(const AutoReverseConnection&) = delete;

private:
	static std::string Narrow(const wchar_t* text)
	{
		const int size = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
		if (size <= 0)
			return {};
		std::string result(size, '\0');
		WideCharToMultiByte(CP_ACP, 0, text, -1, &result[0], size, NULL, NULL);
		result.resize(size - 1);
		return result;
	}

	void Run()
	{
		while (WaitForSingleObject(m_stop, static_cast<DWORD>(m_interval) * 1000) == WAIT_TIMEOUT) {
			if (m_connected->load())
				continue;
			auto socket = std::make_unique<VSocket>();
			const bool connected = settings->getIPV6()
				? socket->CreateConnect(&m_host[0], m_port)
				: socket->Create() && socket->Connect(&m_host[0], m_port);
			if (!connected) {
				vnclog.Print(LL_INTINFO, VNCLOG("auto reverse: repeater connection failed, retrying\n"));
				continue;
			}
			char id[250] = {};
			memcpy(id, m_id.data(), m_id.size());
			socket->SetTimeout(5000);
			if (!socket->SendExact(id, sizeof(id)) || WaitForSingleObject(m_stop, 0) != WAIT_TIMEOUT)
				continue;
			socket->SetTimeout(0);
			m_connected->store(true);
			const vncClientId client = m_server.AddClient(socket.release(),
				!settings->getReverseAuthRequired(), TRUE, 0, NULL, &m_id[0],
				&m_host[0], m_port, TRUE, m_connected);
			if (client < 0)
				m_connected->store(false);
			else
				vnclog.Print(LL_INTINFO, VNCLOG("auto reverse: registered with repeater\n"));
		}
	}

	vncServer& m_server;
	std::string m_host, m_id;
	int m_port = 5500, m_interval = 5;
	HANDLE m_stop = NULL;
	std::thread m_thread;
	std::shared_ptr<std::atomic<bool>> m_connected = std::make_shared<std::atomic<bool>>(false);
};
