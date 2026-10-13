# third_party provenance

## nvcodec/

`dynlink_cuda.h`, `dynlink_cuviddec.h`, `dynlink_nvcuvid.h` from FFmpeg's
[nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers), tag **n13.0.19.1**
(commit `ee1c8a8cdb05f733ca661c30fd50c74f94ad6937`), MIT licensed.

They correspond to NVIDIA Video Codec SDK **13.0.19** (NVDEC API 13.0) and need driver
**570.0+** on Windows. The newest tag (n13.1.15.0, SDK 13.1) needs driver 610+, so 13.1 was deliberately not
used to keep older drivers working.

These were picked over NVIDIA's own `nvcuvid.h`/`cuviddec.h` because they are written for
dynamic loading (function-pointer typedefs, no import lib) and need no NVIDIA developer
login to download. `nvcuvid.dll` and `nvcuda.dll` are loaded at runtime with `LoadLibraryExW`
from System32 (`src/cli/nvdec_api.cpp`). `dynlink_cuda.h` is a minimal Driver API subset. The
few symbols it lacks (`CU_CTX_SCHED_SPIN`, `cuMemHostRegister`, `cuMemHostUnregister`,
`cuCtxSetCurrent`, `cuEventElapsedTime`) are declared in `src/cli/nvdec_api.h`
with the stable Driver API ABI.

## baseclasses/

DirectShow base classes (`strmbase`) from
[microsoft/Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples),
path `Samples/Win7Samples/multimedia/directshow/baseclasses`, commit
`434f6002bdf9cf9829406c3ff2b33387982d6168`, MIT licensed.

Local patches needed to build under MSVC 17.14 / C++20 / `/permissive-`:

| File | Change | Why |
|---|---|---|
| `videoctl.h` | `virtual CAggDirectDraw::~CAggDirectDraw()` → `virtual ~CAggDirectDraw()` (and the same for `CAggDrawSurface`) | C3254/C3244/C2385: a qualified destructor name inside the class body is parsed as an explicit override |
| `transip.h` | `CTransInPlaceFilter::Copy(` → `Copy(` in the class body | C4596: illegal qualified name in member declaration |
| `videoctl.cpp` | `StringFromResource` returns the cleared buffer instead of a string literal | C2440: string literal is `const` |

The build flags in `CMakeLists.txt` handle the rest, for this target only:
- `/Zc:strictStrings-`: `amvideo.cpp`'s subtype-name table and the SDK's `uuids.h` name
  table bind string literals to non-const `CHAR*`/`WCHAR*` across exported signatures.
- `/UNOMINMAX`: the sources rely on the `min`/`max` macros from `windows.h`.
- `/W0`
