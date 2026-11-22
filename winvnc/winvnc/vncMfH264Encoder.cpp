// This file is part of UltraVNC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vncMfH264Encoder.h"

#include <d3d10.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <codecapi.h>
#include <wrl/client.h>

#include <cstdio>

#ifdef _MSC_VER
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

using Microsoft::WRL::ComPtr;

namespace {

struct MediaFoundation {
	const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const HRESULT status = SUCCEEDED(com) || com == RPC_E_CHANGED_MODE
		? MFStartup(MF_VERSION, MFSTARTUP_LITE) : com;

	~MediaFoundation()
	{
		if (SUCCEEDED(status))
			MFShutdown();
		if (SUCCEEDED(com))
			CoUninitialize();
	}
};

bool Fail(std::string& error, const char* operation, HRESULT hr)
{
	char message[128];
	snprintf(message, sizeof(message), "%s failed (0x%08lx)", operation, static_cast<unsigned long>(hr));
	error = message;
	return false;
}

struct HardwareEncoders {
	IMFActivate** entries = nullptr;
	UINT32 count = 0;

	~HardwareEncoders()
	{
		for (UINT32 i = 0; i < count; ++i)
			entries[i]->Release();
		CoTaskMemFree(entries);
	}

	HRESULT Find(const LUID& adapter)
	{
		// Resolve at runtime so the NVENC path still loads on older Windows.
		using EnumFunction = decltype(&MFTEnum2);
		static const auto enumerate = reinterpret_cast<EnumFunction>(
			reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"mfplat.dll"), "MFTEnum2")));
		if (!enumerate)
			return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
		ComPtr<IMFAttributes> attributes;
		HRESULT hr = MFCreateAttributes(&attributes, 1);
		if (SUCCEEDED(hr))
			hr = attributes->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapter), sizeof(adapter));
		if (FAILED(hr))
			return hr;
		const MFT_REGISTER_TYPE_INFO input = { MFMediaType_Video, MFVideoFormat_NV12 };
		const MFT_REGISTER_TYPE_INFO output = { MFMediaType_Video, MFVideoFormat_H264 };
		return enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
			&input, &output, attributes.Get(), &entries, &count);
	}
};

HRESULT SetUInt32(ICodecAPI* api, const GUID& property, uint32_t value)
{
	VARIANT setting = {};
	setting.vt = VT_UI4;
	setting.ulVal = value;
	return api->SetValue(&property, &setting);
}

HRESULT SetVideoType(IMFTransform* encoder, bool output, UINT width, UINT height, UINT fps, uint32_t kbps)
{
	ComPtr<IMFMediaType> type;
	HRESULT hr = MFCreateMediaType(&type);
	if (FAILED(hr) ||
	    FAILED(hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
	    FAILED(hr = type->SetGUID(MF_MT_SUBTYPE, output ? MFVideoFormat_H264 : MFVideoFormat_NV12)) ||
	    FAILED(hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height)) ||
	    FAILED(hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1)) ||
	    FAILED(hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
	    FAILED(hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)))
		return hr;
	if (!output)
		return encoder->SetInputType(0, type.Get(), 0);
	if (FAILED(hr = type->SetUINT32(MF_MT_AVG_BITRATE, kbps * 1000)) ||
	    FAILED(hr = type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main)))
		return hr;
	return encoder->SetOutputType(0, type.Get(), 0);
}

// Start of the NAL header, or bytes.size() when no start code remains.
size_t NextNal(const std::vector<uint8_t>& bytes, size_t from)
{
	for (size_t i = from; i + 3 < bytes.size(); ++i)
		if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
			return i + 3;
	return bytes.size();
}

bool ReadNals(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps)
{
	bool idr = false;
	for (size_t nal = NextNal(bytes, 0); nal < bytes.size();) {
		const size_t next = NextNal(bytes, nal + 1);
		const uint8_t type = bytes[nal] & 0x1f;
		if (type == 5)
			idr = true;
		if (type == 7 || type == 8) {
			auto& parameterSet = type == 7 ? sps : pps;
			const size_t end = next == bytes.size() ? next : next - 3;
			parameterSet.assign(bytes.begin() + nal - 3, bytes.begin() + end);
		}
		nal = next;
	}
	return idr;
}

} // namespace

struct vncMfH264Encoder::Session {
	MediaFoundation mf;
	ComPtr<IMFActivate> activation;
	ComPtr<IMFTransform> encoder;
	ComPtr<IMFMediaEventGenerator> events;
	ComPtr<ICodecAPI> codec;
	ComPtr<IMFDXGIDeviceManager> manager;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11VideoDevice> videoDevice;
	ComPtr<ID3D11VideoContext> videoContext;
	ComPtr<ID3D11VideoProcessorEnumerator> formats;
	ComPtr<ID3D11VideoProcessor> processor;
	MFT_OUTPUT_STREAM_INFO output = {};
	UINT width = 0;
	UINT height = 0;
	LONGLONG duration = 0;
	UINT inputRequests = 0;
	std::vector<uint8_t> sps;
	std::vector<uint8_t> pps;

