#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

static HMODULE hOriginalBCrypt = NULL;
static HMODULE hOriginalCrypt32 = NULL;

// BCryptVerifySignature hook

typedef NTSTATUS(WINAPI* PFN_BCryptVerifySignature)(
    BCRYPT_KEY_HANDLE hKey,
    VOID* pPaddingInfo,
    PUCHAR pbHash,
    ULONG cbHash,
    PUCHAR pbSignature,
    ULONG cbSignature,
    ULONG dwFlags);

static PFN_BCryptVerifySignature Real_BCryptVerifySignature = NULL;

NTSTATUS WINAPI Hooked_BCryptVerifySignature(
    BCRYPT_KEY_HANDLE hKey,
    VOID* pPaddingInfo,
    PUCHAR pbHash,
    ULONG cbHash,
    PUCHAR pbSignature,
    ULONG cbSignature,
    ULONG dwFlags)
{
    if (!Real_BCryptVerifySignature)
        return 0xC0000002;

    NTSTATUS result = Real_BCryptVerifySignature(hKey, pPaddingInfo, pbHash,
        cbHash, pbSignature, cbSignature, dwFlags);

    if (result == 0xC000A000)
        return 0;

    return result;
}

// CryptQueryObject hook

typedef BOOL(WINAPI* PFN_CryptQueryObject)(
    DWORD dwObjectType,
    const void* pvObject,
    DWORD dwExpectedContentTypeFlags,
    DWORD dwExpectedFormatTypeFlags,
    DWORD dwFlags,
    DWORD* pdwMsgAndCertEncodingType,
    DWORD* pdwContentType,
    DWORD* pdwFormatType,
    HCERTSTORE* phCertStore,
    HCRYPTMSG* phMsg,
    const void** ppvContext);

static PFN_CryptQueryObject Real_CryptQueryObject = NULL;

BOOL WINAPI Hooked_CryptQueryObject(
    DWORD dwObjectType,
    const void* pvObject,
    DWORD dwExpectedContentTypeFlags,
    DWORD dwExpectedFormatTypeFlags,
    DWORD dwFlags,
    DWORD* pdwMsgAndCertEncodingType,
    DWORD* pdwContentType,
    DWORD* pdwFormatType,
    HCERTSTORE* phCertStore,
    HCRYPTMSG* phMsg,
    const void** ppvContext)
{
    if (Real_CryptQueryObject)
    {
        BOOL result = Real_CryptQueryObject(
            dwObjectType, pvObject,
            dwExpectedContentTypeFlags, dwExpectedFormatTypeFlags,
            dwFlags,
            pdwMsgAndCertEncodingType, pdwContentType, pdwFormatType,
            phCertStore, phMsg, ppvContext);

        if (result)
            return result;
    }

    if (pdwContentType) *pdwContentType = CERT_QUERY_CONTENT_PKCS7_SIGNED_EMBED;
    if (pdwFormatType)  *pdwFormatType = CERT_QUERY_FORMAT_BINARY;
    if (pdwMsgAndCertEncodingType) *pdwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
    return TRUE;
}

// IAT patcher

static void PatchIATEntry(HMODULE hExe, const char* targetDll,
    const char* funcName, ULONGLONG hookAddr)
{
    PIMAGE_DOS_HEADER pDos = (PIMAGE_DOS_HEADER)hExe;
    PIMAGE_NT_HEADERS pNT = (PIMAGE_NT_HEADERS)((BYTE*)hExe + pDos->e_lfanew);
    DWORD importRVA = pNT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRVA) return;

    PIMAGE_IMPORT_DESCRIPTOR pImport = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hExe + importRVA);
    for (; pImport->Name; pImport++)
    {
        const char* dllName = (const char*)((BYTE*)hExe + pImport->Name);
        if (_stricmp(dllName, targetDll) != 0) continue;

        PIMAGE_THUNK_DATA pOrigThunk = (PIMAGE_THUNK_DATA)((BYTE*)hExe + pImport->OriginalFirstThunk);
        PIMAGE_THUNK_DATA pThunk = (PIMAGE_THUNK_DATA)((BYTE*)hExe + pImport->FirstThunk);

        for (; pOrigThunk->u1.AddressOfData; pOrigThunk++, pThunk++)
        {
            if (pOrigThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;

            PIMAGE_IMPORT_BY_NAME pName = (PIMAGE_IMPORT_BY_NAME)
                ((BYTE*)hExe + pOrigThunk->u1.AddressOfData);

            if (strcmp((char*)pName->Name, funcName) == 0)
            {
                DWORD oldProtect;
                VirtualProtect(&pThunk->u1.Function, sizeof(ULONGLONG),
                    PAGE_EXECUTE_READWRITE, &oldProtect);
                pThunk->u1.Function = hookAddr;
                VirtualProtect(&pThunk->u1.Function, sizeof(ULONGLONG),
                    oldProtect, &oldProtect);
            }
        }
    }
}

