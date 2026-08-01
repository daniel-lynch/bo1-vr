/* experiments/05_submit/d3d9_dxvk.h
 *
 * C bindings for DXVK's D3D9 <-> Vulkan interop interfaces.
 *
 * Transcribed from dxvk v2.6.2 src/d3d9/d3d9_interfaces.h. The DXVK shipped in
 * Proton 10.0-4b reports itself as v2.6.2-23-g3cb664e1260926e (`strings` on
 * files/lib/wine/dxvk/i386-windows/d3d9.dll), so v2.6.2 is the matching tag.
 *
 * These are C++ COM interfaces with STDMETHODCALLTYPE (= __stdcall) methods and
 * single inheritance from IUnknown, so the Win32 C binding is exact: the vtable
 * is [QueryInterface, AddRef, Release] followed by the declared methods in
 * declaration order, and every method takes `this` as its first stack argument.
 * That is the same convention mingw's own d3d9.h uses for IDirect3DDevice9.
 *
 * ONLY the methods this experiment calls are relied upon; the trailing ones are
 * declared to keep the vtable the right length but are never invoked. Slots are
 * numbered in the comments so a DXVK version bump can be diffed quickly.
 */
#ifndef BO1VR_D3D9_DXVK_H
#define BO1VR_D3D9_DXVK_H

#include <windows.h>
#include "vk_min.h"

typedef struct ID3D9VkInteropInterface  ID3D9VkInteropInterface;
typedef struct ID3D9VkInteropInterface1 ID3D9VkInteropInterface1;
typedef struct ID3D9VkInteropTexture    ID3D9VkInteropTexture;
typedef struct ID3D9VkInteropDevice     ID3D9VkInteropDevice;

/* Defined here rather than via DEFINE_GUID/initguid.h so that including this
 * header after <d3d9.h> cannot disturb d3d9.h's own GUID declarations. */
/* MIDL_INTERFACE("3461a81b-ce41-485b-b6b5-fcf08ba6a6bd") */
static const GUID IID_ID3D9VkInteropInterface =
    { 0x3461a81b, 0xce41, 0x485b, { 0xb6, 0xb5, 0xfc, 0xf0, 0x8b, 0xa6, 0xa6, 0xbd } };
/* MIDL_INTERFACE("d6589ed4-7a37-4096-bac2-223b25ae31d2") */
static const GUID IID_ID3D9VkInteropInterface1 =
    { 0xd6589ed4, 0x7a37, 0x4096, { 0xba, 0xc2, 0x22, 0x3b, 0x25, 0xae, 0x31, 0xd2 } };
/* MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1") */
static const GUID IID_ID3D9VkInteropTexture =
    { 0xd56344f5, 0x8d35, 0x46fd, { 0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1 } };
/* MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0") */
static const GUID IID_ID3D9VkInteropDevice =
    { 0x2eaa4b89, 0x0107, 0x4bdb, { 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0 } };

typedef struct ID3D9VkInteropInterfaceVtbl {
    /* 0 */ HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3D9VkInteropInterface *, REFIID, void **);
    /* 1 */ ULONG   (STDMETHODCALLTYPE *AddRef)(ID3D9VkInteropInterface *);
    /* 2 */ ULONG   (STDMETHODCALLTYPE *Release)(ID3D9VkInteropInterface *);
    /* 3 */ void    (STDMETHODCALLTYPE *GetInstanceHandle)(ID3D9VkInteropInterface *, bo1vr_VkInstance *);
    /* 4 */ void    (STDMETHODCALLTYPE *GetPhysicalDeviceHandle)(ID3D9VkInteropInterface *, UINT, bo1vr_VkPhysicalDevice *);
    /* --- ID3D9VkInteropInterface1 adds: --- */
    /* 5 */ HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(ID3D9VkInteropInterface *, UINT *, const char **);
} ID3D9VkInteropInterfaceVtbl;

struct ID3D9VkInteropInterface { const ID3D9VkInteropInterfaceVtbl *lpVtbl; };

typedef struct ID3D9VkInteropTextureVtbl {
    /* 0 */ HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3D9VkInteropTexture *, REFIID, void **);
    /* 1 */ ULONG   (STDMETHODCALLTYPE *AddRef)(ID3D9VkInteropTexture *);
    /* 2 */ ULONG   (STDMETHODCALLTYPE *Release)(ID3D9VkInteropTexture *);
    /* 3 */ HRESULT (STDMETHODCALLTYPE *GetVulkanImageInfo)(ID3D9VkInteropTexture *,
                                                           bo1vr_VkImage *, bo1vr_VkEnum *,
                                                           bo1vr_VkImageCreateInfo *);
} ID3D9VkInteropTextureVtbl;

struct ID3D9VkInteropTexture { const ID3D9VkInteropTextureVtbl *lpVtbl; };

typedef struct ID3D9VkInteropDeviceVtbl {
    /*  0 */ HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3D9VkInteropDevice *, REFIID, void **);
    /*  1 */ ULONG   (STDMETHODCALLTYPE *AddRef)(ID3D9VkInteropDevice *);
    /*  2 */ ULONG   (STDMETHODCALLTYPE *Release)(ID3D9VkInteropDevice *);
    /*  3 */ void    (STDMETHODCALLTYPE *GetVulkanHandles)(ID3D9VkInteropDevice *, bo1vr_VkInstance *,
                                                          bo1vr_VkPhysicalDevice *, bo1vr_VkDevice *);
    /*  4 */ void    (STDMETHODCALLTYPE *GetSubmissionQueue)(ID3D9VkInteropDevice *, bo1vr_VkQueue *,
                                                            uint32_t *, uint32_t *);
    /*  5 */ void    (STDMETHODCALLTYPE *TransitionTextureLayout)(ID3D9VkInteropDevice *, ID3D9VkInteropTexture *,
                                                                 const bo1vr_VkImageSubresourceRange *,
                                                                 bo1vr_VkEnum, bo1vr_VkEnum);
    /*  6 */ void    (STDMETHODCALLTYPE *FlushRenderingCommands)(ID3D9VkInteropDevice *);
    /*  7 */ void    (STDMETHODCALLTYPE *LockSubmissionQueue)(ID3D9VkInteropDevice *);
    /*  8 */ void    (STDMETHODCALLTYPE *ReleaseSubmissionQueue)(ID3D9VkInteropDevice *);
    /*  9 */ void    (STDMETHODCALLTYPE *LockDevice)(ID3D9VkInteropDevice *);
    /* 10 */ void    (STDMETHODCALLTYPE *UnlockDevice)(ID3D9VkInteropDevice *);
    /* 11 */ unsigned char (STDMETHODCALLTYPE *WaitForResource)(ID3D9VkInteropDevice *, void *, DWORD); /* not called */
    /* 12 */ HRESULT (STDMETHODCALLTYPE *CreateImage)(ID3D9VkInteropDevice *, const void *, void **);   /* not called */
} ID3D9VkInteropDeviceVtbl;

struct ID3D9VkInteropDevice { const ID3D9VkInteropDeviceVtbl *lpVtbl; };

#endif /* BO1VR_D3D9_DXVK_H */
