/** @file
  RamPartitionDxe.c — Open-source provider of the EFI_RAMPARTITION_PROTOCOL.

  Strategy:
    1. Try the modern Qualcomm "global partition" SMEM layout
       (magic 0x55EE73AA at SMEM+0x1000) and read item 15 from the target
       partition.
    2. Try the legacy SMEM alloc table (16-byte entries at SMEM+0x1000).
    3. Fall back to deriving usable RAM ranges from the UEFI boot services
       memory map (EfiBootServicesData regions are plain RAM).

  The RAM partition protocol is located by the library constructor of
  RamManagerLib, which vendor drivers (EnvDxe, SmemDxe, DisplayDxe,
  NT_DeviceInfoDxe) link in; without it those drivers fail their constructor
  and the DXE dispatch stalls.

  Copyright (c) 2026, K60 mondrian port project.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/EFIRamPartition.h>

#define SMEM_USABLE_RAM_PARTITION_TABLE  15
#define SMEM_GLOBAL_PARTITION_MAGIC       0x55EE73AA
#define SMEM_GLOBAL_PARTITION_OFFSET      0x1000
#define SMEM_ALLOC_TABLE_MAX_ITEMS       512

#pragma pack(1)
typedef struct {
  UINT32 Offset;
  UINT32 Size;
  UINT8  Attr;
  UINT8  Reserved[3];
  UINT32 Crc32;
} SMEM_ALLOC_ENTRY;

typedef struct {
  UINT64 StartAddress;
  UINT32 Size;
  UINT32 State;
  UINT32 Flag;
  UINT32 Reserved1;
  UINT32 Reserved2;
} RAM_PARTITION;
#pragma pack()

STATIC RAM_PARTITION             *mPartitions     = NULL;
STATIC UINT32                     mPartitionCount = 0;
STATIC EFI_RAMPARTITION_PROTOCOL  mRamPartitionProtocol;
STATIC EFI_HANDLE                 mHandle = NULL;

EFI_STATUS
EFIAPI
RamPartitionGetVersion (
  IN  EFI_RAMPARTITION_PROTOCOL *This,
  OUT UINT32                    *MajorVersion,
  OUT UINT32                    *MinorVersion
  )
{
  if ((MajorVersion == NULL) || (MinorVersion == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *MajorVersion = 1;
  *MinorVersion = 2;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RamPartitionGetHighestBankBit (
  IN  EFI_RAMPARTITION_PROTOCOL *This,
  OUT UINT32                    *HighestBankBit
  )
{
  if (HighestBankBit == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *HighestBankBit = 34;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RamPartitionGetMinPasrSize (
  IN  EFI_RAMPARTITION_PROTOCOL *This,
  OUT UINT32                    *MinPasrSize
  )
{
  if (MinPasrSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *MinPasrSize = 0;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RamPartitionGetPreLoadedImageTable (
  IN  EFI_RAMPARTITION_PROTOCOL *This,
  OUT PreLoadedImageEntry       *PreLoadedImageTable,
  IN  OUT UINT32                *NumPreloadedImg
  )
{
  if (NumPreloadedImg == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *NumPreloadedImg = 0;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RamPartitionGetRamPartitions (
  IN  EFI_RAMPARTITION_PROTOCOL *This,
  OUT RamPartitionEntry         *RamPartitions,
  IN  OUT UINT32                *NumPartition
  )
{
  UINT32 Count;

  if (NumPartition == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (RamPartitions == NULL) {
    *NumPartition = mPartitionCount;
    return (mPartitionCount != 0) ? EFI_BUFFER_TOO_SMALL : EFI_SUCCESS;
  }

  Count = (*NumPartition < mPartitionCount) ? *NumPartition : mPartitionCount;

  for (UINT32 Index = 0; Index < Count; Index++) {
    RamPartitions[Index].Base            = mPartitions[Index].StartAddress;
    RamPartitions[Index].AvailableLength = mPartitions[Index].Size;
  }

  *NumPartition = Count;

  return EFI_SUCCESS;
}

/**
  Fill mPartitions from a raw ram_partition_table payload.
**/
STATIC EFI_STATUS
ParsePartitionTable (
  IN VOID    *TableData,
  IN UINT32   MaxEntries
  )
{
  UINT32         Len;
  RAM_PARTITION *Src;

  Len = *(UINT32 *)((UINT8 *)TableData + 12);
  if ((Len == 0) || (Len > 128) || (Len > MaxEntries)) {
    DEBUG ((DEBUG_ERROR, "RamPartitionDxe: implausible partition count %u\n", Len));
    return EFI_NOT_FOUND;
  }

  mPartitions = (RAM_PARTITION *)AllocateZeroPool (sizeof (RAM_PARTITION) * Len);
  if (mPartitions == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Src = (RAM_PARTITION *)((UINT8 *)TableData + 16);

  for (UINT32 Index = 0; Index < Len; Index++) {
    mPartitions[Index].StartAddress = Src[Index].StartAddress;
    mPartitions[Index].Size         = Src[Index].Size;
    mPartitions[Index].State        = Src[Index].State;
    mPartitions[Index].Flag         = Src[Index].Flag;

    DEBUG ((
      DEBUG_INFO,
      "RamPartitionDxe: partition[%u] base=%llx size=%x state=%x\n",
      Index,
      mPartitions[Index].StartAddress,
      mPartitions[Index].Size,
      mPartitions[Index].State
      ));
  }

  mPartitionCount = Len;

  return EFI_SUCCESS;
}

/**
  Modern layout: global partition descriptor at SMEM+0x1000, item table inside
  the target partition.
**/
STATIC EFI_STATUS
TryGlobalPartition (
  IN VOID    *SmemBase,
  IN UINT64   SmemLength
  )
{
  UINT32            Magic;
  UINT32            EntryCount;
  UINT32            EntryOffset;
  SMEM_ALLOC_ENTRY *Entries;
  UINT32            PartitionOffset;
  UINT32            PartitionCount;
  UINT32           *PartitionWords;
  UINT32            ItemOffset;
  UINT32            ItemSize;

  if (SmemLength <= SMEM_GLOBAL_PARTITION_OFFSET + 16) {
    return EFI_NOT_FOUND;
  }

  Magic = *(UINT32 *)((UINT8 *)SmemBase + SMEM_GLOBAL_PARTITION_OFFSET);
  if (Magic != SMEM_GLOBAL_PARTITION_MAGIC) {
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "RamPartitionDxe: global partition descriptor found\n"));

  EntryCount  = *(UINT32 *)((UINT8 *)SmemBase + SMEM_GLOBAL_PARTITION_OFFSET + 8);
  EntryOffset = *(UINT32 *)((UINT8 *)SmemBase + SMEM_GLOBAL_PARTITION_OFFSET + 12);
  DEBUG ((
    DEBUG_INFO,
    "RamPartitionDxe: global partition entries=%u entry_offset=%x\n",
    EntryCount,
    EntryOffset
    ));

  if ((EntryCount == 0) || (EntryCount > 32)) {
    return EFI_NOT_FOUND;
  }

  Entries = (SMEM_ALLOC_ENTRY *)((UINT8 *)SmemBase + SMEM_GLOBAL_PARTITION_OFFSET + EntryOffset);
  if (((UINT8 *)Entries < (UINT8 *)SmemBase) ||
      ((UINT8 *)Entries + (sizeof (SMEM_ALLOC_ENTRY) * EntryCount) > (UINT8 *)SmemBase + SmemLength)) {
    return EFI_NOT_FOUND;
  }

  PartitionOffset = Entries[0].Offset;
  if ((PartitionOffset == 0) || (PartitionOffset >= SmemLength)) {
    return EFI_NOT_FOUND;
  }

  PartitionWords = (UINT32 *)((UINT8 *)SmemBase + PartitionOffset);

  DEBUG ((
    DEBUG_INFO,
    "RamPartitionDxe: target partition @%x words: %08x %08x %08x %08x\n",
    PartitionOffset,
    PartitionWords[0],
    PartitionWords[1],
    PartitionWords[2],
    PartitionWords[3]
    ));

  PartitionCount = PartitionWords[1];
  if ((PartitionCount == 0) || (PartitionCount > 512)) {
    PartitionCount = PartitionWords[0];
    if ((PartitionCount == 0) || (PartitionCount > 512)) {
      return EFI_NOT_FOUND;
    }
  }

  for (UINT32 Index = 0; Index < PartitionCount; Index++) {
    SMEM_ALLOC_ENTRY *Entry = (SMEM_ALLOC_ENTRY *)(PartitionWords + 4 + (Index * 4));
    if (Index == (SMEM_USABLE_RAM_PARTITION_TABLE & 0xFF)) {
      ItemOffset = Entry->Offset;
      ItemSize   = Entry->Size;
      if ((ItemOffset != 0) && (ItemSize > 32) &&
          ((UINT64)ItemOffset + ItemSize <= SmemLength)) {
        DEBUG ((
          DEBUG_INFO,
          "RamPartitionDxe: item 15 found at %x size %x\n",
          ItemOffset,
          ItemSize
          ));
        return ParsePartitionTable ((UINT8 *)SmemBase + ItemOffset, 128);
      }
    }
  }

  return EFI_NOT_FOUND;
}

