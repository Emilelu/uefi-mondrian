// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include "../../Drivers/MondrianBootMemoryDxe/MondrianInitrd.h"

EFI_STATUS EFIAPI MondrianPayloadProbeEntry (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_FILE_PROTOCOL *Root, *File;
  EFI_STATUS Status;
  CHAR8 Marker[64];
  UINTN Size, Index;
  EFI_EVENT Timer;
  EFI_EVENT Events[2];
  if (gST->ConOut != NULL) { gST->ConOut->ClearScreen (gST->ConOut); }
  Print (L"MONDRIAN PAYLOAD PROBE\r\n");
  Status = gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->HandleProtocol (Loaded->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->HandleProtocol (Loaded->DeviceHandle, &gEfiBlockIoProtocolGuid, (VOID **)&Block);
  if (EFI_ERROR (Status) || !Block->Media->ReadOnly) { Print (L"MONDRIAN PAYLOAD FAIL: disk is not read-only\r\n"); return EFI_DEVICE_ERROR; }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Root->Open (Root, &File, L"\\MONDRIAN-PROBE.TXT", EFI_FILE_MODE_READ, 0);
  if (!EFI_ERROR (Status)) {
    Size = sizeof (Marker);
    Status = File->Read (File, &Size, Marker);
    File->Close (File);
    if (!EFI_ERROR (Status) && (Size != sizeof (MONDRIAN_PAYLOAD_MARKER) - 1 ||
        CompareMem (Marker, MONDRIAN_PAYLOAD_MARKER, Size) != 0)) { Status = EFI_COMPROMISED_DATA; }
  }
  Root->Close (Root);
  if (EFI_ERROR (Status)) { Print (L"MONDRIAN PAYLOAD FAIL: marker %r\r\n", Status); return Status; }
  Print (L"MONDRIAN PAYLOAD PASS\r\nRead-only initrd FAT loaded this ARM64 EFI application.\r\n");
  Print (L"Disk: %lu bytes\r\nNo XNU kernel or iPadOS has been started.\r\n", (Block->Media->LastBlock + 1) * 512);
  Print (L"Press a key to return; automatic return in 120 seconds.\r\n");
  if (gST->ConIn != NULL && !EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &Timer))) {
    gBS->SetTimer (Timer, TimerRelative, 120 * 10000000ULL);
    Events[0] = gST->ConIn->WaitForKey;
    Events[1] = Timer;
    gBS->WaitForEvent (2, Events, &Index);
    gBS->CloseEvent (Timer);
  }
  return EFI_SUCCESS;
}
