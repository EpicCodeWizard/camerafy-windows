// ds_probe: a minimal DirectShow consumer (built x64 and x86).
//   ds_probe                       list video input devices with their ffmpeg-style names
//   ds_probe --capture <substr> [--seconds N]
//                                  build filter -> counting renderer, run, report frames/fps
#include <streams.h>
#include <initguid.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

// Counts frames; no clock, so samples render the instant they arrive.
class CountingRenderer final : public CBaseRenderer {
public:
    explicit CountingRenderer(HRESULT* phr)
        : CBaseRenderer(GUID_NULL, NAME("ds_probe renderer"), nullptr, phr) {}
    HRESULT CheckMediaType(const CMediaType* mt) override {
        if (*mt->Type() != MEDIATYPE_Video) return E_FAIL;
        if (*mt->Subtype() == MEDIASUBTYPE_NV12 || *mt->Subtype() == MEDIASUBTYPE_YUY2) return S_OK;
        return E_FAIL;
    }
    HRESULT SetMediaType(const CMediaType* mt) override {
        nv12 = *mt->Subtype() == MEDIASUBTYPE_NV12;
        if (*mt->FormatType() == FORMAT_VideoInfo) {
            auto* v = (VIDEOINFOHEADER*)mt->Format();
            w = v->bmiHeader.biWidth;
            h = labs(v->bmiHeader.biHeight);
        }
        return S_OK;
    }
    HRESULT DoRenderSample(IMediaSample* s) override {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (!frames) first = t.QuadPart;
        last = t.QuadPart;
        ++frames;
        BYTE* p = nullptr;
        if (SUCCEEDED(s->GetPointer(&p)) && p && w && h) {
            // Mean luma of a sparse grid: proves pixels arrived, not just samples.
            uint64_t sum = 0, n = 0;
            for (long y = 0; y < h; y += 37)
                for (long x = 0; x < w; x += 41) {
                    sum += nv12 ? p[(size_t)y * w + x] : p[((size_t)y * w + x) * 2];
                    ++n;
                }
            lumaSum += n ? (double)sum / (double)n : 0;
        }
        return S_OK;
    }
    bool nv12 = true;
    long w = 0, h = 0;
    uint64_t frames = 0;
    double lumaSum = 0;
    LONGLONG first = 0, last = 0;
};

std::wstring propString(IPropertyBag* bag, const wchar_t* name) {
    VARIANT v;
    VariantInit(&v);
    std::wstring s;
    if (SUCCEEDED(bag->Read(name, &v, nullptr)) && v.vt == VT_BSTR) s = v.bstrVal;
    VariantClear(&v);
    return s;
}

struct Device {
    std::wstring name, display;
    IMoniker* moniker;
};

std::vector<Device> enumerate() {
    std::vector<Device> out;
    ICreateDevEnum* de = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&de)))
        return out;
    IEnumMoniker* em = nullptr;
    if (de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0) == S_OK) {
        IMoniker* m = nullptr;
        while (em->Next(1, &m, nullptr) == S_OK) {
            Device d{L"", L"", m};
            IPropertyBag* bag = nullptr;
            if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&bag))) {
                d.name = propString(bag, L"FriendlyName");
                bag->Release();
            }
            LPOLESTR disp = nullptr;
            if (SUCCEEDED(m->GetDisplayName(nullptr, nullptr, &disp))) {
                d.display = disp;
                CoTaskMemFree(disp);
            }
            out.push_back(d);
        }
        em->Release();
    }
    de->Release();
    return out;
}

// "@device:sw:{...}\{...}" -> "@device_sw_{...}\{...}", the form ffmpeg prints.
std::wstring ffmpegAlt(const std::wstring& disp) {
    std::wstring s = disp;
    if (s.rfind(L"@device:", 0) == 0) {
        s[7] = L'_';
        const size_t c = s.find(L':', 8);
        if (c != std::wstring::npos) s[c] = L'_';
    }
    return s;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring want;
    int seconds = 3;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--capture") && i + 1 < argc) want = argv[++i];
        else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) seconds = _wtoi(argv[++i]);
    }
    auto devs = enumerate();
    wprintf(L"ds_probe (%d-bit): %zu video input device(s)\n", (int)(sizeof(void*) * 8), devs.size());
    Device* chosen = nullptr;
    for (auto& d : devs) {
        wprintf(L"  \"%s\"\n      %s\n", d.name.c_str(), ffmpegAlt(d.display).c_str());
    }
    // Exact name first: "<x> Webcam" is a prefix of "<x> Webcam MediaFoundation (slow)".
    for (auto& d : devs)
        if (!want.empty() && !chosen && d.name == want) chosen = &d;
    for (auto& d : devs)
        if (!want.empty() && !chosen && d.name.find(want) != std::wstring::npos) chosen = &d;
    int rc = 0;
    if (!want.empty()) {
        if (!chosen) {
            wprintf(L"FAIL: no device matching \"%s\"\n", want.c_str());
            return 1;
        }
        IBaseFilter* src = nullptr;
        HRESULT hr = chosen->moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&src);
        if (FAILED(hr)) {
            wprintf(L"FAIL: BindToObject 0x%08lX\n", hr);
            return 1;
        }
        IGraphBuilder* g = nullptr;
        CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder, (void**)&g);
        HRESULT rhr = S_OK;
        auto* rend = new CountingRenderer(&rhr);
        rend->AddRef();
        g->AddFilter(src, L"source");
        g->AddFilter(rend, L"renderer");
        IEnumPins* ep = nullptr;
        IPin* out = nullptr;
        src->EnumPins(&ep);
        ep->Next(1, &out, nullptr);
        ep->Release();
        hr = g->ConnectDirect(out, rend->GetPin(0), nullptr);
        if (FAILED(hr)) {
            wprintf(L"FAIL: ConnectDirect 0x%08lX\n", hr);
            return 1;
        }
        IMediaFilter* mf = nullptr;
        g->QueryInterface(IID_IMediaFilter, (void**)&mf);
        mf->SetSyncSource(nullptr);  // render on arrival
        IMediaControl* mc = nullptr;
        g->QueryInterface(IID_IMediaControl, (void**)&mc);
        hr = mc->Run();
        Sleep((DWORD)seconds * 1000);
        mc->Stop();
        const double secs = rend->frames > 1 ? (double)(rend->last - rend->first) / 1e7 : 0;  // QPC is 10 MHz on Win10+
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const double s2 = rend->frames > 1 ? (double)(rend->last - rend->first) / (double)f.QuadPart : 0;
        (void)secs;
        wprintf(L"%s: %llu frames in %d s from \"%s\" (%s %ldx%ld), %.2f fps, mean luma %.1f\n",
                rend->frames > 10 ? L"PASS" : L"FAIL", (unsigned long long)rend->frames, seconds, chosen->name.c_str(),
                rend->nv12 ? L"NV12" : L"YUY2", rend->w, rend->h, s2 > 0 ? (rend->frames - 1) / s2 : 0.0,
                rend->frames ? rend->lumaSum / (double)rend->frames : 0.0);
        rc = rend->frames > 10 ? 0 : 1;
        mc->Release();
        mf->Release();
        out->Release();
        g->Release();
        src->Release();
        rend->Release();
    }
    for (auto& d : devs) d.moniker->Release();
    CoUninitialize();
    return rc;
}
