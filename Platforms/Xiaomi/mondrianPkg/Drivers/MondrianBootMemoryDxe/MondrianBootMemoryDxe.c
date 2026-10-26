/** @file
  Publish the Mondrian bootloader initrd's tagged FAT payload as a read-only disk.

  The x0/HOB handoff follows UniXNU's ShengBootMemoryDxe. Unlike its raw FAT
  assumption, scan a bounded initrd for a length/CRC-checked MONDRIANFAT1 envelope:
  Android v4 can concatenate the vendor ramdisk before the boot ramdisk.
  Copy the FAT to owned reserved pages before publishing it. This first-stage
  driver does not expand the firmware memory map or access physical disks.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Guid/EventGroup.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include "MondrianInitrd.h"

STATIC UINT8 *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA mMedia;
STATIC VOID *mEntryFdt;

// A bootloader-owned buffer need not be covered by the firmware's static map.
// AT reports a translation fault without dereferencing an unmapped pointer.
STATIC EFI_STATUS MakeReadable (UINT64 Address, UINTN Size) {
  UINT64 Base, End, Page, El, Par;
  if (Address < SIZE_1MB || Size == 0 || Size > MONDRIAN_FAT_MAX_INITRD ||
      Address >= LShiftU64 (1, 48) || Size > LShiftU64 (1, 48) - Address) { return EFI_INVALID_PARAMETER; }
  Base = Address & ~((UINT64)EFI_PAGE_SIZE - 1);
  End = ALIGN_VALUE (Address + Size, EFI_PAGE_SIZE);
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  for (Page = Base; Page < End; Page += EFI_PAGE_SIZE) {
    if (El == 4) { __asm__ volatile ("at s1e1r, %1; isb; mrs %0, par_el1" : "=r" (Par) : "r" (Page) : "memory"); }
    else if (El == 8) { __asm__ volatile ("at s1e2r, %1; isb; mrs %0, par_el1" : "=r" (Par) : "r" (Page) : "memory"); }
    else { return EFI_UNSUPPORTED; }
    if ((Par & 1) != 0) { return ArmSetMemoryAttributes (Base, End - Base, EFI_MEMORY_WB | EFI_MEMORY_XP, 0); }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Reset (EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN Extended) { return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Flush (EFI_BLOCK_IO_PROTOCOL *This) { return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI Write (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  return EFI_WRITE_PROTECTED;
}
STATIC EFI_STATUS EFIAPI Read (EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  if (MediaId != mMedia.MediaId) { return EFI_MEDIA_CHANGED; }
  if (Size == 0) { return EFI_SUCCESS; }
  if (Buffer == NULL) { return EFI_INVALID_PARAMETER; }
  if (Size % 512 != 0) { return EFI_BAD_BUFFER_SIZE; }
  if (Lba > mMedia.LastBlock || Size / 512 > mMedia.LastBlock + 1 - Lba) { return EFI_INVALID_PARAMETER; }
  CopyMem (Buffer, mRamBase + Lba * 512, Size);
  return EFI_SUCCESS;
}
STATIC EFI_BLOCK_IO_PROTOCOL mBlock = {
  EFI_BLOCK_IO_PROTOCOL_REVISION, &mMedia, Reset, Read, Write, Flush
};
#pragma pack(1)
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof (VENDOR_DEVICE_PATH), 0}},
   {0x7B27B767, 0x94A4, 0x4A47, {0x99, 0xDD, 0x28, 0x1F, 0x7C, 0x60, 0x4F, 0xAA}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}}
};
#pragma pack()

STATIC BOOLEAN InitrdRange (VOID *Fdt, UINT64 *Start, UINT64 *End) {
  INT32 Node, ASize, BSize;
  CONST VOID *A, *B;
  Node = FdtPathOffset (Fdt, "/chosen");
  if (Node < 0) { return FALSE; }
  A = FdtGetProp (Fdt, Node, "linux,initrd-start", &ASize);
  B = FdtGetProp (Fdt, Node, "linux,initrd-end", &BSize);
  if (A == NULL || B == NULL || (ASize != 4 && ASize != 8) || (BSize != 4 && BSize != 8)) { return FALSE; }
  *Start = ASize == 8 ? Fdt64ToCpu (ReadUnaligned64 (A)) : Fdt32ToCpu (ReadUnaligned32 (A));
  *End = BSize == 8 ? Fdt64ToCpu (ReadUnaligned64 (B)) : Fdt32ToCpu (ReadUnaligned32 (B));
  return *Start != 0 && *End > *Start && *End <= MAX_UINTN && *End - *Start <= MONDRIAN_FAT_MAX_INITRD;
}

STATIC EFI_STATUS Publish (UINT64 Start, UINT64 End) {
  CONST UINT8 *Bytes, *Header, *Fat;
  UINTN Span, Index, FatBytes;
  UINT64 Size;
  UINT32 Crc, Sectors;
  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS Copy;
  EFI_HANDLE Handle;
  Bytes = (CONST UINT8 *)(UINTN)Start;
  Span = (UINTN)(End - Start);
  if (Span < MONDRIAN_FAT_HEADER_BYTES + 512) { return EFI_NOT_FOUND; }
  for (Index = 0; Index <= Span - MONDRIAN_FAT_HEADER_BYTES - 512; ++Index) {
    Header = Bytes + Index;
    if (Header[0] != 'P' || CompareMem (Header, MONDRIAN_FAT_MAGIC, 8) != 0) { continue; }
    if (ReadUnaligned32 ((CONST UINT32 *)(Header + 8)) != 1 ||
        ReadUnaligned32 ((CONST UINT32 *)(Header + 12)) != MONDRIAN_FAT_HEADER_BYTES) { continue; }
    Size = ReadUnaligned64 ((CONST UINT64 *)(Header + 16));
    if (Size < 512 || Size % 512 != 0 || Size > Span - Index - MONDRIAN_FAT_HEADER_BYTES) { continue; }
    FatBytes = (UINTN)Size;
    Fat = Header + MONDRIAN_FAT_HEADER_BYTES;
    if (Fat[510] != 0x55 || Fat[511] != 0xAA || ReadUnaligned16 ((CONST UINT16 *)(Fat + 11)) != 512) { continue; }
    Sectors = ReadUnaligned16 ((CONST UINT16 *)(Fat + 19));
    if (Sectors == 0) { Sectors = ReadUnaligned32 ((CONST UINT32 *)(Fat + 32)); }
    if (Sectors != FatBytes / 512) { continue; }
    Status = gBS->CalculateCrc32 ((VOID *)Fat, FatBytes, &Crc);
    if (EFI_ERROR (Status) || Crc != ReadUnaligned32 ((CONST UINT32 *)(Header + 24))) { continue; }
    Copy = 0;
    Status = gBS->AllocatePages (AllocateAnyPages, EfiReservedMemoryType, EFI_SIZE_TO_PAGES (FatBytes), &Copy);
    if (EFI_ERROR (Status)) { return Status; }
    mRamBase = (UINT8 *)(UINTN)Copy;
    CopyMem (mRamBase, Fat, FatBytes);
    mMedia.MediaId = 1;
    mMedia.MediaPresent = TRUE;
    mMedia.ReadOnly = TRUE;
    mMedia.BlockSize = 512;
    mMedia.IoAlign = 1;
    mMedia.LastBlock = FatBytes / 512 - 1;
    Handle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (&Handle, &gEfiDevicePathProtocolGuid, &mPath,
                                                     &gEfiBlockIoProtocolGuid, &mBlock, NULL);
    if (EFI_ERROR (Status)) { gBS->FreePages (Copy, EFI_SIZE_TO_PAGES (FatBytes)); mRamBase = NULL; }
    // UniXNU connects controllers at application TPL before discovering config.
    DEBUG ((DEBUG_ERROR, "MONDRIANMEM: FAT at initrd+0x%lx, %lu bytes, %r\n", (UINT64)Index, Size, Status));
    return Status;
  }
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS ProcessFdt (VOID *Fdt, BOOLEAN Show) {
  UINT64 Start, End;
  EFI_STATUS Status;
  if (Show) { Print (L"\r\nMONDRIAN INITRD: FDT at 0x%lx\r\n", (UINT64)(UINTN)Fdt); }
  Status = MakeReadable ((UINT64)(UINTN)Fdt, 40);
  if (EFI_ERROR (Status)) { return Status; }
  if (FdtCheckHeader (Fdt) != 0 || FdtTotalSize (Fdt) > SIZE_2MB) { return EFI_COMPROMISED_DATA; }
  Status = MakeReadable ((UINT64)(UINTN)Fdt, FdtTotalSize (Fdt));
  if (EFI_ERROR (Status)) { return Status; }
  if (!InitrdRange (Fdt, &Start, &End)) { return EFI_NOT_FOUND; }
  if (Show) { Print (L"MONDRIAN INITRD: source 0x%lx-0x%lx\r\n", Start, End); }
  Status = MakeReadable (Start, (UINTN)(End - Start));
  if (EFI_ERROR (Status)) { return Status; }
  return Publish (Start, End);
}

STATIC VOID EFIAPI ReadyToBoot (EFI_EVENT Event, VOID *Context) {
  EFI_STATUS Status;
  gBS->CloseEvent (Event);
  Status = ProcessFdt (mEntryFdt, TRUE);
  Print (L"MONDRIAN INITRD: %r\r\n", Status);
  if (EFI_ERROR (Status)) { gBS->Stall (20 * 1000000); }
}

EFI_STATUS EFIAPI MondrianBootMemoryEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_HOB_GUID_TYPE *Hob;
  VOID *Fdt;
  UINT64 Address;
  EFI_EVENT Event;
  EFI_STATUS Status;
  Fdt = NULL;
  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob != NULL && GET_GUID_HOB_DATA_SIZE (Hob) >= sizeof (Address)) {
    CopyMem (&Address, GET_GUID_HOB_DATA (Hob), sizeof (Address));
    if (Address != 0 && Address <= MAX_UINTN && (Address & 3) == 0) {
      mEntryFdt = (VOID *)(UINTN)Address;
      return gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &Event);
    }
  }
  // Standard configuration-table fallback also permits testing this exact DXE in QEMU.
  if (Fdt == NULL) { EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt); }
  Status = Fdt == NULL ? EFI_NOT_FOUND : ProcessFdt (Fdt, FALSE);
  DEBUG ((DEBUG_ERROR, "MONDRIANMEM: initrd: %r\n", Status));
  // A missing diagnostic payload must not break normal firmware boot.
  return EFI_SUCCESS;
}
