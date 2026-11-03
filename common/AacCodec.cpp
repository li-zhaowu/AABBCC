// This file is part of UltraVNC
// https://github.com/ultravnc/UltraVNC
// https://uvnc.com/
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: Copyright (C) 2002-2025 UltraVNC Team Members. All Rights Reserved.
//

#ifdef _WIN32

#include "AacCodec.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstring>
#include <mutex>

#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace audio {

namespace {

template <class T>
void SafeRelease(T*& p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

std::string Hr(const char* what, HRESULT hr)
{
	char buf[96];
	snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
	return buf;
}

bool StartMf(std::string& error)
{
	// COM for the calling thread (it never leaves, see VideoDecoder).
	static thread_local bool com = false;
	if (!com) {
		const HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		com = SUCCEEDED(co) || co == RPC_E_CHANGED_MODE;
	}
	static std::once_flag once;
	static bool ok = false;
	std::call_once(once, [] { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
	if (!ok)
		error = "Media Foundation is not available (Windows N without the Media Feature Pack?)";
	return ok;
}

IMFTransform* FindTransform(const GUID& category, const GUID& inSubtype, const GUID& outSubtype)
{
	MFT_REGISTER_TYPE_INFO in = { MFMediaType_Audio, inSubtype };
	MFT_REGISTER_TYPE_INFO out = { MFMediaType_Audio, outSubtype };
	IMFActivate** acts = nullptr;
	UINT32 count = 0;
	IMFTransform* mft = nullptr;
	if (SUCCEEDED(MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &count)) &&
	    count > 0)
		acts[0]->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(&mft));
	for (UINT32 i = 0; i < count; ++i)
		acts[i]->Release();
	CoTaskMemFree(acts);
	return mft;
}

int FrequencyIndex(int rate)
{
	static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 };
	for (int i = 0; i < 12; ++i)
		if (rates[i] == rate)
			return i;
	return 3;
}

// Pull every finished output sample out of a synchronous transform.
template <class F>
bool DrainTransform(IMFTransform* mft, F onBuffer, std::string& error)
{
	for (;;) {
		MFT_OUTPUT_STREAM_INFO si = {};
		mft->GetOutputStreamInfo(0, &si);
		IMFSample* sample = nullptr;
		const bool provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
		if (!provides) {
			IMFMediaBuffer* b = nullptr;
			if (FAILED(MFCreateMemoryBuffer((std::max)(si.cbSize, DWORD(16384)), &b))) {
				error = "MFCreateMemoryBuffer failed";
				return false;
			}
			MFCreateSample(&sample);
			sample->AddBuffer(b);
			b->Release();
		}
		MFT_OUTPUT_DATA_BUFFER out = {};
		out.pSample = sample;
		DWORD status = 0;
		const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
		if (out.pEvents)
			out.pEvents->Release();
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
			SafeRelease(sample);
			return true;
		}
		if (FAILED(hr)) {
			SafeRelease(sample);
			error = Hr("ProcessOutput", hr);
			return false;
		}
		IMFSample* result = out.pSample;
		IMFMediaBuffer* mb = nullptr;
		if (result && SUCCEEDED(result->ConvertToContiguousBuffer(&mb))) {
			BYTE* p = nullptr;
			DWORD len = 0;
			if (SUCCEEDED(mb->Lock(&p, NULL, &len))) {
				onBuffer(p, len);
				mb->Unlock();
			}
			mb->Release();
		}
		if (result != sample)
			SafeRelease(result);
		SafeRelease(sample);
	}
}

bool FeedTransform(IMFTransform* mft, const void* data, size_t size, std::string& error)
{
	IMFMediaBuffer* b = nullptr;
	if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &b))) {
		error = "MFCreateMemoryBuffer failed";
		return false;
	}
	BYTE* p = nullptr;
	b->Lock(&p, NULL, NULL);
	memcpy(p, data, size);
	b->Unlock();
	b->SetCurrentLength(static_cast<DWORD>(size));
	IMFSample* s = nullptr;
	MFCreateSample(&s);
	s->AddBuffer(b);
	b->Release();
	const HRESULT hr = mft->ProcessInput(0, s, 0);
	s->Release();
	if (FAILED(hr)) {
		error = Hr("ProcessInput", hr);
		return false;
	}
	return true;
}

} // namespace

std::vector<uint8_t> AacUserData(int sampleRate, int channels)
{
	// HEAACWAVEINFO after WAVEFORMATEX: payload type 0 (raw), profile and
	// level 0x29 (AAC profile L2), struct type 0, two reserved fields.
	std::vector<uint8_t> v(12, 0);
	v[2] = 0x29;
	// AudioSpecificConfig: object type 2 (LC), frequency index, channels.
	const int fi = FrequencyIndex(sampleRate);
	v.push_back(static_cast<uint8_t>((2 << 3) | (fi >> 1)));
	v.push_back(static_cast<uint8_t>(((fi & 1) << 7) | (channels << 3)));
	return v;
}

// --- encoder -----------------------------------------------------------------

