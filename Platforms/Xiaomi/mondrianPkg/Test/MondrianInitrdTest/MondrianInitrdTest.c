/** SPDX-License-Identifier: BSD-2-Clause-Patent
  QEMU integration harness: use the actual Mondrian DXE and FAT driver, not a mock.
  Supply a deliberately unaligned vendor prefix before the tagged initrd.
**/
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include "../../Drivers/MondrianBootMemoryDxe/MondrianInitrd.h"

STATIC EFI_STATUS ReadFile (EFI_FILE_PROTOCOL *Root, CHAR16 *Name, VOID *Bytes, UINTN *Size) {
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status = Root->Open (Root, &File, Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Status = File->Read (File, Size, Bytes);
  File->Close (File);
  return Status;
}

EFI_STATUS EFIAPI MondrianInitrdTestEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *Table) {
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root;
  EFI_STATUS Status;
  UINT8 *Ramdisk, *Driver, *Fdt, Sector[512];
  UINTN Size, DriverSize, Count, Index;
  EFI_HANDLE DriverHandle, *Handles;
  EFI_BLOCK_IO_PROTOCOL *Block;
  UINT64 Cell;
  INT32 Chosen;
  BOOLEAN Found = FALSE;
  Status = gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { goto Fail; }
  Status = gBS->HandleProtocol (Loaded->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) { goto Fail; }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) { goto Fail; }
  Ramdisk = AllocatePages (EFI_SIZE_TO_PAGES (SIZE_16MB));
  Driver = AllocatePool (SIZE_1MB);
  Fdt = AllocatePages (EFI_SIZE_TO_PAGES (SIZE_64KB));
  if (Ramdisk == NULL || Driver == NULL || Fdt == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Fail; }
  ZeroMem (Ramdisk, SIZE_16MB);
  Size = SIZE_16MB - 37;
  Status = ReadFile (Root, L"\\payload.ramdisk", Ramdisk + 37, &Size);
  if (EFI_ERROR (Status)) { goto Fail; }
  // Corrupt the envelope CRC on the negative run; its presence must not publish a disk.
  {
    EFI_FILE_PROTOCOL *Flag;
    if (!EFI_ERROR (Root->Open (Root, &Flag, L"\\negative.flag", EFI_FILE_MODE_READ, 0))) {
      Flag->Close (Flag);
      Ramdisk[37 + 24] ^= 1;
    }
  }
  DriverSize = SIZE_1MB;
  Status = ReadFile (Root, L"\\MondrianBootMemoryDxe.efi", Driver, &DriverSize);
  if (EFI_ERROR (Status)) { goto Fail; }
  FdtCreateEmptyTree (Fdt, SIZE_64KB);
  Chosen = FdtAddSubnode (Fdt, 0, "chosen");
  Cell = CpuToFdt64 ((UINT64)(UINTN)Ramdisk);
  FdtSetProp (Fdt, Chosen, "linux,initrd-start", &Cell, sizeof (Cell));
  Cell = CpuToFdt64 ((UINT64)(UINTN)Ramdisk + Size + 37);
  FdtSetProp (Fdt, Chosen, "linux,initrd-end", &Cell, sizeof (Cell));
  gBS->InstallConfigurationTable (&gFdtTableGuid, Fdt);
  Status = gBS->LoadImage (FALSE, Image, NULL, Driver, DriverSize, &DriverHandle);
  if (EFI_ERROR (Status)) { goto Fail; }
  {
    EFI_FILE_PROTOCOL *Flag;
    if (!EFI_ERROR (Root->Open (Root, &Flag, L"\\unmapped.flag", EFI_FILE_MODE_READ, 0))) {
      Flag->Close (Flag);
      Status = ArmSetMemoryAttributes ((UINTN)Fdt, SIZE_64KB, EFI_MEMORY_RP, EFI_MEMORY_RP);
      if (EFI_ERROR (Status)) { goto Fail; }
      Status = ArmSetMemoryAttributes ((UINTN)Ramdisk, SIZE_16MB, EFI_MEMORY_RP, EFI_MEMORY_RP);
      if (EFI_ERROR (Status)) { goto Fail; }
    }
  }
  Status = gBS->StartImage (DriverHandle, NULL, NULL);
  if (EFI_ERROR (Status)) { goto Fail; }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) { goto Fail; }
  for (Index = 0; Index < Count; ++Index) {
    gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Block);
    if (Block->Media->ReadOnly && Block->Media->BlockSize == 512 && Block->Media->LastBlock == 16383) {
      Status = Block->ReadBlocks (Block, Block->Media->MediaId, 0, sizeof (Sector), Sector);
      if (EFI_ERROR (Status) || Sector[510] != 0x55 || Sector[511] != 0xAA) { goto Fail; }
      if (Block->WriteBlocks (Block, Block->Media->MediaId, 0, sizeof (Sector), Sector) != EFI_WRITE_PROTECTED ||
          Block->ReadBlocks (Block, Block->Media->MediaId + 1, 0, sizeof (Sector), Sector) != EFI_MEDIA_CHANGED ||
          Block->ReadBlocks (Block, Block->Media->MediaId, 16384, sizeof (Sector), Sector) != EFI_INVALID_PARAMETER ||
          Block->ReadBlocks (Block, Block->Media->MediaId, 0, 511, Sector) != EFI_BAD_BUFFER_SIZE) {
        Status = EFI_DEVICE_ERROR; goto Fail;
      }
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
      Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
      if (EFI_ERROR (Status)) { goto Fail; }
      Status = Fs->OpenVolume (Fs, &Root);
      if (EFI_ERROR (Status)) { goto Fail; }
      Size = sizeof (Sector);
      Status = ReadFile (Root, L"\\MONDRIAN-PROBE.TXT", Sector, &Size);
      if (EFI_ERROR (Status) || Size != sizeof (MONDRIAN_PAYLOAD_MARKER) - 1 || CompareMem (Sector, MONDRIAN_PAYLOAD_MARKER, Size) != 0) { Status = EFI_COMPROMISED_DATA; goto Fail; }
      Found = TRUE;
    }
  }
  {
    EFI_FILE_PROTOCOL *Flag;
    BOOLEAN Negative = !EFI_ERROR (Root->Open (Root, &Flag, L"\\negative.flag", EFI_FILE_MODE_READ, 0));
    if (Negative) { Flag->Close (Flag); }
    if (Found == !Negative) {
      Print (L"MONDRIAN-INITRD-TEST: %s PASS\r\n", Negative ? L"negative CRC refusal" : L"FAT mount, marker, read-only and bounds");
      gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    }
  }
  Status = EFI_NOT_FOUND;
Fail:
  Print (L"MONDRIAN-INITRD-TEST: FAIL %r\r\n", Status);
  gRT->ResetSystem (EfiResetShutdown, EFI_ABORTED, 0, NULL);
  return Status;
}
