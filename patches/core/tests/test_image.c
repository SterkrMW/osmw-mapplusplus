#include "core_internal.h"
#include "test_pe_fixture.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void check(BOOL condition, const char *name)
{
    if (condition) {
        printf("ok:   %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        ++g_failures;
    }
}

static void expect_invalid(const char *path, BYTE *file, DWORD size,
                           const char *name)
{
    check(test_pe_write(path, file, size), "malformed fixture written");
    check(!osmw_image_load_file(path, TEST_PE_IMAGE_BASE), name);
    check(osmw_image_last_error()[0] != '\0',
          "malformed image supplies a diagnostic");
    osmw_image_reset();
}

int main(void)
{
    BYTE file[TEST_PE_FILE_SIZE];
    BYTE text[32];
    char path[MAX_PATH];
    OSMW_IMAGE_VIEW view;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *section;
    DWORD size;

    memset(text, 0x90, sizeof(text));
    check(test_pe_temp_path(path, sizeof(path)), "temporary PE path created");
    size = test_pe_build(file, text, sizeof(text));
    check(size == TEST_PE_FILE_SIZE && test_pe_write(path, file, size),
          "valid PE32 fixture written");

    check(osmw_image_load_file(path, TEST_PE_IMAGE_BASE),
          "valid x86 image accepted");
    check(osmw_image_get_view(&view), "immutable image view available");
    check(view.file_size == TEST_PE_FILE_SIZE &&
              view.text_size == sizeof(text) &&
              view.text_rva == TEST_PE_TEXT_RVA,
          "raw .text bounds captured");
    check(view.preferred_base == TEST_PE_IMAGE_BASE &&
              view.loaded_base == TEST_PE_IMAGE_BASE &&
              view.address_compatible,
          "preferred and loaded bases captured");
    check(view.timestamp == TEST_PE_TIMESTAMP &&
              view.image_size == TEST_PE_IMAGE_SIZE,
          "timestamp and SizeOfImage captured");
    check(memcmp(view.text, text, sizeof(text)) == 0,
          "view points at pristine raw .text bytes");

    file[0] = 0;
    check(osmw_image_load_file(path, 0x00500000u),
          "second load reuses the retained image");
    check(osmw_image_get_view(&view) &&
              view.loaded_base == TEST_PE_IMAGE_BASE && view.text[0] == 0x90,
          "retained image is unchanged by a second request");
    osmw_image_reset();

    size = test_pe_build(file, text, sizeof(text));
    check(test_pe_write(path, file, size), "base-mismatch fixture written");
    check(osmw_image_load_file(path, 0x00500000u),
          "relocated image metadata still loads");
    check(osmw_image_get_view(&view) && !view.address_compatible,
          "loaded/preferred base mismatch is recorded");
    osmw_image_reset();

    size = test_pe_build(file, text, sizeof(text));
    file[0] = 0;
    expect_invalid(path, file, size, "bad DOS signature rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    dos->e_lfanew = 0x7FFFFFFF;
    expect_invalid(path, file, size, "out-of-file PE offset rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    expect_invalid(path, file, size, "PE32+ optional header rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    nt->FileHeader.NumberOfSections = 96;
    expect_invalid(path, file, size, "out-of-file section table rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    section = IMAGE_FIRST_SECTION(nt);
    section->PointerToRawData = TEST_PE_FILE_SIZE - 8;
    expect_invalid(path, file, size, "out-of-file raw section rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    section = IMAGE_FIRST_SECTION(nt);
    section->PointerToRawData = 0x100;
    section->SizeOfRawData = 0x100;
    expect_invalid(path, file, size, "header-overlapping raw section rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    section = IMAGE_FIRST_SECTION(nt);
    section->VirtualAddress = TEST_PE_IMAGE_SIZE - 8;
    expect_invalid(path, file, size, "out-of-image virtual section rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    section = IMAGE_FIRST_SECTION(nt);
    section->Characteristics &= ~IMAGE_SCN_MEM_EXECUTE;
    expect_invalid(path, file, size, "non-executable .text rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    section = IMAGE_FIRST_SECTION(nt);
    memcpy(section->Name, ".data\0\0\0", 8);
    expect_invalid(path, file, size, "missing .text section rejected");

    size = test_pe_build(file, text, sizeof(text));
    dos = (IMAGE_DOS_HEADER *)file;
    nt = (IMAGE_NT_HEADERS32 *)(file + dos->e_lfanew);
    section = IMAGE_FIRST_SECTION(nt);
    nt->FileHeader.NumberOfSections = 2;
    section[1] = section[0];
    expect_invalid(path, file, size, "duplicate .text sections rejected");

    expect_invalid(path, file, sizeof(IMAGE_DOS_HEADER),
                   "truncated PE header rejected");

    (void)DeleteFileA(path);
    printf("%s: pristine PE image, %d failure(s)\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