/**
  Legacy layout: allocation table of 16-byte entries at SMEM+0x1000.
**/
STATIC EFI_STATUS
TryLegacyAllocTable (
  IN VOID    *SmemBase,
  IN UINT64   SmemLength
  )
{
  SMEM_ALLOC_ENTRY *AllocTable;
  SMEM_ALLOC_ENTRY *Item;

  if (SmemLength <= SMEM_GLOBAL_PARTITION_OFFSET + (sizeof (SMEM_ALLOC_ENTRY) * SMEM_ALLOC_TABLE_MAX_ITEMS)) {
    return EFI_NOT_FOUND;
  }

  AllocTable = (SMEM_ALLOC_ENTRY *)((UINT8 *)SmemBase + SMEM_GLOBAL_PARTITION_OFFSET);
  Item       = &AllocTable[SMEM_USABLE_RAM_PARTITION_TABLE];

  if ((Item->Size == 0) || (Item->Offset == 0xFFFFFFFF) || ((UINT64)Item->Offset + Item->Size > SmemLength)) {
    return EFI_NOT_FOUND;
  }

  return ParsePartitionTable ((UINT8 *)SmemBase + Item->Offset, 128);
}

/**
  Fallback: usable RAM ranges straight from the UEFI boot services memory map.
  EfiBootServicesData regions are ordinary RAM the firmware has not claimed.
**/
STATIC EFI_STATUS
DeriveFromBootServicesMap (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_MEMORY_DESCRIPTOR *Map     = NULL;
  UINTN                 MapSize  = 0;
  UINTN                 MapKey   = 0;
  UINTN                 DescSize = 0;
  UINT32                DescVer  = 0;
  UINT32                Count    = 0;
  RAM_PARTITION        *Parts    = NULL;

  Status = gBS->GetMemoryMap (&MapSize, &Map, &MapKey, &DescSize, &DescVer);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (UINTN Index = 0; Index < MapSize; Index += DescSize) {
    EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index);

    if ((Desc->Type != EfiBootServicesData) || (Desc->PhysicalStart < 0x80000000ULL)) {
      continue;
    }

    Count++;
  }

  if (Count == 0) {
    return EFI_NOT_FOUND;
  }

  Parts = (RAM_PARTITION *)AllocateZeroPool (sizeof (RAM_PARTITION) * Count);
  if (Parts == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Count = 0;
  for (UINTN Index = 0; Index < MapSize; Index += DescSize) {
    EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index);

    if ((Desc->Type != EfiBootServicesData) || (Desc->PhysicalStart < 0x80000000ULL)) {
      continue;
    }

    Parts[Count].StartAddress = Desc->PhysicalStart;
    Parts[Count].Size         = Desc->NumberOfPages * EFI_PAGE_SIZE;
    Parts[Count].State        = 1;
    Parts[Count].Flag         = 0;
    Count++;

    DEBUG ((
      DEBUG_INFO,
      "RamPartitionDxe: bs-map region[%u] base=%llx size=%llx\n",
      Count - 1,
      Parts[Count - 1].StartAddress,
      Parts[Count - 1].Size
      ));
  }

  mPartitions     = Parts;
  mPartitionCount = Count;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RamPartitionDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR SmemRegion;
  VOID                        *SmemBase;

  LocateMemoryRegionByName ("SMEM", &SmemRegion);
  if (!SmemRegion.Address) {
    DEBUG ((DEBUG_ERROR, "RamPartitionDxe: SMEM region not found in memory map!\n"));
  } else {
    SmemBase = (VOID *)(UINTN)SmemRegion.Address;

    DEBUG ((
      DEBUG_INFO,
      "RamPartitionDxe: SMEM at %llx len %llx, header: %08x %08x %08x %08x\n",
      SmemRegion.Address,
      SmemRegion.Length,
      *(UINT32 *)SmemBase,
      *(UINT32 *)((UINT8 *)SmemBase + 4),
      *(UINT32 *)((UINT8 *)SmemBase + 8),
      *(UINT32 *)((UINT8 *)SmemBase + 12)
      ));

    Status = TryGlobalPartition (SmemBase, SmemRegion.Length);
    if (EFI_ERROR (Status)) {
      Status = TryLegacyAllocTable (SmemBase, SmemRegion.Length);
    }
  }

  if ((mPartitionCount == 0) || (mPartitions == NULL)) {
    DEBUG ((DEBUG_WARN, "RamPartitionDxe: falling back to boot services memory map\n"));
    Status = DeriveFromBootServicesMap ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "RamPartitionDxe: no RAM partition information available: %r\n", Status));
      return Status;
    }
  }

  mRamPartitionProtocol.Revision               = EFI_RAMPARTITION_PROTOCOL_REVISION;
  mRamPartitionProtocol.GetRamPartitionVersion = RamPartitionGetVersion;
  mRamPartitionProtocol.GetHighestBankBit      = RamPartitionGetHighestBankBit;
  mRamPartitionProtocol.GetRamPartitions       = RamPartitionGetRamPartitions;
  mRamPartitionProtocol.GetMinPasrSize         = RamPartitionGetMinPasrSize;
  mRamPartitionProtocol.GetPreLoadedImageTable = RamPartitionGetPreLoadedImageTable;

  mHandle = NULL;
  Status  = gBS->InstallMultipleProtocolInterfaces (
                   &mHandle,
                   &gEfiRamPartitionProtocolGuid,
                   &mRamPartitionProtocol,
                   NULL
                   );
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_INFO, "RamPartitionDxe: installed RAM Partition protocol (%u partitions)\n", mPartitionCount));

  return Status;
}
