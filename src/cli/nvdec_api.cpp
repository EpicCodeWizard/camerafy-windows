#include "nvdec_api.h"
#include <stdio.h>

namespace cf {

CudaApi g_cu;

namespace {

template <typename T>
bool sym(HMODULE m, const char* name, T*& out, std::string& err) {
    out = (T*)GetProcAddress(m, name);
    if (!out) {
        err += std::string(err.empty() ? "" : ", ") + name;
        return false;
    }
    return true;
}

}  // namespace

bool CudaApi::load(std::string& err) {
    if (cuda && cuvid) return true;
    cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) {
        err = "nvcuda.dll not found: no NVIDIA driver installed";
        return false;
    }
    cuvid = LoadLibraryExW(L"nvcuvid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuvid) {
        err = "nvcuvid.dll not found: the NVIDIA driver lacks NVDEC support";
        return false;
    }
    std::string missing;
    bool ok = true;
    ok &= sym(cuda, "cuInit", cuInit, missing);
    ok &= sym(cuda, "cuDriverGetVersion", cuDriverGetVersion, missing);
    ok &= sym(cuda, "cuDeviceGetCount", cuDeviceGetCount, missing);
    ok &= sym(cuda, "cuDeviceGet", cuDeviceGet, missing);
    ok &= sym(cuda, "cuDeviceGetName", cuDeviceGetName, missing);
    ok &= sym(cuda, "cuDeviceGetAttribute", cuDeviceGetAttribute, missing);
    ok &= sym(cuda, "cuCtxCreate_v2", cuCtxCreate, missing);
    ok &= sym(cuda, "cuCtxDestroy_v2", cuCtxDestroy, missing);
    ok &= sym(cuda, "cuCtxPushCurrent_v2", cuCtxPushCurrent, missing);
    ok &= sym(cuda, "cuCtxPopCurrent_v2", cuCtxPopCurrent, missing);
    ok &= sym(cuda, "cuCtxSetCurrent", cuCtxSetCurrent, missing);
    ok &= sym(cuda, "cuStreamCreate", cuStreamCreate, missing);
    ok &= sym(cuda, "cuStreamDestroy_v2", cuStreamDestroy, missing);
    ok &= sym(cuda, "cuStreamSynchronize", cuStreamSynchronize, missing);
    ok &= sym(cuda, "cuStreamQuery", cuStreamQuery, missing);
    ok &= sym(cuda, "cuMemcpyDtoHAsync_v2", cuMemcpyDtoHAsync, missing);
    ok &= sym(cuda, "cuMemcpy2DAsync_v2", cuMemcpy2DAsync, missing);
    ok &= sym(cuda, "cuMemHostRegister_v2", cuMemHostRegister, missing);
    ok &= sym(cuda, "cuMemHostUnregister", cuMemHostUnregister, missing);
    ok &= sym(cuda, "cuMemHostAlloc", cuMemHostAlloc, missing);
    ok &= sym(cuda, "cuMemFreeHost", cuMemFreeHost, missing);
    ok &= sym(cuda, "cuMemAlloc_v2", cuMemAlloc, missing);
    ok &= sym(cuda, "cuMemFree_v2", cuMemFree, missing);
    ok &= sym(cuda, "cuMemsetD8Async", cuMemsetD8Async, missing);
    ok &= sym(cuda, "cuEventCreate", cuEventCreate, missing);
    ok &= sym(cuda, "cuEventDestroy_v2", cuEventDestroy, missing);
    ok &= sym(cuda, "cuEventRecord", cuEventRecord, missing);
    ok &= sym(cuda, "cuEventSynchronize", cuEventSynchronize, missing);
    ok &= sym(cuda, "cuEventQuery", cuEventQuery, missing);
    ok &= sym(cuda, "cuModuleLoadData", cuModuleLoadData, missing);
    ok &= sym(cuda, "cuModuleUnload", cuModuleUnload, missing);
    ok &= sym(cuda, "cuModuleGetFunction", cuModuleGetFunction, missing);
    ok &= sym(cuda, "cuLaunchKernel", cuLaunchKernel, missing);
    ok &= sym(cuda, "cuEventElapsedTime", cuEventElapsedTime, missing);
    ok &= sym(cuda, "cuGetErrorName", cuGetErrorName, missing);
    ok &= sym(cuda, "cuGetErrorString", cuGetErrorString, missing);
    ok &= sym(cuvid, "cuvidGetDecoderCaps", cuvidGetDecoderCaps, missing);
    ok &= sym(cuvid, "cuvidCreateDecoder", cuvidCreateDecoder, missing);
    ok &= sym(cuvid, "cuvidDestroyDecoder", cuvidDestroyDecoder, missing);
    ok &= sym(cuvid, "cuvidDecodePicture", cuvidDecodePicture, missing);
    ok &= sym(cuvid, "cuvidGetDecodeStatus", cuvidGetDecodeStatus, missing);
    ok &= sym(cuvid, "cuvidReconfigureDecoder", cuvidReconfigureDecoder, missing);
    ok &= sym(cuvid, "cuvidMapVideoFrame64", cuvidMapVideoFrame64, missing);
    ok &= sym(cuvid, "cuvidUnmapVideoFrame64", cuvidUnmapVideoFrame64, missing);
    ok &= sym(cuvid, "cuvidCtxLockCreate", cuvidCtxLockCreate, missing);
    ok &= sym(cuvid, "cuvidCtxLockDestroy", cuvidCtxLockDestroy, missing);
    ok &= sym(cuvid, "cuvidCreateVideoParser", cuvidCreateVideoParser, missing);
    ok &= sym(cuvid, "cuvidParseVideoData", cuvidParseVideoData, missing);
    ok &= sym(cuvid, "cuvidDestroyVideoParser", cuvidDestroyVideoParser, missing);
    if (!ok) {
        err = "driver DLLs lack: " + missing;
        return false;
    }
    return true;
}

