// macOS stub for Windows <unknwn.h> (COM IUnknown). Just enough for headers that
// reference IUnknown / GUID / the STDMETHOD macros to parse. The null renderer /
// stubs never use real COM.
#ifndef __LT_COMPAT_UNKNWN_H__
#define __LT_COMPAT_UNKNWN_H__

#include <windows.h>

#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} GUID;
#endif

typedef GUID IID;
typedef GUID CLSID;
typedef const GUID& REFGUID;
typedef const IID&  REFIID;
typedef const CLSID& REFCLSID;

#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#define STDMETHOD(m)        virtual HRESULT m
#define STDMETHOD_(t,m)     virtual t m
#define STDMETHODIMP        HRESULT
#define STDMETHODIMP_(t)    t
#define PURE                = 0
#define THIS_
#define THIS
#endif

struct IUnknown {
    virtual HRESULT QueryInterface(REFIID riid, void** ppv) = 0;
    virtual unsigned long AddRef() = 0;
    virtual unsigned long Release() = 0;
};
typedef IUnknown* LPUNKNOWN;

#endif
