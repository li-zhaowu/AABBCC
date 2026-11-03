// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Audio probe (docs/design.md section 5.1): can the server capture what is
// playing while the remote speakers are muted?
//
// Run it on the remote (server) machine, logged on as a normal user, with
// speakers or headphones connected:
//
//   cl /std:c++17 /EHsc audio_probe.cpp ole32.lib
//   audio_probe.exe
//
// A child process plays a quiet 440 Hz tone. For each way of muting, the
// program records with WASAPI loopback (whole mix) and with process
// loopback (only the child, Windows 10 2004+) and prints the level. A level
// near 0 means "muted AND not capturable" (useless); a clear level while
// muted means that way works. The original volume and mute settings are
// restored at the end (and on Ctrl+C).

#include <windows.h>
#include <audioclient.h>
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#else
// Windows SDK 10.0.19041 declarations (MinGW lacks the header).
typedef enum { AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1 } AUDIOCLIENT_ACTIVATION_TYPE;
typedef enum {
	PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
	PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;
typedef struct {
	DWORD TargetProcessId;
	PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
typedef struct {
	AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
	union {
		AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
	};
} AUDIOCLIENT_ACTIVATION_PARAMS;
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#endif

namespace {

template <class T>
void SafeRelease(T*& p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

// --- child: play a tone until killed ----------------------------------------

int PlayTone()
{
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IMMDeviceEnumerator* en = nullptr;
	IMMDevice* dev = nullptr;
	IAudioClient* client = nullptr;
	IAudioRenderClient* render = nullptr;
	WAVEFORMATEX* fmt = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void**>(&en))) ||
	    FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
	    FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&client))) ||
	    FAILED(client->GetMixFormat(&fmt)) ||
	    FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, fmt, NULL)) ||
	    FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))))
		return 1;
	// The mix format is 32 bit float in practice; anything else: give up.
	const bool isFloat = fmt->wBitsPerSample == 32;
	if (!isFloat)
		return 2;
	UINT32 bufferFrames = 0;
	client->GetBufferSize(&bufferFrames);
	client->Start();
	double phase = 0;
	const double step = 2 * 3.14159265358979 * 440.0 / fmt->nSamplesPerSec;
	for (;;) {
		UINT32 padding = 0;
		client->GetCurrentPadding(&padding);
		const UINT32 n = bufferFrames - padding;
		BYTE* data = nullptr;
		if (n > 0 && SUCCEEDED(render->GetBuffer(n, &data))) {
			float* f = reinterpret_cast<float*>(data);
			for (UINT32 i = 0; i < n; ++i) {
				const float v = static_cast<float>(0.1 * sin(phase)); // quiet: -20 dBFS
				phase += step;
				for (int c = 0; c < fmt->nChannels; ++c)
					*f++ = v;
			}
			render->ReleaseBuffer(n, 0);
		}
		Sleep(10);
	}
}

// --- recording -----------------------------------------------------------------

// Peak level (0..1) of 32 bit float frames.
double Peak(const BYTE* data, UINT32 frames, const WAVEFORMATEX* fmt)
{
	double peak = 0;
	const float* f = reinterpret_cast<const float*>(data);
	for (UINT32 i = 0; i < frames * fmt->nChannels; ++i)
		peak = (std::max)(peak, static_cast<double>(fabs(f[i])));
	return peak;
}

double Capture(IAudioClient* client, int ms)
{
	WAVEFORMATEX* fmt = nullptr;
	client->GetMixFormat(&fmt);
	IAudioCaptureClient* cap = nullptr;
	if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap)))) {
		CoTaskMemFree(fmt);
		return -1;
	}
	client->Start();
	double peak = 0;
	const DWORD end = GetTickCount() + ms;
	Sleep(300); // let the stream settle
	while (static_cast<int>(end - GetTickCount()) > 0) {
		UINT32 packet = 0;
		while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
			BYTE* data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			if (FAILED(cap->GetBuffer(&data, &frames, &flags, NULL, NULL)))
				break;
			if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
				peak = (std::max)(peak, Peak(data, frames, fmt));
			cap->ReleaseBuffer(frames);
		}
		Sleep(10);
	}
	client->Stop();
	cap->Release();
	CoTaskMemFree(fmt);
	return peak;
}