std::string CudaApi::errorText(CUresult r) const {
    const char* name = nullptr;
    const char* desc = nullptr;
    if (cuGetErrorName) cuGetErrorName(r, &name);
    if (cuGetErrorString) cuGetErrorString(r, &desc);
    char b[512];
    snprintf(b, sizeof(b), "%s (%d: %s)", name ? name : "CUDA_ERROR_?", (int)r, desc ? desc : "no description");
    return b;
}

bool probeNvdec(int ordinal, std::string& report, std::string& err) {
    if (!g_cu.load(err)) return false;
    CUresult r = g_cu.cuInit(0);
    if (r != CUDA_SUCCESS) {
        err = "cuInit: " + g_cu.errorText(r);
        return false;
    }
    int drv = 0, count = 0;
    g_cu.cuDriverGetVersion(&drv);
    g_cu.cuDeviceGetCount(&count);
    if (ordinal >= count) {
        err = "GPU ordinal " + std::to_string(ordinal) + " out of range (" + std::to_string(count) + " CUDA devices)";
        return false;
    }
    CUdevice dev = 0;
    g_cu.cuDeviceGet(&dev, ordinal);
    char name[256] = {};
    g_cu.cuDeviceGetName(name, sizeof(name), dev);
    CUcontext ctx = nullptr;
    r = g_cu.cuCtxCreate(&ctx, CU_CTX_SCHED_SPIN, dev);
    if (r != CUDA_SUCCESS) {
        err = "cuCtxCreate: " + g_cu.errorText(r);
        return false;
    }
    CUVIDDECODECAPS caps{};
    caps.eCodecType = cudaVideoCodec_H264;
    caps.eChromaFormat = cudaVideoChromaFormat_420;
    caps.nBitDepthMinus8 = 0;
    r = g_cu.cuvidGetDecoderCaps(&caps);
    g_cu.cuCtxDestroy(ctx);
    if (r != CUDA_SUCCESS) {
        err = "cuvidGetDecoderCaps: " + g_cu.errorText(r);
        return false;
    }
    char b[512];
    snprintf(b, sizeof(b),
             "%s, CUDA driver API %d.%d, H.264 4:2:0 8-bit: bIsSupported=%u nNumNVDECs=%u nMaxWidth=%u nMaxHeight=%u "
             "nMaxMBCount=%u nOutputFormatMask=0x%X%s",
             name, drv / 1000, (drv % 1000) / 10, caps.bIsSupported, caps.nNumNVDECs, caps.nMaxWidth, caps.nMaxHeight,
             caps.nMaxMBCount, caps.nOutputFormatMask,
             (caps.nOutputFormatMask & (1 << cudaVideoSurfaceFormat_NV12)) ? " (NV12 ok)" : " (NO NV12!)");
    report = b;
    if (!caps.bIsSupported) {
        err = "NVDEC does not support H.264 4:2:0 8-bit on this GPU";
        return false;
    }
    return true;
}

}  // namespace cf
