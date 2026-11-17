# References for the hand-written parts

Clean room: Microsoft Learn documentation only (read 2026-11-17); no SDK,
VC, mingw-w64, Wine or ReactOS header. Metadata: windows-rs at the pin in
`PIN.json` (MIT OR Apache-2.0).

| What | Where | Learn page |
|---|---|---|
| base typedefs (`NATIVE_TYPEDEFS`) | generate.py | Windows Data Types (learn.microsoft.com/windows/win32/winprog/windows-data-types) |
| windows.h members, WIN32_LEAN_AND_MEAN | generate.py | Using the Windows Headers; each function page's "Header: X.h (include Windows.h)" line |
| umbrella members (uiautomation.h, objbase.h, ole2.h, shobjidl.h, shlobj.h) | generate.py | function and interface pages' "Header" lines, e.g. IUIAutomation: "uiautomationclient.h (include UIAutomation.h)" |
| WINAPI, CALLBACK, APIENTRY, STDMETHODCALLTYPE, STDAPI, STDMETHODIMP | stxwin_supplement.h | Windows Data Types; Argument Passing and Naming Conventions; COM "Defining COM interfaces" |
| SUCCEEDED, FAILED, HRESULT_FROM_WIN32, MAKE_HRESULT, HRESULT_CODE | stxwin_supplement.h | Using Macros for Error Handling; HRESULT_FROM_WIN32 macro (winerror.h) |
| LOWORD, HIWORD, LOBYTE, HIBYTE, MAKEWORD, MAKELONG, MAKEWPARAM, MAKELPARAM, MAKELRESULT | stxwin_supplement.h | the macro pages under Windows and Messages |
| GET_X_LPARAM, GET_Y_LPARAM, GET_WHEEL_DELTA_WPARAM, GET_KEYSTATE_WPARAM, GET_XBUTTON_WPARAM | stxwin_supplement.h | the macro pages (windowsx.h, winuser.h) |
| RGB, GetRValue, GetGValue, GetBValue | stxwin_supplement.h | the macro pages (wingdi.h) |
| MAKEINTRESOURCEW, IS_INTRESOURCE, MAKEINTATOM, TEXT | stxwin_supplement.h | the macro pages (winuser.h, winbase.h, winnt.h) |
| EXCEPTION_EXECUTE_HANDLER, EXCEPTION_CONTINUE_SEARCH, EXCEPTION_CONTINUE_EXECUTION | stxwin_supplement.h | try-except statement (C++ language reference) |
| SRWLOCK_INIT, CONDITION_VARIABLE_INIT, INIT_ONCE_STATIC_INIT | stxwin_supplement.h | InitializeSRWLock; InitializeConditionVariable; InitOnceInitialize |
| HGDI_ERROR | stxwin_supplement.h | SelectObject (return value) |
| InterlockedIncrement and family, YieldProcessor, MemoryBarrier | stxwin_supplement.h | the Interlocked* pages; Compiler intrinsics (_InterlockedIncrement, __faststorefence) |
| ZeroMemory, CopyMemory, MoveMemory, FillMemory, SecureZeroMemory | stxwin_supplement.h | the macro and function pages (winbase.h, wdm.h) |
| REFIID, REFCLSID, REFGUID, IsEqualGUID, IsEqualIID, IID_PPV_ARGS | stxwin_supplement.h | REFIID in Windows Data Types; IsEqualGUID; IID_PPV_ARGS macro |
| IsWindowsVersionOrGreater, IsWindows7/8/8Point1/10OrGreater | stxwin_versionhelpers.h | Version Helper functions; Verifying the System Version |
| WINAPI_FAMILY_PARTITION | stxwin_supplement.h | Programming with API partitions (WINAPI_FAMILY_PARTITION evaluates a partition's 0/1 value); our spelling `(partitions)` is unconfirmed beyond that page |
| DEFINE_GUID, INITGUID, initguid.h | stxwin_supplement.h, initguid.h | DEFINE_GUID macro (guiddef.h): declares the GUID, defines it where initguid.h is included first |
| `int` in UI Automation methods | generate.py (`INT_IDL_STEMS`) | the IRawElementProviderSimple, ITextRangeProvider and IUIAutomation* method pages (`int` parameters) against IAccessible's (`long`) |
| MONITORINFOEXW derives from MONITORINFO in C++ | generate.py (`based_field`) | MONITORINFOEXW structure (the `cbSize` member is MONITORINFO's); metadata `#[base]` |