static void PatchIAT()
{
    HMODULE hExe = GetModuleHandleA(NULL);
    if (!hExe) return;

    PatchIATEntry(hExe, "bcrypt.dll", "BCryptVerifySignature",
        (ULONGLONG)Hooked_BCryptVerifySignature);

    PatchIATEntry(hExe, "crypt32.dll", "CryptQueryObject",
        (ULONGLONG)Hooked_CryptQueryObject);
}

// RET-patch helpers

static void PatchFunction(const char* module, const char* funcName)
{
    HMODULE hMod = GetModuleHandleA(module);
    if (!hMod) hMod = LoadLibraryA(module);
    if (!hMod) return;

    FARPROC addr = GetProcAddress(hMod, funcName);
    if (!addr) return;

    DWORD oldProtect = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), 8, PAGE_EXECUTE_READWRITE, &oldProtect);
    *reinterpret_cast<BYTE*>(addr) = 0xC3;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), 8, oldProtect, &oldProtect);
}

static DWORD WINAPI PatchThread(LPVOID)
{
    PatchFunction("user32.dll", "MessageBoxTimeoutA");
    PatchFunction("kernelbase.dll", "TerminateProcess");
    return 1;
}

// BCrypt export forwards

NTSTATUS WINAPI BCryptOpenAlgorithmProvider(
    BCRYPT_ALG_HANDLE* phAlgorithm, LPCWSTR pszAlgId,
    LPCWSTR pszImplementation, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptOpenAlgorithmProvider");
    return Real ? Real(phAlgorithm, pszAlgId, pszImplementation, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE hAlgorithm, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptCloseAlgorithmProvider");
    return Real ? Real(hAlgorithm, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptImportKeyPair(
    BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_KEY_HANDLE hImportKey,
    LPCWSTR pszBlobType, BCRYPT_KEY_HANDLE* phKey,
    PUCHAR pbInput, ULONG cbInput, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptImportKeyPair");
    return Real ? Real(hAlgorithm, hImportKey, pszBlobType, phKey, pbInput, cbInput, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDestroyKey(BCRYPT_KEY_HANDLE hKey)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDestroyKey");
    return Real ? Real(hKey) : 0xC0000002;
}

NTSTATUS WINAPI BCryptGetProperty(
    BCRYPT_HANDLE hObject, LPCWSTR pszProperty,
    PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptGetProperty");
    return Real ? Real(hObject, pszProperty, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptSetProperty(
    BCRYPT_HANDLE hObject, LPCWSTR pszProperty,
    PUCHAR pbInput, ULONG cbInput, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptSetProperty");
    return Real ? Real(hObject, pszProperty, pbInput, cbInput, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptGenRandom(
    BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptGenRandom");
    return Real ? Real(hAlgorithm, pbBuffer, cbBuffer, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptCreateHash(
    BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_HASH_HANDLE* phHash,
    PUCHAR pbHashObject, ULONG cbHashObject,
    PUCHAR pbSecret, ULONG cbSecret, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptCreateHash");
    return Real ? Real(hAlgorithm, phHash, pbHashObject, cbHashObject, pbSecret, cbSecret, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptHashData(
    BCRYPT_HASH_HANDLE hHash, PUCHAR pbInput, ULONG cbInput, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptHashData");
    return Real ? Real(hHash, pbInput, cbInput, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptFinishHash(
    BCRYPT_HASH_HANDLE hHash, PUCHAR pbOutput, ULONG cbOutput, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptFinishHash");
    return Real ? Real(hHash, pbOutput, cbOutput, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDestroyHash(BCRYPT_HASH_HANDLE hHash)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HASH_HANDLE);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDestroyHash");
    return Real ? Real(hHash) : 0xC0000002;
}

NTSTATUS WINAPI BCryptEncrypt(
    BCRYPT_KEY_HANDLE hKey, PUCHAR pbInput, ULONG cbInput,
    VOID* pPaddingInfo, PUCHAR pbIV, ULONG cbIV,
    PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptEncrypt");
    return Real ? Real(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDecrypt(
    BCRYPT_KEY_HANDLE hKey, PUCHAR pbInput, ULONG cbInput,
    VOID* pPaddingInfo, PUCHAR pbIV, ULONG cbIV,
    PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDecrypt");
    return Real ? Real(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptGenerateSymmetricKey(
    BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_KEY_HANDLE* phKey,
    PUCHAR pbKeyObject, ULONG cbKeyObject,
    PUCHAR pbSecret, ULONG cbSecret, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptGenerateSymmetricKey");
    return Real ? Real(hAlgorithm, phKey, pbKeyObject, cbKeyObject, pbSecret, cbSecret, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptGenerateKeyPair(
    BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_KEY_HANDLE* phKey,
    ULONG dwLength, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE*, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptGenerateKeyPair");
    return Real ? Real(hAlgorithm, phKey, dwLength, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptFinalizeKeyPair(BCRYPT_KEY_HANDLE hKey, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptFinalizeKeyPair");
    return Real ? Real(hKey, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptExportKey(
    BCRYPT_KEY_HANDLE hKey, BCRYPT_KEY_HANDLE hExportKey,
    LPCWSTR pszBlobType, PUCHAR pbOutput, ULONG cbOutput,
    ULONG* pcbResult, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptExportKey");
    return Real ? Real(hKey, hExportKey, pszBlobType, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptImportKey(
    BCRYPT_ALG_HANDLE hAlgorithm, BCRYPT_KEY_HANDLE hImportKey,
    LPCWSTR pszBlobType, BCRYPT_KEY_HANDLE* phKey,
    PUCHAR pbKeyObject, ULONG cbKeyObject,
    PUCHAR pbInput, ULONG cbInput, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptImportKey");
    return Real ? Real(hAlgorithm, hImportKey, pszBlobType, phKey, pbKeyObject, cbKeyObject, pbInput, cbInput, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptSignHash(
    BCRYPT_KEY_HANDLE hKey, VOID* pPaddingInfo,
    PUCHAR pbInput, ULONG cbInput,
    PUCHAR pbOutput, ULONG cbOutput,
    ULONG* pcbResult, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptSignHash");
    return Real ? Real(hKey, pPaddingInfo, pbInput, cbInput, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptSecretAgreement(
    BCRYPT_KEY_HANDLE hPrivKey, BCRYPT_KEY_HANDLE hPubKey,
    BCRYPT_SECRET_HANDLE* phAgreedSecret, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, BCRYPT_SECRET_HANDLE*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptSecretAgreement");
    return Real ? Real(hPrivKey, hPubKey, phAgreedSecret, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDeriveKey(
    BCRYPT_SECRET_HANDLE hSharedSecret, LPCWSTR pwszKDF,
    BCryptBufferDesc* pParameterList,
    PUCHAR pbDerivedKey, ULONG cbDerivedKey,
    ULONG* pcbResult, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_SECRET_HANDLE, LPCWSTR, BCryptBufferDesc*, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDeriveKey");
    return Real ? Real(hSharedSecret, pwszKDF, pParameterList, pbDerivedKey, cbDerivedKey, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDestroySecret(BCRYPT_SECRET_HANDLE hSecret)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_SECRET_HANDLE);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDestroySecret");
    return Real ? Real(hSecret) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDuplicateHash(
    BCRYPT_HASH_HANDLE hHash, BCRYPT_HASH_HANDLE* phNewHash,
    PUCHAR pbHashObject, ULONG cbHashObject, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HASH_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDuplicateHash");
    return Real ? Real(hHash, phNewHash, pbHashObject, cbHashObject, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDuplicateKey(
    BCRYPT_KEY_HANDLE hKey, BCRYPT_KEY_HANDLE* phNewKey,
    PUCHAR pbKeyObject, ULONG cbKeyObject, ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDuplicateKey");
    return Real ? Real(hKey, phNewKey, pbKeyObject, cbKeyObject, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptVerifySignature(
    BCRYPT_KEY_HANDLE hKey, VOID* pPaddingInfo,
    PUCHAR pbHash, ULONG cbHash,
    PUCHAR pbSignature, ULONG cbSignature, ULONG dwFlags)
{
    return Hooked_BCryptVerifySignature(hKey, pPaddingInfo, pbHash,
        cbHash, pbSignature, cbSignature, dwFlags);
}

// DllMain

static bool IsInitfsTools()
{
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    const char* name = exePath;
    for (const char* p = exePath; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return (_stricmp(name, "InitfsTools.exe") == 0);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        char systemPath[MAX_PATH];
        GetSystemDirectoryA(systemPath, MAX_PATH);
        strcat_s(systemPath, "\\BCrypt.dll");
        hOriginalBCrypt = LoadLibraryExA(systemPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hOriginalBCrypt)
            return FALSE;

        Real_BCryptVerifySignature = (PFN_BCryptVerifySignature)
            GetProcAddress(hOriginalBCrypt, "BCryptVerifySignature");
        if (!Real_BCryptVerifySignature)
        {
            FreeLibrary(hOriginalBCrypt);
            return FALSE;
        }

        // If we're loaded by InitfsTools.exe, act as a pure passthrough —
        // no hooking, no IAT patches, no extra threads.
        if (IsInitfsTools())
            return TRUE;

        hOriginalCrypt32 = LoadLibraryA("crypt32.dll");
        if (hOriginalCrypt32)
        {
            Real_CryptQueryObject = (PFN_CryptQueryObject)
                GetProcAddress(hOriginalCrypt32, "CryptQueryObject");
        }

        PatchIAT();
        CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr);
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        if (hOriginalBCrypt)  FreeLibrary(hOriginalBCrypt);
        if (hOriginalCrypt32) FreeLibrary(hOriginalCrypt32);
    }

    return TRUE;
}