bool AacEncoder::Open(int sampleRate, int channels, uint32_t kbps, std::string& error)
{
	Close();
	if (!StartMf(error))
		return false;
	if ((sampleRate != 44100 && sampleRate != 48000) || channels < 1 || channels > 2) {
		error = "unsupported audio format for AAC";
		return false;
	}
	m_mft = FindTransform(MFT_CATEGORY_AUDIO_ENCODER, MFAudioFormat_PCM, MFAudioFormat_AAC);
	if (!m_mft) {
		error = "no AAC encoder found";
		return false;
	}
	m_channels = channels;
	// The Microsoft AAC encoder takes 12000, 16000, 20000 or 24000 bytes/s.
	m_kbps = kbps <= 112 ? 96 : kbps <= 144 ? 128 : kbps <= 176 ? 160 : 192;

	// The output type must be set before the input type.
	IMFMediaType* outType = nullptr;
	MFCreateMediaType(&outType);
	outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
	outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
	outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
	outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_kbps * 1000 / 8);
	outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
	HRESULT hr = m_mft->SetOutputType(0, outType, 0);
	outType->Release();
	if (FAILED(hr)) {
		error = Hr("AAC encoder SetOutputType", hr);
		Close();
		return false;
	}
	IMFMediaType* inType = nullptr;
	MFCreateMediaType(&inType);
	inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
	inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
	inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
	inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * 2);
	hr = m_mft->SetInputType(0, inType, 0);
	inType->Release();
	if (FAILED(hr)) {
		error = Hr("AAC encoder SetInputType", hr);
		Close();
		return false;
	}

	// The decoder configuration as the encoder sees it.
	m_config = AacUserData(sampleRate, channels);
	IMFMediaType* cur = nullptr;
	if (SUCCEEDED(m_mft->GetOutputCurrentType(0, &cur))) {
		UINT32 size = 0;
		if (SUCCEEDED(cur->GetBlobSize(MF_MT_USER_DATA, &size)) && size > 0 && size <= 64) {
			m_config.resize(size);
			cur->GetBlob(MF_MT_USER_DATA, m_config.data(), size, NULL);
		}
		cur->Release();
	}
	m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	return true;
}

void AacEncoder::Close()
{
	SafeRelease(m_mft);
	m_config.clear();
}

bool AacEncoder::Drain(std::vector<std::vector<uint8_t>>& frames, std::string& error)
{
	return DrainTransform(m_mft, [&](const BYTE* p, DWORD n) {
		if (n > 0)
			frames.emplace_back(p, p + n);
	}, error);
}

bool AacEncoder::Encode(const int16_t* pcm, size_t sampleFrames, std::vector<std::vector<uint8_t>>& frames,
                        std::string& error)
{
	if (!m_mft) {
		error = "encoder not open";
		return false;
	}
	if (!FeedTransform(m_mft, pcm, sampleFrames * m_channels * sizeof(int16_t), error))
		return false;
	return Drain(frames, error);
}

// --- decoder -----------------------------------------------------------------

bool AacDecoder::Open(int sampleRate, int channels, const std::vector<uint8_t>& config, std::string& error)
{
	Close();
	if (!StartMf(error))
		return false;
	m_mft = FindTransform(MFT_CATEGORY_AUDIO_DECODER, MFAudioFormat_AAC, MFAudioFormat_PCM);
	if (!m_mft) {
		error = "no AAC decoder found";
		return false;
	}
	const std::vector<uint8_t> userData = config.size() >= 14 ? config : AacUserData(sampleRate, channels);
	IMFMediaType* inType = nullptr;
	MFCreateMediaType(&inType);
	inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
	inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
	inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
	inType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
	inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	inType->SetBlob(MF_MT_USER_DATA, userData.data(), static_cast<UINT32>(userData.size()));
	HRESULT hr = m_mft->SetInputType(0, inType, 0);
	inType->Release();
	if (FAILED(hr)) {
		error = Hr("AAC decoder SetInputType", hr);
		Close();
		return false;
	}
	// 16 bit PCM output.
	for (DWORD i = 0;; ++i) {
		IMFMediaType* t = nullptr;
		if (FAILED(m_mft->GetOutputAvailableType(0, i, &t))) {
			error = "the AAC decoder offers no 16 bit PCM output";
			Close();
			return false;
		}
		GUID sub;
		UINT32 bits = 0;
		const bool pcm16 = SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFAudioFormat_PCM &&
			SUCCEEDED(t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) && bits == 16;
		if (pcm16)
			hr = m_mft->SetOutputType(0, t, 0);
		t->Release();
		if (pcm16) {
			if (FAILED(hr)) {
				error = Hr("AAC decoder SetOutputType", hr);
				Close();
				return false;
			}
			break;
		}
	}
	m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	return true;
}

void AacDecoder::Close()
{
	SafeRelease(m_mft);
}

bool AacDecoder::Decode(const uint8_t* frame, size_t size, std::vector<int16_t>& pcm, std::string& error)
{
	if (!m_mft) {
		error = "decoder not open";
		return false;
	}
	if (!FeedTransform(m_mft, frame, size, error))
		return false;
	return DrainTransform(m_mft, [&](const BYTE* p, DWORD n) {
		const int16_t* s = reinterpret_cast<const int16_t*>(p);
		pcm.insert(pcm.end(), s, s + n / 2);
	}, error);
}

} // namespace audio

#endif // _WIN32
