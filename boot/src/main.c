#include "efi.h"
#include "efi_file.h"
#include "efi_gop.h"
#include "elf64.h"
#include "boot_info.h"
#include "serial.h"
#include "minilib.h"

static const EFI_GUID gEfiLoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static const EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;
static const EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

static void log(const char *s) {
    serial_write(s);
}

static void log_hex(uint64_t v) {
    char buf[19] = "0x0000000000000000";
    for (int i = 0; i < 16; i++) {
        int shift = (15 - i) * 4;
        uint8_t nibble = (v >> shift) & 0xF;
        buf[2 + i] = (char)(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10));
    }
    serial_write(buf);
}

static void halt(void) {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void fatal(const char *msg) {
    log("FATAL: ");
    log(msg);
    log("\n");
    halt();
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;
    EFI_STATUS status;

    serial_init();
    log("RebornOS bootloader: starting\n");

    /* 1. Find the device we were loaded from. */
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    status = bs->HandleProtocol(ImageHandle, (EFI_GUID *)&gEfiLoadedImageProtocolGuid, (VOID **)&loaded_image);
    if (EFI_ERROR(status)) {
        fatal("HandleProtocol(LoadedImage) failed");
    }

    /* 2. Open its filesystem and read /kernel.elf from the same ESP. */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    status = bs->HandleProtocol(loaded_image->DeviceHandle, (EFI_GUID *)&gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        fatal("HandleProtocol(SimpleFileSystem) failed");
    }

    EFI_FILE_PROTOCOL *root = NULL;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        fatal("OpenVolume failed");
    }

    EFI_FILE_PROTOCOL *kernel_file = NULL;
    status = root->Open(root, &kernel_file, (CHAR16 *)u"\\kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        fatal("cannot open \\kernel.elf on the ESP");
    }

    UINT8 info_buf[512];
    UINTN info_size = sizeof(info_buf);
    status = kernel_file->GetInfo(kernel_file, (EFI_GUID *)&gEfiFileInfoGuid, &info_size, info_buf);
    if (EFI_ERROR(status)) {
        fatal("GetInfo(kernel.elf) failed");
    }
    EFI_FILE_INFO *file_info = (EFI_FILE_INFO *)info_buf;
    UINTN kernel_file_size = (UINTN)file_info->FileSize;

    log("kernel.elf size: ");
    log_hex(kernel_file_size);
    log("\n");

    VOID *kernel_file_buffer = NULL;
    status = bs->AllocatePool(EfiLoaderData, kernel_file_size, &kernel_file_buffer);
    if (EFI_ERROR(status)) {
        fatal("AllocatePool(kernel file buffer) failed");
    }

    UINTN read_size = kernel_file_size;
    status = kernel_file->Read(kernel_file, &read_size, kernel_file_buffer);
    if (EFI_ERROR(status) || read_size != kernel_file_size) {
        fatal("Read(kernel.elf) failed");
    }
    kernel_file->Close(kernel_file);

    /* 3. Parse the ELF64 header and load every PT_LOAD segment at its
     * physical address. UEFI hands us the CPU already in long mode with
     * an identity map covering usable RAM, so "physical address" and
     * "the address the kernel runs at" are the same thing in Milestone 0. */
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)kernel_file_buffer;
    if (ehdr->e_ident[0] != ELF_MAGIC0 || ehdr->e_ident[1] != ELF_MAGIC1 ||
        ehdr->e_ident[2] != ELF_MAGIC2 || ehdr->e_ident[3] != ELF_MAGIC3) {
        fatal("kernel.elf: bad ELF magic");
    }
    if (ehdr->e_ident[4] != ELFCLASS64 || ehdr->e_machine != EM_X86_64) {
        fatal("kernel.elf: not a 64-bit x86_64 ELF");
    }

    Elf64_Phdr *phdrs = (Elf64_Phdr *)((UINT8 *)kernel_file_buffer + ehdr->e_phoff);
    for (UINT16 i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) {
            continue;
        }

        UINTN pages = (ph->p_memsz + 4095) / 4096;
        EFI_PHYSICAL_ADDRESS addr = ph->p_paddr;
        status = bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr);
        if (EFI_ERROR(status)) {
            fatal("AllocatePages(PT_LOAD segment) failed");
        }

        memcpy((VOID *)(UINTN)ph->p_paddr, (UINT8 *)kernel_file_buffer + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset((VOID *)((UINTN)ph->p_paddr + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
        }
    }
    UINT64 kernel_entry_addr = ehdr->e_entry;
    log("kernel entry point: ");
    log_hex(kernel_entry_addr);
    log("\n");

    /* 4. Grab the framebuffer from GOP's current mode -- we don't call
     * SetMode, we just use whatever the firmware already initialized. */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    status = bs->LocateProtocol((EFI_GUID *)&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status)) {
        fatal("LocateProtocol(GOP) failed");
    }

    boot_info_t *info = NULL;
    status = bs->AllocatePool(EfiLoaderData, sizeof(boot_info_t), (VOID **)&info);
    if (EFI_ERROR(status)) {
        fatal("AllocatePool(boot_info) failed");
    }
    memset(info, 0, sizeof(*info));

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info = gop->Mode->Info;
    if (mode_info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
        info->framebuffer.red_shift = 0;
        info->framebuffer.green_shift = 8;
        info->framebuffer.blue_shift = 16;
    } else if (mode_info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        info->framebuffer.red_shift = 16;
        info->framebuffer.green_shift = 8;
        info->framebuffer.blue_shift = 0;
    } else {
        fatal("GOP pixel format unsupported (need packed 32bpp RGB/BGR)");
    }

    info->framebuffer.base = gop->Mode->FrameBufferBase;
    info->framebuffer.size = gop->Mode->FrameBufferSize;
    info->framebuffer.width = mode_info->HorizontalResolution;
    info->framebuffer.height = mode_info->VerticalResolution;
    info->framebuffer.pixels_per_scanline = mode_info->PixelsPerScanLine;
    info->framebuffer.bytes_per_pixel = 4;

    log("framebuffer: ");
    log_hex(info->framebuffer.width);
    log(" x ");
    log_hex(info->framebuffer.height);
    log(" @ ");
    log_hex(info->framebuffer.base);
    log("\n");

    /* 5. Get the memory map and exit boot services. Per spec, any pool
     * allocation between GetMemoryMap and ExitBootServices can change
     * the map and invalidate MapKey, so we retry once if that happens. */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_version = 0;
    VOID *map_buffer = NULL;

    for (int attempt = 0; attempt < 3; attempt++) {
        map_size = 0;
        status = bs->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_version);
        if (status != EFI_BUFFER_TOO_SMALL) {
            fatal("GetMemoryMap(sizing) returned unexpected status");
        }

        map_size += desc_size * 8; /* headroom for the allocation below */
        if (map_buffer) {
            bs->FreePool(map_buffer);
        }
        status = bs->AllocatePool(EfiLoaderData, map_size, &map_buffer);
        if (EFI_ERROR(status)) {
            fatal("AllocatePool(memory map) failed");
        }

        status = bs->GetMemoryMap(&map_size, (EFI_MEMORY_DESCRIPTOR *)map_buffer, &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(status)) {
            fatal("GetMemoryMap(fetch) failed");
        }

        status = bs->ExitBootServices(ImageHandle, map_key);
        if (!EFI_ERROR(status)) {
            break;
        }
        if (attempt == 2) {
            fatal("ExitBootServices failed after retries");
        }
    }

    info->memory_map_addr = (uint64_t)(UINTN)map_buffer;
    info->memory_map_size = map_size;
    info->memory_map_descriptor_size = desc_size;
    info->memory_map_descriptor_version = desc_version;
    info->magic = BOOT_INFO_MAGIC;

    /* 6. Jump into the kernel. The kernel ELF was built with a plain
     * SysV ABI cross-compiler, but we (the bootloader) were built with
     * the Microsoft x64 ABI (mingw's default). Marking the function
     * pointer sysv_abi tells GCC to marshal the call correctly across
     * that boundary -- no assembly trampoline needed for this part. */
    typedef void (*kernel_entry_fn)(boot_info_t *) __attribute__((sysv_abi));
    kernel_entry_fn kernel_entry = (kernel_entry_fn)(UINTN)kernel_entry_addr;
    kernel_entry(info);

    /* Unreachable: the kernel does not return in Milestone 0. */
    halt();
    return EFI_SUCCESS;
}
