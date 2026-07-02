#ifndef REBORNOS_EFI_H
#define REBORNOS_EFI_H

/*
 * Hand-written subset of the UEFI 2.x specification. This is NOT copied
 * from gnu-efi or EDK2 -- it is transcribed from the public UEFI spec,
 * limited to exactly the types, protocols, and Boot Services calls that
 * boot/src/main.c actually uses.
 *
 * Struct field ORDER matters: these tables are called through as raw
 * function-pointer offsets by the firmware, so getting an order wrong
 * means the wrong function gets called and QEMU crashes or resets. Slots
 * we never call are still declared (as EFI_UNUSED_SLOT) purely to keep
 * every later field at its correct offset.
 */

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   BOOLEAN;
typedef int64_t   INTN;
typedef uint64_t  UINTN;
typedef int8_t    INT8;
typedef uint8_t   UINT8;
typedef int16_t   INT16;
typedef uint16_t  UINT16;
typedef int32_t   INT32;
typedef uint32_t  UINT32;
typedef int64_t   INT64;
typedef uint64_t  UINT64;
typedef uint16_t  CHAR16;
typedef void      VOID;
typedef UINTN     EFI_STATUS;
typedef VOID     *EFI_HANDLE;
typedef VOID     *EFI_EVENT;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;

#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)

/* UEFI x86_64 uses the Microsoft x64 calling convention for every
 * firmware entry point and callback. */
#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS 0
#define EFIERR(n) (0x8000000000000000ULL | (n))
#define EFI_BUFFER_TOO_SMALL EFIERR(5)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_ERROR(status) (((INTN)(status)) < 0)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* Generic placeholder for table slots whose exact signature we never
 * call through. Same size (8 bytes) and calling convention as every
 * other slot, so it doesn't disturb layout. */
typedef VOID (EFIAPI *EFI_UNUSED_SLOT)(VOID);

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ---- Memory services ------------------------------------------------ */

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32 Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    UINT64 NumberOfPages;
    UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(VOID *Buffer);

/* ---- Protocol handling / misc slots we declare but never call ------- */

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(EFI_GUID *Protocol, VOID *Registration, VOID **Interface);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);

/*
 * EFI_BOOT_SERVICES, full field order per the UEFI spec. We only ever
 * call AllocatePages, GetMemoryMap, AllocatePool, HandleProtocol,
 * LocateProtocol, and ExitBootServices -- everything else is a correctly
 * sized/positioned EFI_UNUSED_SLOT.
 */
typedef struct {
    EFI_TABLE_HEADER Hdr;

    EFI_UNUSED_SLOT RaiseTPL;
    EFI_UNUSED_SLOT RestoreTPL;

    EFI_ALLOCATE_PAGES AllocatePages;
    EFI_FREE_PAGES FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    EFI_ALLOCATE_POOL AllocatePool;
    EFI_FREE_POOL FreePool;

    EFI_UNUSED_SLOT CreateEvent;
    EFI_UNUSED_SLOT SetTimer;
    EFI_UNUSED_SLOT WaitForEvent;
    EFI_UNUSED_SLOT SignalEvent;
    EFI_UNUSED_SLOT CloseEvent;
    EFI_UNUSED_SLOT CheckEvent;

    EFI_UNUSED_SLOT InstallProtocolInterface;
    EFI_UNUSED_SLOT ReinstallProtocolInterface;
    EFI_UNUSED_SLOT UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL HandleProtocol;
    EFI_UNUSED_SLOT Reserved;
    EFI_UNUSED_SLOT RegisterProtocolNotify;
    EFI_UNUSED_SLOT LocateHandle;
    EFI_UNUSED_SLOT LocateDevicePath;
    EFI_UNUSED_SLOT InstallConfigurationTable;

    EFI_UNUSED_SLOT LoadImage;
    EFI_UNUSED_SLOT StartImage;
    EFI_UNUSED_SLOT Exit;
    EFI_UNUSED_SLOT UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;

    EFI_UNUSED_SLOT GetNextMonotonicCount;
    EFI_STALL Stall;
    EFI_UNUSED_SLOT SetWatchdogTimer;

    EFI_UNUSED_SLOT ConnectController;
    EFI_UNUSED_SLOT DisconnectController;

    EFI_UNUSED_SLOT OpenProtocol;
    EFI_UNUSED_SLOT CloseProtocol;
    EFI_UNUSED_SLOT OpenProtocolInformation;

    EFI_UNUSED_SLOT ProtocolsPerHandle;
    EFI_UNUSED_SLOT LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL LocateProtocol;
    EFI_UNUSED_SLOT InstallMultipleProtocolInterfaces;
    EFI_UNUSED_SLOT UninstallMultipleProtocolInterfaces;

    EFI_UNUSED_SLOT CalculateCrc32;

    EFI_UNUSED_SLOT CopyMem;
    EFI_UNUSED_SLOT SetMem;
    EFI_UNUSED_SLOT CreateEventEx;
} EFI_BOOT_SERVICES;

/* We never call anything through Runtime Services or ConOut/ConIn in
 * Milestone 0 (all boot logging goes over our own serial driver), so
 * they stay opaque -- we only ever store their pointers. */
typedef struct EFI_RUNTIME_SERVICES EFI_RUNTIME_SERVICES;
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
    EFI_GUID VendorGuid;
    VOID *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#endif /* REBORNOS_EFI_H */
