#pragma once
// COM identities shared by the CLI, the installers and both DLLs.
#include <windows.h>

namespace cf::ids {

// MF virtual camera media source (activator CLSID passed to MFCreateVirtualCamera).
// {57C43686-EC55-4716-93E7-A563FB0E70F6}
inline constexpr GUID kMfSourceClsid = {0x57c43686, 0xec55, 0x4716, {0x93, 0xe7, 0xa5, 0x63, 0xfb, 0x0e, 0x70, 0xf6}};
inline constexpr wchar_t kMfSourceClsidStr[] = L"{57C43686-EC55-4716-93E7-A563FB0E70F6}";

// DirectShow source filter.
// {E63A4255-4E3B-48D8-8B63-461FD7470F7A}
inline constexpr GUID kDsFilterClsid = {0xe63a4255, 0x4e3b, 0x48d8, {0x8b, 0x63, 0x46, 0x1f, 0xd7, 0x47, 0x0f, 0x7a}};
inline constexpr wchar_t kDsFilterClsidStr[] = L"{E63A4255-4E3B-48D8-8B63-461FD7470F7A}";

// CLSID_VideoInputDeviceCategory
inline constexpr wchar_t kVideoInputCategoryStr[] = L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}";

inline constexpr wchar_t kMfSuffix[] = L" Webcam MediaFoundation (slow)";
inline constexpr wchar_t kDsSuffix[] = L" Webcam";
// Used only by DllRegisterServer before any phone has ever been seen; the CLI rewrites
// the friendly name with the phone's real name at runtime.
inline constexpr wchar_t kDsDefaultFriendlyName[] = L"iPhone Webcam";

}  // namespace cf::ids
