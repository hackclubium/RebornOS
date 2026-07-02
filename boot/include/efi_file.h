#ifndef REBORNOS_EFI_FILE_H
#define REBORNOS_EFI_FILE_H

/* EFI_LOADED_IMAGE_PROTOCOL, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL and
 * EFI_FILE_PROTOCOL: enough of each to locate the ESP we booted from,
 * open it, and read \kernel.elf into memory. Hand-transcribed from the
 * public UEFI spec, same rules as efi.h (field order matters, unused
 * fields are opaque placeholders of the right size). */

#include "efi.h"

/* {5B1B31A1-9562-11d2-8E3F-00A0C969723B} */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

/* {964E5B22-6459-11d2-8E39-00A0C969723B} */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* {09576E92-6D3F-11D2-8E39-00A0C969723B} */
#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    VOID *FilePath; /* EFI_DEVICE_PATH_PROTOCOL*, opaque -- never dereferenced */
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;
    UINT64 ImageSize;
    EFI_MEMORY_TYPE ImageCodeType;
    EFI_MEMORY_TYPE ImageDataType;
    EFI_UNUSED_SLOT Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, VOID *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(EFI_FILE_PROTOCOL *This, UINT64 Position);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize, VOID *Buffer);

struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_FILE_OPEN Open;
    EFI_FILE_CLOSE Close;
    EFI_UNUSED_SLOT Delete;
    EFI_FILE_READ Read;
    EFI_UNUSED_SLOT Write;
    EFI_UNUSED_SLOT GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO GetInfo;
    EFI_UNUSED_SLOT SetInfo;
    EFI_UNUSED_SLOT Flush;
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
};

typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef struct {
    UINT64 Size;
    UINT64 FileSize;
    UINT64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

#endif /* REBORNOS_EFI_FILE_H */
