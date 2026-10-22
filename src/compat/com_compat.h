/*
 *  VSFilterMod cross-platform port -- HRESULT and friends.
 *
 *  The COM interfaces themselves are gone; what remains is the return-code
 *  vocabulary that RTS::Render and the STS loaders still speak. Keeping the
 *  signatures intact avoids touching several hundred `return E_FAIL;` sites.
 */

#pragma once

#include "platform.h"

#if !defined(_WIN32)

typedef LONG HRESULT;

#define S_OK             ((HRESULT)0L)
#define S_FALSE          ((HRESULT)1L)
#define E_FAIL           ((HRESULT)0x80004005L)
#define E_INVALIDARG     ((HRESULT)0x80070057L)
#define E_POINTER        ((HRESULT)0x80004003L)
#define E_OUTOFMEMORY    ((HRESULT)0x8007000EL)
#define E_NOTIMPL        ((HRESULT)0x80004001L)
#define E_UNEXPECTED     ((HRESULT)0x8000FFFFL)

#define SUCCEEDED(hr)    (((HRESULT)(hr)) >= 0)
#define FAILED(hr)       (((HRESULT)(hr)) < 0)

#define STDMETHODIMP        HRESULT
#define STDMETHODIMP_(type) type

#endif // !_WIN32