	~Session()
	{
		if (encoder) {
			encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
			encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
		}
		if (activation)
			activation->ShutdownObject();
	}

	HRESULT InitTransform(IMFActivate* entry, UINT fps, uint32_t kbps)
	{
		ComPtr<IMFAttributes> attributes;
		HRESULT hr = entry->ActivateObject(IID_PPV_ARGS(&encoder));
		if (FAILED(hr))
			return hr;
		activation = entry;
		if (FAILED(hr = encoder->GetAttributes(&attributes)) ||
		    FAILED(hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)) ||
		    FAILED(hr = encoder.As(&events)) ||
		    FAILED(hr = encoder.As(&codec)) ||
		    FAILED(hr = encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
				reinterpret_cast<ULONG_PTR>(manager.Get()))))
			return hr;

		VARIANT lowLatency = {};
		lowLatency.vt = VT_BOOL;
		lowLatency.boolVal = VARIANT_TRUE;
		codec->SetValue(&CODECAPI_AVLowLatencyMode, &lowLatency);
		SetUInt32(codec.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
		SetUInt32(codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);

		if (FAILED(hr = SetVideoType(encoder.Get(), true, width, height, fps, kbps)) ||
		    FAILED(hr = SetVideoType(encoder.Get(), false, width, height, fps, kbps)) ||
		    FAILED(hr = encoder->GetOutputStreamInfo(0, &output)) ||
		    FAILED(hr = encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)))
			return hr;
		return encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	}

	HRESULT MakeInput(ID3D11Texture2D* source, uint64_t timestamp, IMFSample** sample)
	{
		D3D11_TEXTURE2D_DESC texture = {};
		texture.Width = width;
		texture.Height = height;
		texture.MipLevels = 1;
		texture.ArraySize = 1;
		texture.Format = DXGI_FORMAT_NV12;
		texture.SampleDesc.Count = 1;
		texture.BindFlags = D3D11_BIND_RENDER_TARGET;
		ComPtr<ID3D11Texture2D> nv12;
		ComPtr<ID3D11VideoProcessorInputView> inputView;
		ComPtr<ID3D11VideoProcessorOutputView> outputView;
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input = {};
		input.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC target = {};
		target.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		HRESULT hr = device->CreateTexture2D(&texture, nullptr, &nv12);
		if (FAILED(hr) ||
		    FAILED(hr = videoDevice->CreateVideoProcessorInputView(source, formats.Get(), &input, &inputView)) ||
		    FAILED(hr = videoDevice->CreateVideoProcessorOutputView(nv12.Get(), formats.Get(), &target, &outputView)))
			return hr;

		D3D11_VIDEO_PROCESSOR_STREAM stream = {};
		stream.Enable = TRUE;
		stream.pInputSurface = inputView.Get();
		ComPtr<IMFMediaBuffer> buffer;
		ComPtr<IMFSample> inputSample;
		if (FAILED(hr = videoContext->VideoProcessorBlt(processor.Get(), outputView.Get(), 0, 1, &stream)) ||
		    FAILED(hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12.Get(), 0, FALSE, &buffer)) ||
		    FAILED(hr = MFCreateSample(&inputSample)) ||
		    FAILED(hr = inputSample->AddBuffer(buffer.Get())) ||
		    FAILED(hr = inputSample->SetSampleTime(static_cast<LONGLONG>(timestamp) * 10)) ||
		    FAILED(hr = inputSample->SetSampleDuration(duration)))
			return hr;
		*sample = inputSample.Detach();
		return S_OK;
	}

	HRESULT WaitFor(MediaEventType wanted)
	{
		// Limit a stalled driver so closing the connection can join this thread.
		const ULONGLONG deadline = GetTickCount64() + 1000;
		do {
			ComPtr<IMFMediaEvent> event;
			HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
			if (hr == MF_E_NO_EVENTS_AVAILABLE) {
				Sleep(1);
				continue;
			}
			MediaEventType type;
			HRESULT status;
			if (FAILED(hr) || FAILED(hr = event->GetStatus(&status)) || FAILED(hr = event->GetType(&type)))
				return hr;
			if (FAILED(status))
				return status;
			if (type == wanted)
				return S_OK;
			if (type == METransformNeedInput)
				++inputRequests;
		} while (GetTickCount64() < deadline);
		return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
	}

	HRESULT ReadOutput(std::vector<uint8_t>& out)
	{
		ComPtr<IMFSample> sample;
		HRESULT hr;
		if (!(output.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
			ComPtr<IMFMediaBuffer> buffer;
			if (FAILED(hr = MFCreateSample(&sample)) ||
			    FAILED(hr = MFCreateAlignedMemoryBuffer(output.cbSize, output.cbAlignment ? output.cbAlignment - 1 : 0, &buffer)) ||
			    FAILED(hr = sample->AddBuffer(buffer.Get())))
				return hr;
		}
		MFT_OUTPUT_DATA_BUFFER data = {};
		data.pSample = sample.Get();
		DWORD status = 0;
		hr = encoder->ProcessOutput(0, 1, &data, &status);
		if (data.pEvents)
			data.pEvents->Release();
		if (data.pSample != sample.Get())
			sample.Attach(data.pSample);
		if (FAILED(hr))
			return hr;
		if (!sample)
			return E_UNEXPECTED;

		ComPtr<IMFMediaBuffer> buffer;
		BYTE* bytes = nullptr;
		DWORD length = 0;
		if (FAILED(hr = sample->ConvertToContiguousBuffer(&buffer)) ||
		    FAILED(hr = buffer->Lock(&bytes, nullptr, &length)))
			return hr;
		if (length)
			out.assign(bytes, bytes + length);
		buffer->Unlock();
		return S_OK;
	}

	HRESULT UpdateOutputType()
	{
		ComPtr<IMFMediaType> type;
		HRESULT hr = encoder->GetOutputAvailableType(0, 0, &type);
		if (FAILED(hr) || FAILED(hr = encoder->SetOutputType(0, type.Get(), 0)))
			return hr;
		return encoder->GetOutputStreamInfo(0, &output);
	}
};

