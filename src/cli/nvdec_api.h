#pragma once
// nvcuda.dll / nvcuvid.dll loaded at runtime from System32 (they ship in the driver),
// so the binary runs on any driver that has them and never links a stub library.
#include <windows.h>
#include "dynlink_cuda.h"
#include "dynlink_nvcuvid.h"
#include <string>

// Driver API pieces absent from the vendored minimal dynlink_cuda.h (stable ABI).
#ifndef CU_CTX_SCHED_SPIN
#define CU_CTX_SCHED_SPIN 1
#endif
#ifndef CU_CTX_SCHED_YIELD
#define CU_CTX_SCHED_YIELD 2
#endif
#ifndef CU_MEMHOSTREGISTER_PORTABLE
#define CU_MEMHOSTREGISTER_PORTABLE 1
#endif
#ifndef CU_MEMHOSTALLOC_PORTABLE
#define CU_MEMHOSTALLOC_PORTABLE 1
#endif
typedef CUresult CUDAAPI tcuMemHostRegister_v2(void* p, size_t bytesize, unsigned int flags);
typedef CUresult CUDAAPI tcuMemHostUnregister(void* p);
typedef CUresult CUDAAPI tcuCtxSetCurrent(CUcontext ctx);
typedef CUresult CUDAAPI tcuEventElapsedTime(float* ms, CUevent start, CUevent end);

namespace cf {

struct CudaApi {
    HMODULE cuda = nullptr, cuvid = nullptr;

    tcuInit* cuInit = nullptr;
    tcuDriverGetVersion* cuDriverGetVersion = nullptr;
    tcuDeviceGetCount* cuDeviceGetCount = nullptr;
    tcuDeviceGet* cuDeviceGet = nullptr;
    tcuDeviceGetName* cuDeviceGetName = nullptr;
    tcuDeviceGetAttribute* cuDeviceGetAttribute = nullptr;
    tcuCtxCreate_v2* cuCtxCreate = nullptr;
    tcuCtxDestroy_v2* cuCtxDestroy = nullptr;
    tcuCtxPushCurrent_v2* cuCtxPushCurrent = nullptr;
    tcuCtxPopCurrent_v2* cuCtxPopCurrent = nullptr;
    tcuCtxSetCurrent* cuCtxSetCurrent = nullptr;
    tcuStreamCreate* cuStreamCreate = nullptr;
    tcuStreamDestroy_v2* cuStreamDestroy = nullptr;
    tcuStreamSynchronize* cuStreamSynchronize = nullptr;
    tcuStreamQuery* cuStreamQuery = nullptr;
    tcuMemcpyDtoHAsync_v2* cuMemcpyDtoHAsync = nullptr;
    tcuMemcpy2DAsync_v2* cuMemcpy2DAsync = nullptr;
    tcuMemHostRegister_v2* cuMemHostRegister = nullptr;
    tcuMemHostUnregister* cuMemHostUnregister = nullptr;
    tcuMemHostAlloc* cuMemHostAlloc = nullptr;
    tcuMemFreeHost* cuMemFreeHost = nullptr;
    tcuMemAlloc_v2* cuMemAlloc = nullptr;
    tcuMemFree_v2* cuMemFree = nullptr;
    tcuMemsetD8Async* cuMemsetD8Async = nullptr;
    tcuEventCreate* cuEventCreate = nullptr;
    tcuEventDestroy_v2* cuEventDestroy = nullptr;
    tcuEventRecord* cuEventRecord = nullptr;
    tcuEventSynchronize* cuEventSynchronize = nullptr;
    tcuEventQuery* cuEventQuery = nullptr;
    tcuModuleLoadData* cuModuleLoadData = nullptr;
    tcuModuleUnload* cuModuleUnload = nullptr;
    tcuModuleGetFunction* cuModuleGetFunction = nullptr;
    tcuLaunchKernel* cuLaunchKernel = nullptr;
    tcuEventElapsedTime* cuEventElapsedTime = nullptr;
    tcuGetErrorName* cuGetErrorName = nullptr;
    tcuGetErrorString* cuGetErrorString = nullptr;

    tcuvidGetDecoderCaps* cuvidGetDecoderCaps = nullptr;
    tcuvidCreateDecoder* cuvidCreateDecoder = nullptr;
    tcuvidDestroyDecoder* cuvidDestroyDecoder = nullptr;
    tcuvidDecodePicture* cuvidDecodePicture = nullptr;
    tcuvidGetDecodeStatus* cuvidGetDecodeStatus = nullptr;
    tcuvidReconfigureDecoder* cuvidReconfigureDecoder = nullptr;
    tcuvidMapVideoFrame64* cuvidMapVideoFrame64 = nullptr;
    tcuvidUnmapVideoFrame64* cuvidUnmapVideoFrame64 = nullptr;
    tcuvidCtxLockCreate* cuvidCtxLockCreate = nullptr;
    tcuvidCtxLockDestroy* cuvidCtxLockDestroy = nullptr;
    tcuvidCreateVideoParser* cuvidCreateVideoParser = nullptr;
    tcuvidParseVideoData* cuvidParseVideoData = nullptr;
    tcuvidDestroyVideoParser* cuvidDestroyVideoParser = nullptr;

    bool load(std::string& err);
    // "CUDA_ERROR_xxx (description)"
    std::string errorText(CUresult r) const;
};

extern CudaApi g_cu;

// Probes H.264 decode support on `ordinal` and prints the caps. Used by env checks.
bool probeNvdec(int ordinal, std::string& report, std::string& err);

}  // namespace cf
