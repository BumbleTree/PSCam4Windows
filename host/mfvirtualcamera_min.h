#pragma once
//
// Minimal local declaration of the Windows 11 virtual camera API
// (mfvirtualcamera.h ships only with SDK >= 10.0.22000, this machine builds
// with 10.0.19041). IIDs and vtable order were verified against the real SDK
// header (via DirectN's generated bindings) and the Microsoft Learn docs:
//   IMFVirtualCamera    {1C08A864-EF6C-4C75-AF59-5F2D68DA9563}
//   IMFCameraSyncObject {6338B23A-3042-49D2-A3EA-EC0FED815407}
// MFCreateVirtualCamera is exported from mfsensorgroup.dll (build >= 22000);
// resolve it with GetProcAddress so no import library is needed.
//
#include <mfidl.h>

typedef enum MFVirtualCameraType
{
    MFVirtualCameraType_SoftwareCameraSource = 0,
} MFVirtualCameraType;

typedef enum MFVirtualCameraLifetime
{
    MFVirtualCameraLifetime_Session = 0,
    MFVirtualCameraLifetime_System  = 1,
} MFVirtualCameraLifetime;

typedef enum MFVirtualCameraAccess
{
    MFVirtualCameraAccess_CurrentUser = 0,
    MFVirtualCameraAccess_AllUsers    = 1,
} MFVirtualCameraAccess;

struct _DEVPROPKEY;

MIDL_INTERFACE("6338b23a-3042-49d2-a3ea-ec0fed815407")
IMFCameraSyncObject : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE WaitOnSignal(DWORD timeOutInMs) = 0;
    virtual void    STDMETHODCALLTYPE Shutdown() = 0;
};

MIDL_INTERFACE("1c08a864-ef6c-4c75-af59-5f2d68da9563")
IMFVirtualCamera : public IMFAttributes
{
    virtual HRESULT STDMETHODCALLTYPE AddDeviceSourceInfo(LPCWSTR DeviceSourceInfo) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProperty(const struct _DEVPROPKEY* pKey,
                                                  ULONG Type /*DEVPROPTYPE*/,
                                                  const BYTE* pbData, ULONG cbData) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddRegistryEntry(LPCWSTR EntryName, LPCWSTR SubkeyPath,
                                                       DWORD dwRegType, const BYTE* pbData,
                                                       ULONG cbData) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(IMFAsyncCallback* pCallback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** ppMediaSource) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendCameraProperty(REFGUID propertySet, ULONG propertyId,
                                                         ULONG propertyFlags, void* propertyPayload,
                                                         ULONG propertyPayloadLength, void* data,
                                                         ULONG dataLength, ULONG* dataWritten) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncEvent(REFGUID kseventSet, ULONG kseventId,
                                                      ULONG kseventFlags, HANDLE eventHandle,
                                                      IMFCameraSyncObject** cameraSyncObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncSemaphore(REFGUID kseventSet, ULONG kseventId,
                                                          ULONG kseventFlags, HANDLE semaphoreHandle,
                                                          LONG semaphoreAdjustment,
                                                          IMFCameraSyncObject** cameraSyncObject) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};

typedef HRESULT(STDAPICALLTYPE* PFN_MFCreateVirtualCamera)(
    MFVirtualCameraType type,
    MFVirtualCameraLifetime lifetime,
    MFVirtualCameraAccess access,
    LPCWSTR friendlyName,
    LPCWSTR sourceId,
    const GUID* categories,
    ULONG categoryCount,
    IMFVirtualCamera** virtualCamera);