vncMfH264Encoder::vncMfH264Encoder() = default;
vncMfH264Encoder::~vncMfH264Encoder() = default;

bool vncMfH264Encoder::IsAvailable()
{
	MediaFoundation mf;
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(mf.status) || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return false;
	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		if (FAILED(factory->EnumAdapters1(i, &adapter)))
			return false;
		DXGI_ADAPTER_DESC1 description = {};
		if (FAILED(adapter->GetDesc1(&description)) || description.VendorId != 0x8086)
			continue;
		HardwareEncoders list;
		if (SUCCEEDED(list.Find(description.AdapterLuid)) && list.count)
			return true;
	}
}

bool vncMfH264Encoder::Open(ID3D11Device* device, int width, int height, int fps, uint32_t kbps,
                           std::string& error)
{
	Close();
	if (!device || width < 2 || height < 2 || fps < 1 || !kbps)
		return Fail(error, "configure hardware H.264 encoder", E_INVALIDARG);
	auto session = std::make_unique<Session>();
	if (FAILED(session->mf.status))
		return Fail(error, "Media Foundation startup", session->mf.status);
	session->width = width & ~1;
	session->height = height & ~1;
	session->duration = 10000000 / fps;
	session->device = device;

	ComPtr<IDXGIDevice> dxgiDevice;
	ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC description = {};
	HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
	if (FAILED(hr) ||
	    FAILED(hr = dxgiDevice->GetAdapter(&adapter)) ||
	    FAILED(hr = adapter->GetDesc(&description)))
		return Fail(error, "read capture adapter", hr);
	HardwareEncoders list;
	if (FAILED(hr = list.Find(description.AdapterLuid)))
		return Fail(error, "enumerate hardware H.264 encoders", hr);
	if (!list.count) {
		error = "the capture GPU exposes no Media Foundation H.264 hardware encoder";
		return false;
	}

	ComPtr<ID3D10Multithread> multithread;
	ComPtr<ID3D11DeviceContext> context;
	device->GetImmediateContext(&context);
	UINT token = 0;
	if (FAILED(hr = context.As(&multithread)) ||
	    FAILED(hr = context.As(&session->videoContext)) ||
	    FAILED(hr = device->QueryInterface(IID_PPV_ARGS(&session->videoDevice))) ||
	    FAILED(hr = MFCreateDXGIDeviceManager(&token, &session->manager)))
		return Fail(error, "initialize D3D11 video encoding", hr);
	multithread->SetMultithreadProtected(TRUE);
	if (FAILED(hr = session->manager->ResetDevice(device, token)))
		return Fail(error, "set encoder D3D11 device", hr);

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC format = {};
	format.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	format.InputWidth = width;
	format.InputHeight = height;
	format.OutputWidth = session->width;
	format.OutputHeight = session->height;
	format.InputFrameRate = format.OutputFrameRate = { static_cast<UINT>(fps), 1 };
	format.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
	if (FAILED(hr = session->videoDevice->CreateVideoProcessorEnumerator(&format, &session->formats)) ||
	    FAILED(hr = session->videoDevice->CreateVideoProcessor(session->formats.Get(), 0, &session->processor)))
		return Fail(error, "create D3D11 BGRA-to-NV12 processor", hr);
	const RECT crop = { 0, 0, static_cast<LONG>(session->width), static_cast<LONG>(session->height) };
	session->videoContext->VideoProcessorSetStreamSourceRect(session->processor.Get(), 0, TRUE, &crop);
	session->videoContext->VideoProcessorSetStreamAutoProcessingMode(session->processor.Get(), 0, FALSE);

	for (UINT32 i = 0; i < list.count; ++i) {
		hr = session->InitTransform(list.entries[i], fps, kbps);
		if (SUCCEEDED(hr)) {
			m_session = std::move(session);
			return true;
		}
		if (session->activation)
			session->activation->ShutdownObject();
		session->activation.Reset();
		session->encoder.Reset();
		session->events.Reset();
		session->codec.Reset();
	}
	return Fail(error, "open hardware H.264 encoder", hr);
}

