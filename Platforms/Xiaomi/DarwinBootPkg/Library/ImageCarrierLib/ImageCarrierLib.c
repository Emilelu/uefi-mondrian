/** @file
  Load the modem firmware carrier into its carveout before XNU starts.

  Contract and rationale: k60-cellular docs/research/cellular/
  15-FIRMWARE-CARRIER-LOADER.md.  The window is reserved by the platform
  memory map and is not a Boot Services resource, so nothing is allocated
  for it and nothing is released on failure.

  This build is the read-only half of the library: it validates the table,
  checks that the window is still reserved, opens EFI\UniXNU\MODEM and
  proves that the 27 carrier files are present with the sizes the table
  expects.  The copy into 0x8bc00000 and the K60ModemImage property are not
  implemented yet; they return EFI_UNSUPPORTED so that a call site added by
  mistake refuses the boot instead of starting a modem with stale bytes.

  Cache note: MemoryMapLib.c registers the window as UNCACHED_UNBUFFERED_XN,
  so writes need no maintenance.  If that ever becomes WRITE_BACK, the copy
  must clean the range (DarwinBoot.c FlushRange or ArmCleanDataCacheToPoU)
  before the modem is handed control, otherwise it fetches stale bytes.
**/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/ImageCarrierLib.h>
#include <K60ModemImageTable.h>

//
// Display Reserved, the first descriptor after the ABL staging area
// (MemoryMapLib.c:52).  Its start is where ABL stopped writing.
//
#define ABL_END_ADDRESS  0xB8000000ULL

STATIC CONST CHAR16  mModemDir[] = L"\\EFI\\UniXNU\\MODEM";

//
// The lowest carrier file, used as the sentinel that separates a missing
// directory from an empty one.
//
#define CARRIER_SENTINEL  "modem.b01"

/**
  Copy one ASCII table name into a CHAR16 buffer.

  @param[out]  Out   Destination, at least 20 characters.
  @param[in]   Name  Table name, "modem.bNN".
**/
STATIC
VOID
CarrierName (
  OUT CHAR16       *Out,
  IN  CONST CHAR8  *Name
  )
{
  AsciiStrToUnicodeStrS (Name, Out, 20);
}

/**
  Reject a table that cannot describe this window.

  Five rules from 15-FIRMWARE-CARRIER-LOADER.md 2.4 step 9: row count, 4 KiB
  alignment, containment, name present exactly when bytes are present, and
  the last row ending on the window edge.

  @retval EFI_SUCCESS           The table is self-consistent.
  @retval EFI_COMPROMISED_DATA  A rule failed; the caller prints the detail.
**/
STATIC
EFI_STATUS
CarrierCheckTable (
  VOID
  )
{
  UINTN   Index;
  UINT64  Start;
  UINT64  End;
  UINT64  Limit;

  if (MODEM_IMAGE_SEGMENTS != ARRAY_SIZE (k60_modem_fw_segments)) {
    return EFI_COMPROMISED_DATA;
  }

  Limit = MODEM_IMAGE_BASE + MODEM_IMAGE_WINDOW;
  End   = MODEM_IMAGE_BASE;
  for (Index = 0; Index < MODEM_IMAGE_SEGMENTS; Index++) {
    Start = k60_modem_fw_segments[Index].paddr;
    End   = Start + k60_modem_fw_segments[Index].bytes + k60_modem_fw_segments[Index].bss;
    if ((Start & (SIZE_4KB - 1)) != 0) {
      Print (L"modem-image: row %d paddr 0x%lx unaligned\n", (UINTN)Index, Start);
      return EFI_COMPROMISED_DATA;
    }
    if ((Start < MODEM_IMAGE_BASE) || (End > Limit)) {
      Print (L"modem-image: row %d outside 0x8bc00000..0x9ee00000\n", (UINTN)Index);
      return EFI_COMPROMISED_DATA;
    }
    if ((k60_modem_fw_segments[Index].name == NULL) != (k60_modem_fw_segments[Index].bytes == 0)) {
      Print (L"modem-image: table inconsistent\n");
      return EFI_COMPROMISED_DATA;
    }
  }

  if (End != Limit) {
    Print (L"modem-image: table inconsistent\n");
    return EFI_COMPROMISED_DATA;
  }

  return EFI_SUCCESS;
}