// Classic loopback: the whole mix of the default speakers.
double CaptureSystemLoopback(IMMDevice* dev, int ms)
{
	IAudioClient* client = nullptr;
	WAVEFORMATEX* fmt = nullptr;
	double r = -1;
	if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&client))) &&
	    SUCCEEDED(client->GetMixFormat(&fmt)) &&
	    SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, fmt, NULL)))
		r = Capture(client, ms);
	CoTaskMemFree(fmt);
	SafeRelease(client);
	return r;
}

// Process loopback (Windows 10 2004+): only the given process tree.
class ActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
	HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
	HRESULT result = E_FAIL;
	IAudioClient* client = nullptr;
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (riid == IID_IUnknown || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
			*ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
			return S_OK;
		}
		if (riid == __uuidof(IAgileObject)) {
			*ppv = static_cast<IAgileObject*>(this);
			return S_OK;
		}
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }
	STDMETHODIMP_(ULONG) Release() override { return 1; }
	STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override
	{
		HRESULT hr = E_FAIL;
		IUnknown* unk = nullptr;
		if (SUCCEEDED(op->GetActivateResult(&hr, &unk)) && SUCCEEDED(hr) && unk) {
			unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));
			unk->Release();
		}
		result = hr;
		SetEvent(done);
		return S_OK;
	}
};

double CaptureProcessLoopback(DWORD pid, int ms)
{
	typedef HRESULT(WINAPI* PFN)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
		IActivateAudioInterfaceAsyncOperation**);
	PFN activate = reinterpret_cast<PFN>(reinterpret_cast<void*>(
		GetProcAddress(LoadLibraryW(L"mmdevapi.dll"), "ActivateAudioInterfaceAsync")));
	if (!activate)
		return -2;
	AUDIOCLIENT_ACTIVATION_PARAMS params = {};
	params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	params.ProcessLoopbackParams.TargetProcessId = pid;
	params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
	PROPVARIANT pv = {};
	pv.vt = VT_BLOB;
	pv.blob.cbSize = sizeof(params);
	pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
	ActivationHandler h;
	IActivateAudioInterfaceAsyncOperation* op = nullptr;
	if (FAILED(activate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, &h, &op)))
		return -2;
	WaitForSingleObject(h.done, 5000);
	SafeRelease(op);
	if (FAILED(h.result) || !h.client)
		return -2;
	// Process loopback has no mix format; ask for 32 bit float stereo 48 kHz.
	WAVEFORMATEX fmt = {};
	fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
	fmt.nChannels = 2;
	fmt.nSamplesPerSec = 48000;
	fmt.wBitsPerSample = 32;
	fmt.nBlockAlign = 8;
	fmt.nAvgBytesPerSec = 48000 * 8;
	double r = -1;
	if (SUCCEEDED(h.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, &fmt, NULL))) {
		IAudioCaptureClient* cap = nullptr;
		if (SUCCEEDED(h.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap)))) {
			h.client->Start();
			double peak = 0;
			Sleep(300);
			const DWORD end = GetTickCount() + ms;
			while (static_cast<int>(end - GetTickCount()) > 0) {
				UINT32 packet = 0;
				while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
					BYTE* data = nullptr;
					UINT32 frames = 0;
					DWORD flags = 0;
					if (FAILED(cap->GetBuffer(&data, &frames, &flags, NULL, NULL)))
						break;
					if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
						peak = (std::max)(peak, Peak(data, frames, &fmt));
					cap->ReleaseBuffer(frames);
				}
				Sleep(10);
			}
			h.client->Stop();
			cap->Release();
			r = peak;
		}
	}
	h.client->Release();
	return r;
}

// The child's audio session (for per-program mute).
ISimpleAudioVolume* FindSessionVolume(IMMDevice* dev, DWORD pid)
{
	IAudioSessionManager2* mgr = nullptr;
	IAudioSessionEnumerator* en = nullptr;
	ISimpleAudioVolume* result = nullptr;
	if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&mgr))) ||
	    FAILED(mgr->GetSessionEnumerator(&en))) {
		SafeRelease(mgr);
		return nullptr;
	}
	int count = 0;
	en->GetCount(&count);
	for (int i = 0; i < count && !result; ++i) {
		IAudioSessionControl* c = nullptr;
		IAudioSessionControl2* c2 = nullptr;
		if (SUCCEEDED(en->GetSession(i, &c)) &&
		    SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&c2)))) {
			DWORD p = 0;
			if (SUCCEEDED(c2->GetProcessId(&p)) && p == pid)
				c->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&result));
		}
		SafeRelease(c2);
		SafeRelease(c);
	}
	en->Release();
	mgr->Release();
	return result;
}

