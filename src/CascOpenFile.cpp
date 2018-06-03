/*****************************************************************************/
/* CascOpenFile.cpp                       Copyright (c) Ladislav Zezula 2014 */
/*---------------------------------------------------------------------------*/
/* System-dependent directory functions for CascLib                          */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 01.05.14  1.00  Lad  The first version of CascOpenFile.cpp                */
/*****************************************************************************/

#define __CASCLIB_SELF__
#include "CascLib.h"
#include "CascCommon.h"

//-----------------------------------------------------------------------------
// Local functions

TCascFile * IsValidFileHandle(HANDLE hFile)
{
    TCascFile * hf = (TCascFile *)hFile;

    return (hf != NULL && hf->hs != NULL && hf->szClassName != NULL && !strcmp(hf->szClassName, "TCascFile")) ? hf : NULL;
}

PCASC_CKEY_ENTRY FindCKeyEntry(TCascStorage * hs, PQUERY_KEY pCKey, PDWORD PtrIndex)
{
    return (PCASC_CKEY_ENTRY)Map_FindObject(hs->pCKeyEntryMap, pCKey->pbData, PtrIndex);
}

PCASC_EKEY_ENTRY FindEKeyEntry(TCascStorage * hs, PQUERY_KEY pEKey)
{
    return (PCASC_EKEY_ENTRY)Map_FindObject(hs->pEKeyEntryMap, pEKey->pbData, NULL);
}

static DWORD GetFileContentSize(TCascStorage * hs, PQUERY_KEY pCKey, PQUERY_KEY pEKey, DWORD dwContentSize)
{
    // Check CKey of the ENCODING file. The size of ENCODING file
    // is often wrong in CKeyEntry (WoW builds 18125 - 23420)
    if(hs->EncodingFile.cbData >= MD5_HASH_SIZE && pCKey != NULL && !memcmp(hs->EncodingFile.pbData, pCKey->pbData, pCKey->cbData))
        return hs->EncodingSize.ContentSize;

    // Check EKey of the ENCODING file
    if(hs->EncodingFile.cbData > MD5_HASH_SIZE && pEKey != NULL && !memcmp(hs->EncodingFile.pbData + MD5_HASH_SIZE, pEKey->pbData, pEKey->cbData))
        return hs->EncodingSize.ContentSize;

    // Return whatever was entered from the called
    return dwContentSize;
}

static TCascFile * CreateFileHandle(TCascStorage * hs, PQUERY_KEY pCKey, PQUERY_KEY pEKey, PCASC_EKEY_ENTRY pEKeyEntry, DWORD dwContentSize)
{
    ULONGLONG ArchiveAndOffset = ConvertBytesToInteger_5(pEKeyEntry->ArchiveAndOffset);
    ULONGLONG FileOffsMask = ((ULONGLONG)1 << hs->IndexFile[0].FileOffsetBits) - 1;
    TCascFile * hf;

    // Allocate the CASC file structure
    hf = (TCascFile *)CASC_ALLOC(TCascFile, 1);
    if(hf != NULL)
    {
        // Initialize the structure
        memset(hf, 0, sizeof(TCascFile));
        hf->ArchiveIndex = (DWORD)(ArchiveAndOffset >> hs->IndexFile[0].FileOffsetBits);
        hf->ArchiveOffset = (DWORD)(ArchiveAndOffset & FileOffsMask);
        hf->szClassName = "TCascFile";

        // Supply the content key, if available
        if(pCKey != NULL)
        {
            assert(pCKey->pbData != NULL && pCKey->cbData == MD5_HASH_SIZE);
            memcpy(hf->CKey.Value, pCKey->pbData, pCKey->cbData);
        }

        // Supply the encoded key, if available
        if(pEKey != NULL)
        {
            assert(pEKey->pbData != NULL && CASC_EKEY_SIZE <= pEKey->cbData && pEKey->cbData <= MD5_HASH_SIZE);
            memcpy(hf->EKey.Value, pEKey->pbData, pEKey->cbData);
        }

        // Copy the encoded file size
        if(pEKeyEntry != NULL)
        {
            hf->EncodedSize = ConvertBytesToInteger_4_LE(pEKeyEntry->EncodedSize);
        }

        // Set the content size
        hf->ContentSize = GetFileContentSize(hs, pCKey, pEKey, dwContentSize);

        // Increment the number of references to the archive
        hs->dwRefCount++;
        hf->hs = hs;
    }

    return hf;
}