/**
  Confirm the window is still covered by one reserved descriptor and note
  where the ABL staging area ends.

  The array in MemoryMapLib.c is written at build time; GetMemoryMap reports
  what the HOB processing actually produced.  ABL Staging (MemoryMapLib.c:39)
  is the precedent for trusting the map over the array.

  @param[in,out]  Carrier  Base and Span are inputs; AblEnd is the output.

  @retval EFI_SUCCESS         The window is reserved; AblEnd is filled or 0.
  @retval EFI_DEVICE_ERROR    The window is free or absent from the map.
  @retval EFI_OUT_OF_RESOURCES  The descriptor buffer could not be allocated.
**/
STATIC
EFI_STATUS
CarrierScanMap (
  IN OUT IMAGE_CARRIER  *Carrier
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Entry;
  UINTN                  Capacity;
  UINTN                  Size;
  UINTN                  Key;
  UINTN                  Stride;
  UINTN                  Offset;
  UINTN                  Attempt;
  UINT32                 Version;
  UINT64                 Base;
  UINT64                 Limit;
  UINT64                 Start;
  UINT64                 End;
  BOOLEAN                Covered;

  Map      = NULL;
  Capacity = 0;
  Size     = 0;
  Base     = Carrier->Base;
  Limit    = Carrier->Base + Carrier->Span;
  Covered  = FALSE;
  Carrier->AblEnd = 0;

  for (Attempt = 0; Attempt < 3; Attempt++) {
    Size   = Capacity;
    Status = gBS->GetMemoryMap (&Size, Map, &Key, &Stride, &Version);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      break;
    }
    if (Map != NULL) {
      FreePool (Map);
    }
    Capacity = Size + 32 * sizeof (EFI_MEMORY_DESCRIPTOR);
    Map      = AllocatePool (Capacity);
    if (Map == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
  }
  if (EFI_ERROR (Status)) {
    if (Map != NULL) {
      FreePool (Map);
    }
    return EFI_DEVICE_ERROR;
  }

  for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
    Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    if (Entry->NumberOfPages >= (MAX_UINT64 - Entry->PhysicalStart) / EFI_PAGE_SIZE) {
      continue;
    }
    Start = Entry->PhysicalStart;
    End   = Start + EFI_PAGES_TO_SIZE (Entry->NumberOfPages);
    if ((Start <= Base) && (End >= Limit)) {
      if (Entry->Type == EfiConventionalMemory) {
        Print (L"modem-image: window 0x8bc00000..0x9ee00000 is not reserved, refusing\n");
        FreePool (Map);
        return EFI_DEVICE_ERROR;
      }
      Covered = TRUE;
    }
    if (Start == ABL_END_ADDRESS) {
      Carrier->AblEnd = Start;
    }
  }

  FreePool (Map);
  return Covered ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/**
  Read the size of one carrier file and reject directories.

  @param[in]   Dir   Open EFI\UniXNU\MODEM directory.
  @param[in]   Name  ASCII table name, converted to CHAR16 here.
  @param[out]  Size  File size in bytes.

  @retval EFI_SUCCESS        The file exists and is a regular file.
  @retval EFI_NOT_FOUND      No such file in the directory.
  @retval EFI_DEVICE_ERROR   GetInfo failed twice.
**/
STATIC
EFI_STATUS
CarrierFileSize (
  IN  EFI_FILE_PROTOCOL  *Dir,
  IN  CONST CHAR8        *Name,
  OUT UINT64             *Size
  )
{
  EFI_STATUS     Status;
  EFI_FILE_INFO  *Info;
  EFI_FILE_PROTOCOL  *File;
  CHAR16         Path[20];
  UINTN          InfoSize;

  CarrierName (Path, Name);
  Status = Dir->Open (Dir, &File, Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    File->Close (File);
    return EFI_DEVICE_ERROR;
  }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (!EFI_ERROR (Status)) {
    if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
      Status = EFI_NOT_FOUND;
    } else {
      *Size = Info->FileSize;
    }
  }
  FreePool (Info);
  File->Close (File);
  return Status;
}

