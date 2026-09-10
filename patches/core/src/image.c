/* Immutable PE32 image loaded from the exact process executable on disk. */
#include "core_internal.h"

#include <stddef.h>
#include <string.h>

#define OSMW_IMAGE_MAX_FILE_SIZE (64u * 1024u * 1024u)
#define OSMW_IMAGE_MAX_SECTIONS 96u

struct osmw_image_state {
    BYTE *data;
    DWORD file_size;
    const BYTE *text;
    DWORD text_size;
    DWORD text_rva;
    DWORD preferred_base;
    DWORD loaded_base;
    DWORD timestamp;
    DWORD image_size;
    BOOL address_compatible;
};

static struct osmw_image_state g_image;
static char g_last_error[160];

static BOOL fail(const char *message)
{
    size_t length = strlen(message);
    if (length >= sizeof(g_last_error)) {
        length = sizeof(g_last_error) - 1;
    }
    memcpy(g_last_error, message, length);
    g_last_error[length] = '\0';
    return FALSE;
}

static BOOL range_within(DWORD offset, DWORD length, DWORD total)
{
    return offset <= total && length <= total - offset;
}

static BOOL parse_image(struct osmw_image_state *image)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_FILE_HEADER *file_header;
    IMAGE_OPTIONAL_HEADER32 *optional;
    IMAGE_SECTION_HEADER *sections;
    DWORD pe_offset;
    DWORD file_header_offset;
    DWORD optional_offset;
    DWORD section_offset;
    DWORD section_bytes;
    DWORD index;
    DWORD text_count = 0;
    static const BYTE text_name[IMAGE_SIZEOF_SHORT_NAME] =
        {'.', 't', 'e', 'x', 't', 0, 0, 0};

    if (!range_within(0, sizeof(IMAGE_DOS_HEADER), image->file_size)) {
        return fail("file is shorter than an IMAGE_DOS_HEADER");
    }
    dos = (IMAGE_DOS_HEADER *)image->data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return fail("invalid DOS header");
    }
    pe_offset = (DWORD)dos->e_lfanew;
    if (!range_within(pe_offset, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER),
                      image->file_size)) {
        return fail("PE header runs past the file");
    }
    if (*(DWORD *)(image->data + pe_offset) != IMAGE_NT_SIGNATURE) {
        return fail("invalid PE signature");
    }

    file_header_offset = pe_offset + sizeof(DWORD);
    file_header = (IMAGE_FILE_HEADER *)(image->data + file_header_offset);
    if (file_header->Machine != IMAGE_FILE_MACHINE_I386 ||
            (file_header->Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) == 0) {
        return fail("image is not an x86 executable");
    }
    if (file_header->NumberOfSections == 0 ||
            file_header->NumberOfSections > OSMW_IMAGE_MAX_SECTIONS) {
        return fail("invalid PE section count");
    }
    if (file_header->SizeOfOptionalHeader <
            sizeof(IMAGE_OPTIONAL_HEADER32)) {
        return fail("truncated PE32 optional header");
    }

    optional_offset = file_header_offset + sizeof(IMAGE_FILE_HEADER);
    if (!range_within(optional_offset, file_header->SizeOfOptionalHeader,
                      image->file_size)) {
        return fail("optional header runs past the file");
    }
    optional = (IMAGE_OPTIONAL_HEADER32 *)(image->data + optional_offset);
    if (optional->Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return fail("image is not PE32");
    }
    if (optional->ImageBase == 0 || optional->SizeOfImage == 0 ||
            optional->ImageBase > MAXDWORD - optional->SizeOfImage) {
        return fail("invalid preferred image range");
    }

    section_offset = optional_offset + file_header->SizeOfOptionalHeader;
    section_bytes = (DWORD)file_header->NumberOfSections *
                    (DWORD)sizeof(IMAGE_SECTION_HEADER);
    if (!range_within(section_offset, section_bytes, image->file_size) ||
            optional->SizeOfHeaders < section_offset + section_bytes ||
            optional->SizeOfHeaders > image->file_size) {
        return fail("section table is outside the PE headers");
    }
    sections = (IMAGE_SECTION_HEADER *)(image->data + section_offset);

    for (index = 0; index < file_header->NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER *section = &sections[index];
        DWORD virtual_size = section->Misc.VirtualSize;
        DWORD virtual_span = virtual_size;

        if (section->SizeOfRawData != 0 &&
                (section->PointerToRawData < optional->SizeOfHeaders ||
                 !range_within(section->PointerToRawData,
                               section->SizeOfRawData, image->file_size))) {
            return fail("a section raw range overlaps headers or the file end");
        }
        if (virtual_span < section->SizeOfRawData) {
            virtual_span = section->SizeOfRawData;
        }
        if (virtual_span != 0 &&
                !range_within(section->VirtualAddress, virtual_span,
                              optional->SizeOfImage)) {
            return fail("a section virtual range runs past SizeOfImage");
        }

        if (memcmp(section->Name, text_name, sizeof(text_name)) == 0) {
            DWORD scan_size = virtual_size;
            ++text_count;
            if ((section->Characteristics &
                    (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)) !=
                    (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)) {
                return fail(".text is not marked executable code");
            }
            if (scan_size == 0 || scan_size > section->SizeOfRawData) {
                scan_size = section->SizeOfRawData;
            }
            if (scan_size == 0 ||
                    !range_within(section->PointerToRawData, scan_size,
                                  image->file_size) ||
                    !range_within(section->VirtualAddress, scan_size,
                                  optional->SizeOfImage)) {
                return fail("invalid .text section range");
            }
            image->text = image->data + section->PointerToRawData;
            image->text_size = scan_size;
            image->text_rva = section->VirtualAddress;
        }
    }

    if (text_count != 1) {
        return fail(text_count == 0
            ? "image has no .text section"
            : "image has multiple .text sections");
    }
    image->preferred_base = optional->ImageBase;
    image->timestamp = file_header->TimeDateStamp;
    image->image_size = optional->SizeOfImage;
    image->address_compatible = image->loaded_base == image->preferred_base;
    return TRUE;
}