void vncMfH264Encoder::Close()
{
	m_session.reset();
}

bool vncMfH264Encoder::Encode(ID3D11Texture2D* source, bool forceIdr, uint64_t timestamp,
                             std::vector<uint8_t>& out, bool& isKeyframe, std::string& error)
{
	out.clear();
	isKeyframe = false;
	if (!m_session)
		return Fail(error, "encode H.264 frame", MF_E_NOT_INITIALIZED);
	auto& session = *m_session;
	ComPtr<IMFSample> sample;
	HRESULT hr = session.MakeInput(source, timestamp, &sample);
	if (FAILED(hr))
		return Fail(error, "convert desktop frame to NV12", hr);

	if (session.inputRequests)
		--session.inputRequests;
	else if (FAILED(hr = session.WaitFor(METransformNeedInput)))
		return Fail(error, "wait for hardware H.264 encoder input", hr);
	// A freshly started encoder supplies the first IDR without a request.
	if (forceIdr && !session.sps.empty() &&
	    FAILED(hr = SetUInt32(session.codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1)))
		return Fail(error, "request an H.264 keyframe", hr);
	if (FAILED(hr = session.encoder->ProcessInput(0, sample.Get(), 0)))
		return Fail(error, "submit frame to hardware H.264 encoder", hr);
	if (SUCCEEDED(hr = session.WaitFor(METransformHaveOutput))) {
		hr = session.ReadOutput(out);
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE && SUCCEEDED(hr = session.UpdateOutputType()) &&
		    SUCCEEDED(hr = session.WaitFor(METransformHaveOutput)))
			hr = session.ReadOutput(out);
	}
	if (FAILED(hr))
		return Fail(error, "read hardware H.264 output", hr);
	if (out.empty() || NextNal(out, 0) == out.size()) {
		error = "the hardware H.264 encoder returned no Annex B frame";
		return false;
	}

	std::vector<uint8_t> sps, pps;
	isKeyframe = ReadNals(out, sps, pps);
	const bool missingHeaders = sps.empty() || pps.empty();
	if (!sps.empty())
		session.sps = std::move(sps);
	if (!pps.empty())
		session.pps = std::move(pps);
	if (isKeyframe && missingHeaders) {
		if (session.sps.empty() || session.pps.empty()) {
			ComPtr<IMFMediaType> type;
			UINT8* bytes = nullptr;
			UINT32 length = 0;
			if (SUCCEEDED(session.encoder->GetOutputCurrentType(0, &type)) &&
			    SUCCEEDED(type->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &bytes, &length))) {
				std::vector<uint8_t> headers(bytes, bytes + length);
				CoTaskMemFree(bytes);
				ReadNals(headers, session.sps, session.pps);
			}
		}
		if (session.sps.empty() || session.pps.empty()) {
			error = "the hardware H.264 keyframe is missing SPS/PPS";
			return false;
		}
		out.insert(out.begin(), session.pps.begin(), session.pps.end());
		out.insert(out.begin(), session.sps.begin(), session.sps.end());
	}
	if (forceIdr && !isKeyframe) {
		error = "the hardware H.264 encoder did not produce the requested IDR";
		return false;
	}
	return true;
}

bool vncMfH264Encoder::SetBitrate(uint32_t kbps)
{
	return m_session && kbps &&
		SUCCEEDED(SetUInt32(m_session->codec.Get(), CODECAPI_AVEncCommonMeanBitRate, kbps * 1000));
}
