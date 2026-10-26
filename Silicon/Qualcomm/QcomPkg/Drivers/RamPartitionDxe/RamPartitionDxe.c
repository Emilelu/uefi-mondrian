/** @file
  RamPartitionDxe.c — Open-source provider of the EFI_RAMPARTITION_PROTOCOL.

  Reads the usable RAM partition table from SMEM item 15
  (SMEM_USABLE_RAM_PARTITION_TABLE, Qualcomm standard layout) and installs
  the protocol that RamManagerLib (linked into vendor drivers like EnvDxe,
  SmemDxe, DisplayDxe, NT_DeviceInfoDxe) locates in its constructor.

  Without this driver every vendor driver linking RamManagerLib fails its
  library constructor with EFI_NOT_FOUND and stalls the DXE dispatch.

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

//
// Qualcomm SMEM constants
//
#define SMEM_USABLE_RAM_PARTITION_TABLE  15
#define SMEM_ALLOC_TABLE_MAX_ITEMS       512
#define SMEM_ALLOC_TABLE_OFFSET          0x1000  // alloc table sits after the 4K header page

//
// SMEM allocation table entry (16 bytes each)
//
typedef struct {
  UINT32 Offset;       // offset of item data from SMEM base
  UINT32 Size;         // item size in bytes
  UINT8  Attr;
  UINT8  Reserved[3];
  UINT32 Crc32;
} SMEM_ALLOC_ENTRY;

//
// RAM partition table (SMEM item 15 payload), Qualcomm standard layout
//
typedef struct {
  UINT64 StartAddress;
  UINT32 Size;
  UINT32 State;
  UINT32 Flag;
  UINT32 Reserved1;
  UINT32 Reserved2;
} RAM_PARTITION;

STATIC RAM_PARTITION               *mPartitions    = NULL;
STATIC UINT32                       mPartitionCount = 0;
STATIC EFI_RAMPARTITION_PROTOCOL    mRamPartitionProtocol;
STATIC EFI_HANDLE                   mHandle = NULL;

EFI_STATUS
EFIAPI
RamPartitionGetVersion (
  IN  EFI_RAMPARTITION_PROTOCOL *This,
  OUT UINT32                    *MajorVersion,
  OUT UINT32                    *MinorVersion)
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
  OUT UINT32                    *HighestBankBit)
{
  if (HighestBankBit == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // SM8475 (K60): 12 GiB LPDDR5 -> highest bank bit 34
  *HighestBankBit = 34;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
RamPartitionGetMinPasrSize (
  IN  EFI_RAMPARTITION_PROTOCOL *This,
  OUT UINT32                    *MinPasrSize)
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
  IN  OUT UINT32                *NumPreloadedImg)
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
  IN  OUT UINT32                *NumPartition)
{
  UINT32 Count;

  if (NumPartition == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (RamPartitions == NULL) {
    // Caller probes the number of partitions first.
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
  Locate the SMEM region from the memory map and parse item 15.
**/
STATIC EFI_STATUS
ParseSmemRamPartitionTable (
  VOID
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR SmemRegion;
  VOID                         *SmemBase;
  SMEM_ALLOC_ENTRY             *AllocTable;
  SMEM_ALLOC_ENTRY             *Item;
  VOID                         *TableData;
  UINT32                        Len;
  RAM_PARTITION                *Src;

  // Locate SMEM region (renamed to "SMEM" in our memory map)
  LocateMemoryRegionByName ("SMEM", &SmemRegion);
  if (!SmemRegion.Address) {
    DEBUG ((DEBUG_ERROR, "RamPartitionDxe: SMEM region not found in memory map!\n"));
    return EFI_NOT_FOUND;
  }

  SmemBase   = (VOID *)(UINTN)SmemRegion.Address;
  AllocTable = (SMEM_ALLOC_ENTRY *)((UINT8 *)SmemBase + SMEM_ALLOC_TABLE_OFFSET);

  // Bounds check: the alloc table must fit inside the SMEM region
  if (SMEM_ALLOC_TABLE_OFFSET + (sizeof (SMEM_ALLOC_ENTRY) * SMEM_ALLOC_TABLE_MAX_ITEMS) > SmemRegion.Length) {
    DEBUG ((DEBUG_ERROR, "RamPartitionDxe: SMEM region too small for alloc table!\n"));
    return EFI_NOT_FOUND;
  }

  Item = &AllocTable[SMEM_USABLE_RAM_PARTITION_TABLE];

  if ((Item->Size == 0) || (Item->Offset == 0xFFFFFFFF)) {
    DEBUG ((DEBUG_ERROR, "RamPartitionDxe: SMEM item 15 is not allocated!\n"));
    return EFI_NOT_FOUND;
  }

  if (Item->Offset + Item->Size > SmemRegion.Length) {
    DEBUG ((DEBUG_ERROR, "RamPartitionDxe: SMEM item 15 out of bounds!\n"));
    return EFI_NOT_FOUND;
  }

  TableData = (UINT8 *)SmemBase + Item->Offset;

  // ram_partition_table: u64 start, u32 size, u32 state, u32 flag, u32 rsvd1, u32 rsvd2
  // layout: u32 version @8, u32 len @12, partitions @16
  Len = *(UINT32 *)((UINT8 *)TableData + 12);
  if ((Len == 0) || (Len > 128)) {
    DEBUG ((DEBUG_ERROR, "RamPartitionDxe: implausible partition count %u!\n", Len));
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

EFI_STATUS
EFIAPI
RamPartitionDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  Status = ParseSmemRamPartitionTable ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "RamPartitionDxe: failed to parse SMEM RAM partition table: %r\n", Status));
    return Status;
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