IAudioEndpointVolume* g_endpoint = nullptr;
BOOL g_origMute = FALSE;
float g_origVolume = 1.0f;
HANDLE g_child = NULL;

void Restore()
{
	if (g_endpoint) {
		g_endpoint->SetMute(g_origMute, NULL);
		g_endpoint->SetMasterVolumeLevelScalar(g_origVolume, NULL);
	}
	if (g_child)
		TerminateProcess(g_child, 0);
}

BOOL WINAPI OnCtrl(DWORD)
{
	Restore();
	return FALSE;
}

std::string Verdict(double peak)
{
	if (peak == -2)
		return "not available (Windows 10 2004+ needed)";
	if (peak < 0)
		return "error";
	char buf[64];
	snprintf(buf, sizeof(buf), "%.3f  %s", peak, peak > 0.02 ? "CAPTURED" : "silent");
	return buf;
}

} // namespace

int main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--play") == 0)
		return PlayTone();

	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IMMDeviceEnumerator* en = nullptr;
	IMMDevice* dev = nullptr;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
			reinterpret_cast<void**>(&en))) ||
	    FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
	    FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, reinterpret_cast<void**>(&g_endpoint)))) {
		printf("No default playback device.\n");
		return 1;
	}
	g_endpoint->GetMute(&g_origMute);
	g_endpoint->GetMasterVolumeLevelScalar(&g_origVolume);
	SetConsoleCtrlHandler(&OnCtrl, TRUE);

	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(NULL, exe, MAX_PATH);
	std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --play";
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	if (!CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
		printf("Could not start the tone player.\n");
		return 1;
	}
	g_child = pi.hProcess;
	printf("Playing a quiet 440 Hz tone (you may hear it briefly)...\n\n");
	Sleep(800);

	const int ms = 1200;
	struct Row { const char* what; double system, process; };
	std::vector<Row> rows;

	g_endpoint->SetMute(FALSE, NULL);
	g_endpoint->SetMasterVolumeLevelScalar((std::max)(g_origVolume, 0.3f), NULL);
	rows.push_back({ "1. nothing muted (reference)", CaptureSystemLoopback(dev, ms), CaptureProcessLoopback(pi.dwProcessId, ms) });

	g_endpoint->SetMute(TRUE, NULL);
	rows.push_back({ "2. speakers muted", CaptureSystemLoopback(dev, ms), CaptureProcessLoopback(pi.dwProcessId, ms) });
	g_endpoint->SetMute(FALSE, NULL);

	g_endpoint->SetMasterVolumeLevelScalar(0.0f, NULL);
	rows.push_back({ "3. speaker volume 0", CaptureSystemLoopback(dev, ms), CaptureProcessLoopback(pi.dwProcessId, ms) });
	g_endpoint->SetMasterVolumeLevelScalar((std::max)(g_origVolume, 0.3f), NULL);

	ISimpleAudioVolume* session = FindSessionVolume(dev, pi.dwProcessId);
	if (session) {
		session->SetMute(TRUE, NULL);
		rows.push_back({ "4. only the playing program muted", CaptureSystemLoopback(dev, ms),
			CaptureProcessLoopback(pi.dwProcessId, ms) });
		session->SetMute(FALSE, NULL);
		session->Release();
	} else {
		rows.push_back({ "4. only the playing program muted", -1, -1 });
	}

	Restore();
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	printf("%-36s %-28s %s\n", "", "whole mix (loopback)", "program only (process loopback)");
	for (const Row& r : rows)
		printf("%-36s %-28s %s\n", r.what, Verdict(r.system).c_str(), Verdict(r.process).c_str());

	OSVERSIONINFOW v = {};
	v.dwOSVersionInfoSize = sizeof(v);
	typedef LONG(WINAPI* PFN_Rtl)(OSVERSIONINFOW*);
	PFN_Rtl rtl = reinterpret_cast<PFN_Rtl>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
	if (rtl)
		rtl(&v);
	printf("\nWindows %lu.%lu build %lu. Volume and mute were restored.\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
	printf("Please send this table back.\n");
	return 0;
}
