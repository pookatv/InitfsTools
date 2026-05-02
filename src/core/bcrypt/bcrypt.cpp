#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

// BCrypt.dll proxy

static HMODULE hOriginalBCrypt = NULL;

typedef NTSTATUS(WINAPI* PFN_BCryptVerifySignature)(
    BCRYPT_KEY_HANDLE hKey,
    VOID* pPaddingInfo,
    PUCHAR pbHash,
    ULONG cbHash,
    PUCHAR pbSignature,
    ULONG cbSignature,
    ULONG dwFlags);

static PFN_BCryptVerifySignature Real_BCryptVerifySignature = NULL;

// Hooked BCryptVerifySignature - Bypass invalid signatures
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
        return 0xC0000002; // STATUS_NOT_IMPLEMENTED

    // Call real BCrypt
    NTSTATUS result = Real_BCryptVerifySignature(hKey, pPaddingInfo, pbHash,
        cbHash, pbSignature, cbSignature, dwFlags);

    // Bypass STATUS_INVALID_SIGNATURE (0xC000A000) -> STATUS_SUCCESS (0)
    if (result == 0xC000A000)
        return 0;

    return result;
}

// Forward BCrypt functions to system BCrypt.dll
NTSTATUS WINAPI BCryptOpenAlgorithmProvider(
    BCRYPT_ALG_HANDLE* phAlgorithm,
    LPCWSTR pszAlgId,
    LPCWSTR pszImplementation,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptOpenAlgorithmProvider");
    return Real ? Real(phAlgorithm, pszAlgId, pszImplementation, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptCloseAlgorithmProvider(
    BCRYPT_ALG_HANDLE hAlgorithm,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptCloseAlgorithmProvider");
    return Real ? Real(hAlgorithm, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptImportKeyPair(
    BCRYPT_ALG_HANDLE hAlgorithm,
    BCRYPT_KEY_HANDLE hImportKey,
    LPCWSTR pszBlobType,
    BCRYPT_KEY_HANDLE* phKey,
    PUCHAR pbInput,
    ULONG cbInput,
    ULONG dwFlags)
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
    BCRYPT_HANDLE hObject,
    LPCWSTR pszProperty,
    PUCHAR pbOutput,
    ULONG cbOutput,
    ULONG* pcbResult,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptGetProperty");
    return Real ? Real(hObject, pszProperty, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptSetProperty(
    BCRYPT_HANDLE hObject,
    LPCWSTR pszProperty,
    PUCHAR pbInput,
    ULONG cbInput,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptSetProperty");
    return Real ? Real(hObject, pszProperty, pbInput, cbInput, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptGenRandom(
    BCRYPT_ALG_HANDLE hAlgorithm,
    PUCHAR pbBuffer,
    ULONG cbBuffer,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptGenRandom");
    return Real ? Real(hAlgorithm, pbBuffer, cbBuffer, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptCreateHash(
    BCRYPT_ALG_HANDLE hAlgorithm,
    BCRYPT_HASH_HANDLE* phHash,
    PUCHAR pbHashObject,
    ULONG cbHashObject,
    PUCHAR pbSecret,
    ULONG cbSecret,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptCreateHash");
    return Real ? Real(hAlgorithm, phHash, pbHashObject, cbHashObject, pbSecret, cbSecret, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptHashData(
    BCRYPT_HASH_HANDLE hHash,
    PUCHAR pbInput,
    ULONG cbInput,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptHashData");
    return Real ? Real(hHash, pbInput, cbInput, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptFinishHash(
    BCRYPT_HASH_HANDLE hHash,
    PUCHAR pbOutput,
    ULONG cbOutput,
    ULONG dwFlags)
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
    BCRYPT_KEY_HANDLE hKey,
    PUCHAR pbInput,
    ULONG cbInput,
    VOID* pPaddingInfo,
    PUCHAR pbIV,
    ULONG cbIV,
    PUCHAR pbOutput,
    ULONG cbOutput,
    ULONG* pcbResult,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptEncrypt");
    return Real ? Real(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDecrypt(
    BCRYPT_KEY_HANDLE hKey,
    PUCHAR pbInput,
    ULONG cbInput,
    VOID* pPaddingInfo,
    PUCHAR pbIV,
    ULONG cbIV,
    PUCHAR pbOutput,
    ULONG cbOutput,
    ULONG* pcbResult,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDecrypt");
    return Real ? Real(hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptGenerateSymmetricKey(
    BCRYPT_ALG_HANDLE hAlgorithm,
    BCRYPT_KEY_HANDLE* phKey,
    PUCHAR pbKeyObject,
    ULONG cbKeyObject,
    PUCHAR pbSecret,
    ULONG cbSecret,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptGenerateSymmetricKey");
    return Real ? Real(hAlgorithm, phKey, pbKeyObject, cbKeyObject, pbSecret, cbSecret, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptGenerateKeyPair(
    BCRYPT_ALG_HANDLE hAlgorithm,
    BCRYPT_KEY_HANDLE* phKey,
    ULONG dwLength,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE*, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptGenerateKeyPair");
    return Real ? Real(hAlgorithm, phKey, dwLength, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptFinalizeKeyPair(
    BCRYPT_KEY_HANDLE hKey,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptFinalizeKeyPair");
    return Real ? Real(hKey, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptExportKey(
    BCRYPT_KEY_HANDLE hKey,
    BCRYPT_KEY_HANDLE hExportKey,
    LPCWSTR pszBlobType,
    PUCHAR pbOutput,
    ULONG cbOutput,
    ULONG* pcbResult,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptExportKey");
    return Real ? Real(hKey, hExportKey, pszBlobType, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptImportKey(
    BCRYPT_ALG_HANDLE hAlgorithm,
    BCRYPT_KEY_HANDLE hImportKey,
    LPCWSTR pszBlobType,
    BCRYPT_KEY_HANDLE* phKey,
    PUCHAR pbKeyObject,
    ULONG cbKeyObject,
    PUCHAR pbInput,
    ULONG cbInput,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptImportKey");
    return Real ? Real(hAlgorithm, hImportKey, pszBlobType, phKey, pbKeyObject, cbKeyObject, pbInput, cbInput, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptSignHash(
    BCRYPT_KEY_HANDLE hKey,
    VOID* pPaddingInfo,
    PUCHAR pbInput,
    ULONG cbInput,
    PUCHAR pbOutput,
    ULONG cbOutput,
    ULONG* pcbResult,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptSignHash");
    return Real ? Real(hKey, pPaddingInfo, pbInput, cbInput, pbOutput, cbOutput, pcbResult, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptSecretAgreement(
    BCRYPT_KEY_HANDLE hPrivKey,
    BCRYPT_KEY_HANDLE hPubKey,
    BCRYPT_SECRET_HANDLE* phAgreedSecret,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE, BCRYPT_SECRET_HANDLE*, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptSecretAgreement");
    return Real ? Real(hPrivKey, hPubKey, phAgreedSecret, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDeriveKey(
    BCRYPT_SECRET_HANDLE hSharedSecret,
    LPCWSTR pwszKDF,
    BCryptBufferDesc* pParameterList,
    PUCHAR pbDerivedKey,
    ULONG cbDerivedKey,
    ULONG* pcbResult,
    ULONG dwFlags)
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
    BCRYPT_HASH_HANDLE hHash,
    BCRYPT_HASH_HANDLE* phNewHash,
    PUCHAR pbHashObject,
    ULONG cbHashObject,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_HASH_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDuplicateHash");
    return Real ? Real(hHash, phNewHash, pbHashObject, cbHashObject, dwFlags) : 0xC0000002;
}

NTSTATUS WINAPI BCryptDuplicateKey(
    BCRYPT_KEY_HANDLE hKey,
    BCRYPT_KEY_HANDLE* phNewKey,
    PUCHAR pbKeyObject,
    ULONG cbKeyObject,
    ULONG dwFlags)
{
    typedef NTSTATUS(WINAPI* PFN)(BCRYPT_KEY_HANDLE, BCRYPT_KEY_HANDLE*, PUCHAR, ULONG, ULONG);
    static PFN Real = NULL;
    if (!Real) Real = (PFN)GetProcAddress(hOriginalBCrypt, "BCryptDuplicateKey");
    return Real ? Real(hKey, phNewKey, pbKeyObject, cbKeyObject, dwFlags) : 0xC0000002;
}

// DLL Entry Point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);

        // Load real BCrypt.dll from System32
        char systemPath[MAX_PATH];
        GetSystemDirectoryA(systemPath, MAX_PATH);
        strcat_s(systemPath, "\\BCrypt.dll");

        hOriginalBCrypt = LoadLibraryA(systemPath);

        if (!hOriginalBCrypt)
            return FALSE;

        // Get real BCryptVerifySignature
        Real_BCryptVerifySignature = (PFN_BCryptVerifySignature)
            GetProcAddress(hOriginalBCrypt, "BCryptVerifySignature");

        if (!Real_BCryptVerifySignature)
        {
            FreeLibrary(hOriginalBCrypt);
            return FALSE;
        }
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        if (hOriginalBCrypt)
            FreeLibrary(hOriginalBCrypt);
    }

    return TRUE;
}