/**
  Load the carrier named by the generated firmware table.

  @param[in]      Root            Volume root, opened by the caller; the
                                  library never closes it.
  @param[in,out]  Tree            Parsed ADT image in pool memory.  The
                                  16-byte K60ModemImage placeholder is
                                  written here, not into the window copy.
  @param[in]      TreeSize        Size of Tree in bytes.
  @param[out]     Carrier         Filled on every return path.
  @param[out]     PropertyOffset  Offset of the placeholder inside Tree.

  @retval EFI_SUCCESS           The payload carries no EFI\UniXNU\MODEM
                                directory (Files == 0).
  @retval EFI_NOT_FOUND         The directory exists but modem.b01 is gone.
  @retval EFI_COMPROMISED_DATA  Size, table or range mismatch.
  @retval EFI_DEVICE_ERROR      The window is not reserved in the map.
  @retval EFI_OUT_OF_RESOURCES  A scratch allocation failed.
  @retval EFI_BAD_BUFFER_SIZE   Tree is too small for the placeholder.
  @retval EFI_UNSUPPORTED       The copy and the property are not in this
                                build yet.
**/
EFI_STATUS
EFIAPI
ImageCarrierLoad (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN OUT VOID            *Tree,
  IN  UINTN               TreeSize,
  OUT IMAGE_CARRIER      *Carrier,
  OUT UINTN              *PropertyOffset
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Dir;
  UINTN              Index;
  UINT64             Size;
  UINT64             Copied;
  UINT64             Zeroed;
  UINT64             Gaps;
  UINT32             Files;

  if ((Root == NULL) || (Tree == NULL) || (Carrier == NULL) || (PropertyOffset == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Carrier, sizeof (*Carrier));
  Carrier->Base     = MODEM_IMAGE_BASE;
  Carrier->Span     = MODEM_IMAGE_WINDOW;
  Carrier->Segments = MODEM_IMAGE_SEGMENTS;
  *PropertyOffset   = 0;
  if (TreeSize < sizeof (UINT64) * 2) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = CarrierCheckTable ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CarrierScanMap (Carrier);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  //
  // D3 lines 2 and 3 belong to whoever holds the values: the window constant
  // and AblEnd are only known here, so the library prints them.  Line 1
  // (the chosen DTB address) stays in DarwinBoot.c, which is the only place
  // that knows TreeAddress.  Keep one owner per line: the acceptance gate
  // greps each literal and requires exactly one hit.
  //
  Print (L"modem-image: window 0x8bc00000..0x9ee00000\n");
  Print (L"modem-image: abl staging ends at 0x%016lx\n", Carrier->AblEnd);

  Status = Root->Open (Root, &Dir, (CHAR16 *)mModemDir, EFI_FILE_MODE_READ, 0);
  if (Status == EFI_NOT_FOUND) {
    Print (L"modem-image: no carrier in payload, skipped\n");
    Carrier->Files = 0;
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    Print (L"modem-image: open EFI\\UniXNU\\MODEM failed: %r\n", Status);
    return EFI_DEVICE_ERROR;
  }

  //
  // The sentinel separates "the directory is not there" from "the directory
  // is there but empty", which must not be mistaken for a legal skip.
  //
  Status = CarrierFileSize (Dir, CARRIER_SENTINEL, &Size);
  if (Status == EFI_NOT_FOUND) {
    Print (L"modem-image: modem.b01 missing\n");
    Dir->Close (Dir);
    return EFI_NOT_FOUND;
  }
  if (EFI_ERROR (Status)) {
    Print (L"modem-image: read modem.b01 failed: %r\n", Status);
    Dir->Close (Dir);
    return EFI_DEVICE_ERROR;
  }

  Files  = 0;
  Copied = 0;
  Zeroed = 0;
  for (Index = 0; Index < MODEM_IMAGE_SEGMENTS; Index++) {
    Zeroed += k60_modem_fw_segments[Index].bss;
    if (k60_modem_fw_segments[Index].bytes == 0) {
      continue;
    }
    Status = CarrierFileSize (Dir, k60_modem_fw_segments[Index].name, &Size);
    if (Status == EFI_NOT_FOUND) {
      Print (L"modem-image: %a missing\n", k60_modem_fw_segments[Index].name);
      Dir->Close (Dir);
      return EFI_NOT_FOUND;
    }
    if (EFI_ERROR (Status)) {
      Print (L"modem-image: read %a failed: %r\n", k60_modem_fw_segments[Index].name, Status);
      Dir->Close (Dir);
      return EFI_DEVICE_ERROR;
    }
    if (Size != k60_modem_fw_segments[Index].bytes) {
      Print (L"modem-image: %a size %d, table says %d\n", k60_modem_fw_segments[Index].name,
             (UINTN)Size, (UINTN)k60_modem_fw_segments[Index].bytes);
      Dir->Close (Dir);
      return EFI_COMPROMISED_DATA;
    }
    Copied += k60_modem_fw_segments[Index].bytes;
    Files++;
  }
  Dir->Close (Dir);

  Gaps           = MODEM_IMAGE_WINDOW - Copied - Zeroed;
  Carrier->Files  = Files;
  Carrier->Copied = (UINT32)Copied;
  Carrier->Zeroed = (UINT32)Zeroed;
  Carrier->Gaps   = (UINT32)Gaps;
  Print (L"modem-image: plan ok, %d files, copied %d, zeroed %d, gaps %d\n",
         (UINTN)Files, (UINTN)Copied, (UINTN)Zeroed, (UINTN)Gaps);

  //
  // Copy and property publication land in the next delivery.  Returning
  // failure here keeps a premature call site from booting a modem against
  // an empty window.
  //
  Print (L"modem-image: copy not implemented in this build\n");
  return EFI_UNSUPPORTED;
}
