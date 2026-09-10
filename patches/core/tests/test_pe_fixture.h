#ifndef OSMW_TEST_PE_FIXTURE_H
#define OSMW_TEST_PE_FIXTURE_H

#include <windows.h>

#include <string.h>

#define TEST_PE_FILE_SIZE 0x600u
#define TEST_PE_HEADER_SIZE 0x200u
#define TEST_PE_TEXT_RAW 0x200u
#define TEST_PE_TEXT_CAP 0x400u
#define TEST_PE_TEXT_RVA 0x1000u
#define TEST_PE_IMAGE_BASE 0x00400000u
#define TEST_PE_IMAGE_SIZE 0x2000u
#define TEST_PE_TIMESTAMP 0x45650696u

static DWORD test_pe_build(BYTE *file, const BYTE *text, DWORD text_size)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *section;

    if (text_size > TEST_PE_TEXT_CAP) {
        return 0;
    }
    ZeroMemory(file, TEST_PE_FILE_SIZE);
    dos = (IMAGE_DOS_HEADER *)file;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.TimeDateStamp = TEST_PE_TIMESTAMP;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    nt->FileHeader.Characteristics =
        IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_32BIT_MACHINE;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.ImageBase = TEST_PE_IMAGE_BASE;
    nt->OptionalHeader.SectionAlignment = 0x1000;
    nt->OptionalHeader.FileAlignment = 0x200;
    nt->OptionalHeader.SizeOfImage = TEST_PE_IMAGE_SIZE;
    nt->OptionalHeader.SizeOfHeaders = TEST_PE_HEADER_SIZE;
    nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;

    section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".text", 5);
    section->Misc.VirtualSize = text_size;
    section->VirtualAddress = TEST_PE_TEXT_RVA;
    section->SizeOfRawData = TEST_PE_TEXT_CAP;
    section->PointerToRawData = TEST_PE_TEXT_RAW;
    section->Characteristics =
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
    if (text != NULL && text_size != 0) {
        memcpy(file + TEST_PE_TEXT_RAW, text, text_size);
    }
    return TEST_PE_FILE_SIZE;
}

static BOOL test_pe_temp_path(char *output, DWORD output_size)
{
    char temporary[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(temporary), temporary);

    if (length == 0 || length >= sizeof(temporary) ||
            output_size < MAX_PATH) {
        return FALSE;
    }
    return GetTempFileNameA(temporary, "ocp", 0, output) != 0;
}

static BOOL test_pe_write(const char *path, const BYTE *file, DWORD file_size)
{
    HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL result;

    if (handle == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    result = WriteFile(handle, file, file_size, &written, NULL) &&
             written == file_size;
    CloseHandle(handle);
    return result;
}

#endif /* OSMW_TEST_PE_FIXTURE_H */
