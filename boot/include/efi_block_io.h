#ifndef REBORNOS_EFI_BLOCK_IO_H
#define REBORNOS_EFI_BLOCK_IO_H

/* EFI_BLOCK_IO_PROTOCOL: enough to read raw LBA blocks off the same
 * device handle SimpleFileSystem already gave us access to. For a FAT
 * partition, this handle's block space *is* the FAT volume's own LBA
 * space (LBA 0 is the FAT32 boot sector) -- no MBR/GPT parsing needed.
 * Hand-transcribed from the public UEFI spec, same rules as efi.h. */

#include "efi.h"

/* {964E5B21-6459-11D2-8E39-00A0C969723B} */
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT32 MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32 BlockSize;
    UINT32 IoAlign;
    UINT64 LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_RESET)(EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_BLOCK_READ)(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, UINT64 Lba, UINTN BufferSize, VOID *Buffer);

struct EFI_BLOCK_IO_PROTOCOL {
    UINT64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_BLOCK_RESET Reset;
    EFI_BLOCK_READ ReadBlocks;
    EFI_UNUSED_SLOT WriteBlocks;
    EFI_UNUSED_SLOT FlushBlocks;
};

#endif /* REBORNOS_EFI_BLOCK_IO_H */