static bool OpenFileByEKey(TCascStorage * hs, PQUERY_KEY pCKey, PQUERY_KEY pEKey, DWORD dwContentSize, TCascFile ** PtrCascFile)
{
    PCASC_EKEY_ENTRY pEKeyEntry;
    TCascFile * hf = NULL;

    // Find the EKey entry in the array of encoded keys
    pEKeyEntry = FindEKeyEntry(hs, pEKey);
    if(pEKeyEntry == NULL)
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }

    // Create the file handle structure
    hf = CreateFileHandle(hs, pCKey, pEKey, pEKeyEntry, dwContentSize);
    if(hf == NULL)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }

#ifdef CASCLIB_TEST
    hf->FileSize_EEntry = ConvertBytesToInteger_4_LE(pEKeyEntry->EncodedSize);
#endif

    // Give the results
    PtrCascFile[0] = hf;
    return true;
}

static bool OpenFileByCKey(TCascStorage * hs, PQUERY_KEY pCKey, TCascFile ** PtrCascFile)
{
    PCASC_CKEY_ENTRY pCKeyEntry;
    QUERY_KEY EKey;
    DWORD dwContentSize;

    // Find the encoding entry
    pCKeyEntry = FindCKeyEntry(hs, pCKey, NULL);
    if(pCKeyEntry == NULL)
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }

    // Retrieve the content size from the CKey entry
    dwContentSize = ConvertBytesToInteger_4(pCKeyEntry->ContentSize);

    // Prepare the file index and open the file by index
    // Note: We don't know what to do if there is more than just one EKey
    // We always take the first file present. Is that correct?
    EKey.pbData = GET_EKEY(pCKeyEntry);
    EKey.cbData = MD5_HASH_SIZE;
    return OpenFileByEKey(hs, pCKey, &EKey, dwContentSize, PtrCascFile);
}

//-----------------------------------------------------------------------------
// Public functions