BOOL osmw_image_load_file(const char *path, DWORD loaded_base)
{
    struct osmw_image_state image;
    LARGE_INTEGER file_size;
    HANDLE file;
    DWORD read = 0;

    if (g_image.data != NULL) {
        return TRUE;
    }
    g_last_error[0] = '\0';
    if (path == NULL || path[0] == '\0' || loaded_base == 0) {
        return fail("invalid image path or loaded base");
    }

    ZeroMemory(&image, sizeof(image));
    image.loaded_base = loaded_base;
    file = CreateFileA(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return fail("cannot open the process executable");
    }
    if (!GetFileSizeEx(file, &file_size) || file_size.HighPart != 0 ||
            file_size.LowPart < sizeof(IMAGE_DOS_HEADER) ||
            file_size.LowPart > OSMW_IMAGE_MAX_FILE_SIZE) {
        CloseHandle(file);
        return fail("process executable has an implausible size");
    }
    image.file_size = file_size.LowPart;
    image.data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, image.file_size);
    if (image.data == NULL) {
        CloseHandle(file);
        return fail("cannot allocate the immutable executable image");
    }
    if (!ReadFile(file, image.data, image.file_size, &read, NULL) ||
            read != image.file_size) {
        CloseHandle(file);
        HeapFree(GetProcessHeap(), 0, image.data);
        return fail("short read of the process executable");
    }
    CloseHandle(file);

    if (!parse_image(&image)) {
        HeapFree(GetProcessHeap(), 0, image.data);
        return FALSE;
    }
    g_image = image;
    return TRUE;
}

BOOL osmw_image_load_process(void)
{
    char path[MAX_PATH];
    HMODULE module;
    DWORD length;

    if (g_image.data != NULL) {
        return TRUE;
    }
    length = GetModuleFileNameA(NULL, path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) {
        return fail("cannot obtain the exact process executable path");
    }
    module = GetModuleHandleA(NULL);
    if (module == NULL) {
        return fail("cannot obtain the loaded process image base");
    }
    return osmw_image_load_file(path, (DWORD)(ULONG_PTR)module);
}

BOOL osmw_image_get_view(OSMW_IMAGE_VIEW *output)
{
    if (output == NULL || g_image.data == NULL) {
        return FALSE;
    }
    output->file_data = g_image.data;
    output->file_size = g_image.file_size;
    output->text = g_image.text;
    output->text_size = g_image.text_size;
    output->text_rva = g_image.text_rva;
    output->preferred_base = g_image.preferred_base;
    output->loaded_base = g_image.loaded_base;
    output->timestamp = g_image.timestamp;
    output->image_size = g_image.image_size;
    output->address_compatible = g_image.address_compatible;
    return TRUE;
}

const char *osmw_image_last_error(void)
{
    return g_last_error;
}

void osmw_image_reset(void)
{
    if (g_image.data != NULL) {
        HeapFree(GetProcessHeap(), 0, g_image.data);
    }
    ZeroMemory(&g_image, sizeof(g_image));
    g_last_error[0] = '\0';
}