bool WINAPI CascOpenFileByEKey(HANDLE hStorage, PQUERY_KEY pCKey, PQUERY_KEY pEKey, DWORD dwContentSize, HANDLE * phFile)
{
    TCascStorage * hs;

    // Validate the storage handle
    hs = IsValidStorageHandle(hStorage);
    if(hs == NULL)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    // Validate the other parameters
    // Note that the 
    if(pEKey == NULL || pEKey->pbData == NULL || pEKey->cbData == 0 || phFile == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    // Use the internal function to open the file
    return OpenFileByEKey(hs, pCKey, pEKey, dwContentSize, (TCascFile **)phFile);
}

bool WINAPI CascOpenFileByCKey(HANDLE hStorage, PQUERY_KEY pCKey, HANDLE * phFile)
{
    TCascStorage * hs;

    // Validate the storage handle
    hs = IsValidStorageHandle(hStorage);
    if(hs == NULL)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    // Validate the other parameters
    if(pCKey == NULL || pCKey->pbData == NULL || pCKey->cbData == 0 || phFile == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    // Use the internal function fo open the file
    return OpenFileByCKey(hs, pCKey, (TCascFile **)phFile);
}

bool WINAPI CascOpenFile(HANDLE hStorage, const char * szFileName, DWORD dwLocale, DWORD dwFlags, HANDLE * phFile)
{
    TCascStorage * hs;
    QUERY_KEY QueryKey;
    LPBYTE pbQueryKey = NULL;
    BYTE KeyBuffer[MD5_HASH_SIZE];
    DWORD dwContentSize = CASC_INVALID_SIZE;
    DWORD dwOpenMode = 0;
    bool bOpenResult = false;
    int nError = ERROR_SUCCESS;

    CASCLIB_UNUSED(dwLocale);

    // Validate the storage handle
    hs = IsValidStorageHandle(hStorage);
    if(hs == NULL)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    // Validate the other parameters
    if(szFileName == NULL || szFileName[0] == 0 || phFile == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    // Retrieve the CKey/EKey from the file name in different modes
    switch(dwFlags & CASC_OPEN_TYPE_MASK)
    {
        case CASC_OPEN_BY_NAME:
            
            // Retrieve the file CKey/EKey
            pbQueryKey = RootHandler_GetKey(hs->pRootHandler, szFileName, &dwContentSize);
            if(pbQueryKey == NULL)
            {
                nError = ERROR_FILE_NOT_FOUND;
                break;
            }

            // Set the proper open mode
            dwOpenMode = (hs->pRootHandler->dwRootFlags & ROOT_FLAG_USES_EKEY) ? CASC_OPEN_BY_EKEY : CASC_OPEN_BY_CKEY;
            break;

        case CASC_OPEN_BY_CKEY:

            // Convert the string to binary
            nError = ConvertStringToBinary(szFileName, MD5_STRING_SIZE, KeyBuffer);
            if(nError != ERROR_SUCCESS)
                break;
            
            // Proceed opening by CKey
            dwOpenMode = CASC_OPEN_BY_CKEY;
            pbQueryKey = KeyBuffer;
            break;

        case CASC_OPEN_BY_EKEY:

            // Convert the string to binary
            nError = ConvertStringToBinary(szFileName, MD5_STRING_SIZE, KeyBuffer);
            if(nError != ERROR_SUCCESS)
                break;
            
            // Proceed opening by EKey
            dwOpenMode = CASC_OPEN_BY_EKEY;
            pbQueryKey = KeyBuffer;
            break;

        default:

            // Unknown open mode
            nError = ERROR_INVALID_PARAMETER;
            break;
    }

    // Perform the open operation
    if(nError == ERROR_SUCCESS)
    {
        // Setup the CKey/EKey
        QueryKey.pbData = pbQueryKey;
        QueryKey.cbData = MD5_HASH_SIZE;

        // Either open by CKey or EKey
        switch(dwOpenMode)
        {
            case CASC_OPEN_BY_CKEY:
                bOpenResult = OpenFileByCKey(hs, &QueryKey, (TCascFile **)phFile);
                break;

            case CASC_OPEN_BY_EKEY:
                bOpenResult = OpenFileByEKey(hs, NULL, &QueryKey, dwContentSize, (TCascFile **)phFile);
                break;

            default:
                SetLastError(ERROR_INVALID_PARAMETER);
                break;
        }

        // Handle the error code
        if(!bOpenResult)
        {
            assert(GetLastError() != ERROR_SUCCESS);
            nError = GetLastError();
        }
    }

    // Set the last error and return
    if(nError != ERROR_SUCCESS)
        SetLastError(nError);
    return (nError == ERROR_SUCCESS);
}

DWORD WINAPI CascGetFileId(HANDLE hStorage, const char * szFileName)
{
    TCascStorage * hs;

    // Validate the storage handle
    hs = IsValidStorageHandle(hStorage);
    if (hs == NULL)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    // Validate the other parameters
    if (szFileName == NULL || szFileName[0] == 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    return RootHandler_GetFileId(hs->pRootHandler, szFileName);
}

bool WINAPI CascCloseFile(HANDLE hFile)
{
    TCascFile * hf;

    hf = IsValidFileHandle(hFile);
    if(hf != NULL)
    {
        // Close (dereference) the archive handle
        if(hf->hs != NULL)
            CascCloseStorage((HANDLE)hf->hs);
        hf->hs = NULL;

        // Free the file cache and frame array
        if(hf->pbFileCache != NULL)
            CASC_FREE(hf->pbFileCache);
        if(hf->pFrames != NULL)
            CASC_FREE(hf->pFrames);

        // Free the structure itself
        hf->szClassName = NULL;
        CASC_FREE(hf);
        return true;
    }

    SetLastError(ERROR_INVALID_HANDLE);
    return false;
}

