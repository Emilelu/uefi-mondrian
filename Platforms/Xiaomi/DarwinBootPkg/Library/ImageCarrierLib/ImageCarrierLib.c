/** @file
  Load the modem firmware carrier into its carveout before XNU starts.

  Contract and rationale: k60-cellular docs/research/cellular/
  15-FIRMWARE-CARRIER-LOADER.md.  The window is reserved by the platform
  memory map and is not a Boot Services resource, so nothing is allocated
  for it and nothing is released on failure.

  Route D: the payload carries 27 LZMA files named modem.bNN.lzma.  For each
  one the library reads the 13-byte LZMA-alone header first and compares the
  declared uncompressed size with the generated table; only a header that
  agrees causes the body to be read into pool.  The decoded bytes go straight
  to their place in the carveout: LzmaDecode takes the destination as its
  dictionary window (Sdk/C/LzmaDec.c:1229-1230), so a fixed physical address
  is a legal destination and no staging buffer is needed.

  The K60ModemImage property is written by the caller, and only after this
  function returned success, so a half-filled window never advertises itself
  (15 section 1.8.4).  The 16-byte placeholder itself is created at build
  time by k60-cellular scripts/make-pong-adt.py.

  Cache note: MemoryMapLib.c registers the window as UNCACHED_UNBUFFERED_XN,
  so writes need no maintenance.  If that ever becomes WRITE_BACK, the copy
  must clean the range (DarwinBoot.c FlushRange or ArmCleanDataCacheToPoU)
  before the modem is handed control, otherwise it fetches stale bytes.
**/

#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleFileSystem.h>
#include "ImageCarrierLib.h"
#include "K60ModemImageTable.h"
#include "K60ModemMetadata.h"
#include "LzmaDec.h"

//
// Display Reserved, the first descriptor after the ABL staging area
// (MemoryMapLib.c:52).  Its start is where ABL stopped writing.
//
#define ABL_END_ADDRESS  0xB8000000ULL

//
// The PAS metadata page: the last 64 KiB of ABL staging, above the boot image
// and below Display Reserved.  The blob is small (modem.mdt's ELF header plus
// its hash section), so it is embedded in this library rather than carried as
// a 28th payload file.  The page is write-back memory (MemoryMapLib.c:51),
// hence the explicit clean before the secure monitor reads it.  The kernel
// uses the same address (K60ModemBoot.c K60_MODEM_METADATA);
// scripts/check-modem-metadata.py keeps the two definitions equal.
//
#define ABL_STAGING_BASE             0xA8300000ULL
#define K60_MODEM_METADATA_ADDRESS   0xB7FF0000ULL

STATIC CONST CHAR16  mModemDir[] = L"\\EFI\\UniXNU\\MODEM";

//
// The lowest carrier file, used as the sentinel that separates a missing
// directory from one that exists without a carrier in it.
//
#define CARRIER_SENTINEL  "modem.b01"

//
// LZMA-alone header: five property bytes plus the 8-byte little-endian
// uncompressed size (LzmaDecompress.c:71).  The format carries no checksum,
// which is why the declared size is compared with the table before decoding
// and why a stream that ends early can only be caught by its return value.
//
#define CARRIER_HEADER_SIZE  13

//
// The decoder allocates its probability tables through ISzAlloc; EDK2 sizes
// that scratch buffer at 64 KiB (LzmaDecompress.c:14) and the tables need
// 16,256 bytes for the properties these files carry.
//
#define CARRIER_SCRATCH_SIZE  SIZE_64KB

/**
  Build the CHAR16 path of one carrier file: table name plus ".lzma".

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
  UnicodeSPrint (Out, 20 * sizeof (CHAR16), L"%a.lzma", Name);
}

/**
  Reject a table that cannot describe this window.

  Five rules from 15-FIRMWARE-CARRIER-LOADER.md 2.4 step 9: row count, 4 KiB
  alignment, containment, name present exactly when bytes are present, and
  the last row ending on the window edge.

  @retval EFI_SUCCESS           The table is self-consistent.
  @retval EFI_COMPROMISED_DATA  A rule failed; the detail was printed.
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
  Confirm the window is still covered by a reserved descriptor and note
  where the ABL staging area ends.

  The array in MemoryMapLib.c is written at build time; GetMemoryMap reports
  what the HOB processing actually produced, and ABL Staging (MemoryMapLib.c:39)
  is the precedent for trusting the map over the array.

  @param[in,out]  Carrier  Base and Span are inputs; AblEnd is the output.

  @retval EFI_SUCCESS           The window is reserved; AblEnd is filled or 0.
  @retval EFI_DEVICE_ERROR      The window is free or absent from the map.
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

  Map             = NULL;
  Capacity        = 0;
  Size            = 0;
  Base            = Carrier->Base;
  Limit           = Carrier->Base + Carrier->Span;
  Covered         = FALSE;
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
    if ((Start <= ABL_END_ADDRESS) && (End >= ABL_END_ADDRESS)) {
      Carrier->AblEnd = ABL_END_ADDRESS;
    }
  }

  FreePool (Map);
  return Covered ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

/**
  Open one carrier file and report its size.

  @param[in]   Dir   Open EFI\\UniXNU\\MODEM directory.
  @param[in]   Name  ASCII table name; ".lzma" is appended here.
  @param[out]  File  Open handle, closed by the caller.
  @param[out]  Size  File size in bytes.

  @retval EFI_SUCCESS           The file exists and is not a directory.
  @retval EFI_NOT_FOUND         No such file, or it is a directory.
  @retval EFI_DEVICE_ERROR      GetInfo failed.
  @retval EFI_OUT_OF_RESOURCES  The info buffer could not be allocated.
**/
STATIC
EFI_STATUS
CarrierOpenFile (
  IN  EFI_FILE_PROTOCOL  *Dir,
  IN  CONST CHAR8        *Name,
  OUT EFI_FILE_PROTOCOL  **File,
  OUT UINT64             *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_INFO      *Info;
  CHAR16             Path[20];
  UINTN              InfoSize;

  CarrierName (Path, Name);
  Status = Dir->Open (Dir, File, Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status   = (*File)->GetInfo (*File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    (*File)->Close (*File);
    return EFI_DEVICE_ERROR;
  }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) {
    (*File)->Close (*File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = (*File)->GetInfo (*File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (!EFI_ERROR (Status)) {
    if ((Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
      Status = EFI_NOT_FOUND;
    } else {
      *Size = Info->FileSize;
    }
  } else {
    Status = EFI_DEVICE_ERROR;
  }
  FreePool (Info);
  if (EFI_ERROR (Status)) {
    (*File)->Close (*File);
  }
  return Status;
}

/**
  Decode the little-endian uncompressed size stored in the LZMA-alone header.

  @param[in]  Source  Buffer holding at least CARRIER_HEADER_SIZE bytes.

  @return The declared size, or MAX_UINT64 for the 0xffffffffffffffff
          placeholder xz writes when the size is not known in advance.
**/
STATIC
UINT64
CarrierDeclaredSize (
  IN CONST UINT8  *Source
  )
{
  UINT64  Size;
  INTN    Byte;

  Size = 0;
  for (Byte = CARRIER_HEADER_SIZE - 1; Byte >= LZMA_PROPS_SIZE; Byte--) {
    Size = LShiftU64 (Size, 8) + Source[Byte];
  }
  return Size;
}

/**
  Scratch allocator handed to the decoder, mirroring LzmaDecompress.c:30-69.
  A bump allocator over a fixed buffer: no free, no fragmentation.
**/
typedef struct {
  ISzAlloc  Funcs;
  UINT8     *Buffer;
  UINTN     BufferSize;
} CARRIER_LZMA_ALLOC;

STATIC
VOID *
CarrierLzmaAlloc (
  ISzAllocPtr  P,
  size_t       Size
  )
{
  CARRIER_LZMA_ALLOC  *Private;
  VOID                *Address;

  Private = (CARRIER_LZMA_ALLOC *)(UINTN)P;
  if (Private->BufferSize < Size) {
    //
    // The EDK2 wrapper asserts here; in a release build it simply returns
    // NULL and the decoder reports SZ_ERROR_MEM.
    //
    return NULL;
  }
  Address             = Private->Buffer;
  Private->Buffer     = Private->Buffer + Size;
  Private->BufferSize = Private->BufferSize - Size;
  return Address;
}

STATIC
VOID
CarrierLzmaFree (
  ISzAllocPtr  P,
  VOID         *Address
  )
{
  (VOID)P;
  (VOID)Address;
}

/**
  Decode one segment straight into its place in the carveout.

  The declared size is both the output limit and the dictionary window
  (Sdk/C/LzmaDec.c:1229-1230), so a stream that would produce more than the
  table says fails instead of running past the segment.  Success requires the
  whole stream consumed and a finished status: a stream that stops early
  leaves the window half written, which is why nothing is published unless
  every segment passes.

  @param[in]   Source       Compressed file, header included.
  @param[in]   SourceSize   Bytes in Source.
  @param[in]   Destination  Where the segment lands.
  @param[in]   Scratch      CARRIER_SCRATCH_SIZE bytes of pool memory.
  @param[out]  DecodedSize  Bytes actually produced.

  @retval RETURN_SUCCESS            The segment decoded completely.
  @retval RETURN_INVALID_PARAMETER  Corrupt, truncated or over-long data.
**/
STATIC
RETURN_STATUS
CarrierLzmaDecode (
  IN  CONST UINT8  *Source,
  IN  UINTN         SourceSize,
  IN  VOID          *Destination,
  IN  VOID          *Scratch,
  OUT UINTN         *DecodedSize
  )
{
  CARRIER_LZMA_ALLOC  Alloc;
  SRes                Result;
  ELzmaStatus         Status;
  SizeT               DestinationLength;
  SizeT               SourceLength;

  *DecodedSize = 0;
  if (SourceSize < CARRIER_HEADER_SIZE) {
    return RETURN_INVALID_PARAMETER;
  }

  Alloc.Funcs.Alloc = CarrierLzmaAlloc;
  Alloc.Funcs.Free  = CarrierLzmaFree;
  Alloc.Buffer      = (UINT8 *)Scratch;
  Alloc.BufferSize  = CARRIER_SCRATCH_SIZE;

  DestinationLength = (SizeT)CarrierDeclaredSize (Source);
  SourceLength      = (SizeT)(SourceSize - CARRIER_HEADER_SIZE);

  Result = LzmaDecode (
             (Byte *)Destination,
             &DestinationLength,
             (CONST Byte *)(Source + CARRIER_HEADER_SIZE),
             &SourceLength,
             Source,
             LZMA_PROPS_SIZE,
             LZMA_FINISH_END,
             &Status,
             &Alloc.Funcs
             );

  *DecodedSize = (UINTN)DestinationLength;
  if (Result != SZ_OK) {
    return RETURN_INVALID_PARAMETER;
  }
  //
  // LzmaDecode reports the compressed bytes it consumed, not the bytes left
  // over (Sdk/C/LzmaDec.c:1232 seeds *srcLen with inSize and
  // LzmaDec_DecodeToDic overwrites it with the consumed count), so a complete
  // stream is one that consumed every byte after the 13-byte header.
  //
  if (SourceLength != (SizeT)(SourceSize - CARRIER_HEADER_SIZE)) {
    return RETURN_INVALID_PARAMETER;
  }
  if ((Status != LZMA_STATUS_FINISHED_WITH_MARK) &&
      (Status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)) {
    return RETURN_INVALID_PARAMETER;
  }
  return RETURN_SUCCESS;
}

/**
  Walk the ADT looking for one property of /chosen/memory-map.

  Same 36-byte property header, 32-byte NUL-padded name, 4-byte alignment and
  64-level depth limit as DarwinBoot.c AdtNode (:206-309).

  @param[in]      Data         ADT image.
  @param[in]      Size         Bytes in Data.
  @param[in,out]  Offset       Cursor; advanced past this node.
  @param[in]      Depth        Current depth, 0 at the root.
  @param[in]      ParentPath   Path identifier of the parent node.
  @param[in]      Name         Property name to find.
  @param[in,out]  Found        Offset of the value, MAX_UINTN while absent.
  @param[in,out]  FoundLength  Length of the value.

  @retval TRUE   The node and all its children were well formed.
  @retval FALSE  Malformed ADT, or the property appears twice or elsewhere.
**/
STATIC
BOOLEAN
AdtWalk (
  IN     CONST UINT8  *Data,
  IN     UINTN         Size,
  IN OUT UINTN        *Offset,
  IN     UINTN         Depth,
  IN     UINTN         ParentPath,
  IN     CONST CHAR8  *Name,
  IN OUT UINTN        *Found,
  IN OUT UINT32       *FoundLength
  )
{
  UINT32       Properties;
  UINT32       Children;
  UINT32       Length;
  UINT32       CandidateLength;
  CONST UINT8  *NodeName;
  UINTN        NodeNameLength;
  UINTN        Candidate;
  UINTN        Path;
  UINTN        Index;
  BOOLEAN      HaveCandidate;

  if ((Depth > 64) || (Size - *Offset < 8)) {
    return FALSE;
  }
  Properties = ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Data + *Offset));
  Children   = ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Data + *Offset + 4));
  *Offset   += 8;
  if (Properties > (Size - *Offset) / 36) {
    return FALSE;
  }

  NodeName        = NULL;
  NodeNameLength  = 0;
  Candidate       = MAX_UINTN;
  CandidateLength = 0;
  HaveCandidate   = FALSE;

  for (Index = 0; Index < Properties; Index++) {
    if (Size - *Offset < 36) {
      return FALSE;
    }
    if (ScanMem8 (Data + *Offset, 32, 0) == NULL) {
      return FALSE;
    }
    Length = ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Data + *Offset + 32));
    if (Length > Size - *Offset - 36) {
      return FALSE;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset), "name") == 0) {
      if (NodeName != NULL) {
        return FALSE;
      }
      NodeName       = Data + *Offset + 36;
      NodeNameLength = Length;
    } else if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset), Name) == 0) {
      if (HaveCandidate) {
        return FALSE;
      }
      HaveCandidate   = TRUE;
      Candidate       = *Offset + 36;
      CandidateLength = Length;
    }
    *Offset += 36 + Length;
    *Offset  = ALIGN_VALUE (*Offset, 4);
    if (*Offset > Size) {
      return FALSE;
    }
  }

  Path = 3;
  if (Depth == 0) {
    Path = 0;
  } else if ((Depth == 1) && (ParentPath == 0) && (NodeNameLength == 7) &&
             (CompareMem (NodeName, "chosen", 7) == 0)) {
    Path = 1;
  } else if ((Depth == 2) && (ParentPath == 1) && (NodeNameLength == 11) &&
             (CompareMem (NodeName, "memory-map", 11) == 0)) {
    Path = 2;
  }

  if (HaveCandidate) {
    if ((Path != 2) || (*Found != MAX_UINTN)) {
      return FALSE;
    }
    *Found       = Candidate;
    *FoundLength = CandidateLength;
  }

  if (Children > (Size - *Offset) / 8) {
    return FALSE;
  }
  for (Index = 0; Index < Children; Index++) {
    if (!AdtWalk (Data, Size, Offset, Depth + 1, Path, Name, Found, FoundLength)) {
      return FALSE;
    }
  }
  return TRUE;
}

/**
  Find /chosen/memory-map/<Name> and report where its value starts.

  @param[in]   Tree      ADT image.
  @param[in]   TreeSize  Bytes in Tree.
  @param[in]   Name      Property name.
  @param[out]  Offset    Offset of the property value inside Tree.
  @param[out]  Length    Length of the property value.

  @retval EFI_SUCCESS           The property exists exactly once.
  @retval EFI_NOT_FOUND         No such property.
  @retval EFI_COMPROMISED_DATA  The ADT is malformed or the property repeats.
**/
STATIC
EFI_STATUS
AdtFindMemoryMapProperty (
  IN  CONST VOID   *Tree,
  IN  UINTN         TreeSize,
  IN  CONST CHAR8  *Name,
  OUT UINTN        *Offset,
  OUT UINT32       *Length
  )
{
  UINTN    Cursor;
  UINTN    Found;
  UINT32   FoundLength;
  BOOLEAN  Valid;

  Cursor      = 0;
  Found       = MAX_UINTN;
  FoundLength = 0;
  Valid       = AdtWalk ((CONST UINT8 *)Tree, TreeSize, &Cursor, 0, 3, Name, &Found, &FoundLength);
  if (!Valid) {
    return EFI_COMPROMISED_DATA;
  }
  if (Found == MAX_UINTN) {
    return EFI_NOT_FOUND;
  }
  *Offset = Found;
  *Length = FoundLength;
  return EFI_SUCCESS;
}

/**
  Read, decode and place one carrier segment.

  The header is read first and compared with the table: a file whose
  declared size disagrees is never read into pool, so a corrupt entry costs
  one 13-byte read instead of 40 MB.

  @param[in]   Dir         Open EFI\\UniXNU\\MODEM directory.
  @param[in]   Name        Table name, "modem.bNN".
  @param[in]   Bytes       Expected uncompressed size from the table.
  @param[in]   Target      Physical address of the segment.
  @param[in]   Scratch     CARRIER_SCRATCH_SIZE bytes of pool memory.
  @param[out]  Decoded     Bytes produced, 0 on failure.

  @retval EFI_SUCCESS           The segment is in place.
  @retval EFI_NOT_FOUND         The file is gone.
  @retval EFI_COMPROMISED_DATA  Header or stream disagrees with the table.
  @retval EFI_DEVICE_ERROR      Read error.
  @retval EFI_OUT_OF_RESOURCES  The source buffer could not be allocated.
**/
STATIC
EFI_STATUS
CarrierLoadSegment (
  IN  EFI_FILE_PROTOCOL  *Dir,
  IN  CONST CHAR8        *Name,
  IN  UINT32              Bytes,
  IN  UINT64              Target,
  IN  VOID               *Scratch,
  OUT UINTN              *Decoded
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  UINT64             FileSize;
  UINT64             Declared;
  UINT8              Header[CARRIER_HEADER_SIZE];
  UINT8              *Source;
  UINTN              Read;
  RETURN_STATUS      DecodeStatus;

  *Decoded = 0;
  Status   = CarrierOpenFile (Dir, Name, &File, &FileSize);
  if (Status == EFI_NOT_FOUND) {
    Print (L"modem-image: %a missing\n", Name);
    return EFI_NOT_FOUND;
  }
  if (EFI_ERROR (Status)) {
    Print (L"modem-image: read %a failed: %r\n", Name, Status);
    return EFI_DEVICE_ERROR;
  }

  if (FileSize < CARRIER_HEADER_SIZE) {
    File->Close (File);
    Print (L"modem-image: %a header says 0, table says %d\n", Name, (UINTN)Bytes);
    return EFI_COMPROMISED_DATA;
  }
  Read   = CARRIER_HEADER_SIZE;
  Status = File->Read (File, &Read, Header);
  if (EFI_ERROR (Status) || (Read != CARRIER_HEADER_SIZE)) {
    File->Close (File);
    Print (L"modem-image: read %a failed: %r\n", Name, Status);
    return EFI_DEVICE_ERROR;
  }
  Declared = CarrierDeclaredSize (Header);
  if (Declared != Bytes) {
    File->Close (File);
    Print (L"modem-image: %a header says %d, table says %d\n", Name, (UINTN)Declared, (UINTN)Bytes);
    return EFI_COMPROMISED_DATA;
  }

  //
  // The header probe advanced the file position.  Rewind before the full-file
  // read: otherwise the read starts at CARRIER_HEADER_SIZE and comes up short
  // by exactly that many bytes, which the caller reports as EFI_DEVICE_ERROR
  // and the loader never reaches the kernel.
  //
  Status = File->SetPosition (File, 0);
  if (EFI_ERROR (Status)) {
    File->Close (File);
    Print (L"modem-image: rewind %a failed: %r\n", Name, Status);
    return EFI_DEVICE_ERROR;
  }

  Source = AllocatePool ((UINTN)FileSize);
  if (Source == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Read   = (UINTN)FileSize;
  Status = File->Read (File, &Read, Source);
  File->Close (File);
  if (EFI_ERROR (Status) || (Read != FileSize)) {
    Print (L"modem-image: read %a failed: %r\n", Name, Status);
    FreePool (Source);
    return EFI_DEVICE_ERROR;
  }

  DecodeStatus = CarrierLzmaDecode (Source, (UINTN)FileSize, (VOID *)(UINTN)Target, Scratch, Decoded);
  FreePool (Source);
  if (EFI_ERROR (DecodeStatus)) {
    Print (L"modem-image: %a did not decompress (%r)\n", Name, (RETURN_STATUS)DecodeStatus);
    return EFI_COMPROMISED_DATA;
  }
  if (*Decoded != Bytes) {
    Print (L"modem-image: %a decoded %d, table says %d\n", Name, *Decoded, (UINTN)Bytes);
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

/**
  Copy the PAS metadata blob into its reserved page.

  The blob is the ELF header and hash section of modem.mdt, embedded in this
  library by scripts/gen-modem-metadata-header.py.  It is not compressed and
  not part of the segment table: the framework payload step copies only the
  names in MODEM_MANIFEST, so a 28th payload file would need a framework
  change.  PAS_INIT_IMAGE reads the blob from this address.

  @param[in]  Carrier  Filled by CarrierScanMap; AblEnd bounds the page.

  @retval EFI_SUCCESS           The blob is in place and the cache is clean.
  @retval EFI_COMPROMISED_DATA  The array size or the page bounds are wrong.
  @retval EFI_DEVICE_ERROR      The CRC could not be computed.
**/
STATIC
EFI_STATUS
CarrierLoadMetadata (
  IN CONST IMAGE_CARRIER *Carrier
  )
{
  EFI_STATUS  Status;
  UINT32      Crc;

  if (sizeof (gK60ModemMetadata) != K60_MODEM_METADATA_BYTES) {
    Print (
      L"modem-image: metadata array is %d bytes, expected %d\n",
      (UINTN)sizeof (gK60ModemMetadata),
      (UINTN)K60_MODEM_METADATA_BYTES
      );
    return EFI_COMPROMISED_DATA;
  }
  if ((K60_MODEM_METADATA_ADDRESS < ABL_STAGING_BASE) ||
      (K60_MODEM_METADATA_ADDRESS + K60_MODEM_METADATA_BYTES > Carrier->AblEnd)) {
    Print (
      L"modem-image: metadata page 0x%lx is outside ABL staging\n",
      (UINT64)K60_MODEM_METADATA_ADDRESS
      );
    return EFI_COMPROMISED_DATA;
  }

  Status = gBS->CalculateCrc32 ((VOID *)(UINTN)gK60ModemMetadata, sizeof (gK60ModemMetadata), &Crc);
  if (EFI_ERROR (Status)) {
    Print (L"modem-image: metadata crc32 failed: %r\n", Status);
    return EFI_DEVICE_ERROR;
  }

  CopyMem ((VOID *)(UINTN)K60_MODEM_METADATA_ADDRESS, (VOID *)(UINTN)gK60ModemMetadata, K60_MODEM_METADATA_BYTES);
  WriteBackDataCacheRange ((VOID *)(UINTN)K60_MODEM_METADATA_ADDRESS, K60_MODEM_METADATA_BYTES);

  Print (
    L"modem-image: metadata %d bytes crc32 0x%08x at 0x%lx\n",
    (UINTN)K60_MODEM_METADATA_BYTES,
    Crc,
    (UINT64)K60_MODEM_METADATA_ADDRESS
    );
  return EFI_SUCCESS;
}

/**
  Load the carrier named by the generated firmware table.

  @param[in]      Root            Volume root, opened by the caller; the
                                  library never closes it.
  @param[in,out]  Tree            Parsed ADT image in pool memory.  The
                                  16-byte K60ModemImage placeholder is
                                  validated here; the caller writes it.
  @param[in]      TreeSize        Size of Tree in bytes.
  @param[out]     Carrier         Filled on every return path.
  @param[out]     PropertyOffset  Offset of the placeholder inside Tree.

  @retval EFI_SUCCESS           Every segment was placed and published, or
                                the payload carries no EFI\\UniXNU\\MODEM
                                directory (Files == 0).
  @retval EFI_NOT_FOUND         The directory exists but a segment is gone.
  @retval EFI_COMPROMISED_DATA  Size, header, table, ADT or placeholder
                                mismatch.
  @retval EFI_DEVICE_ERROR      Read error, or the window is not reserved.
  @retval EFI_OUT_OF_RESOURCES  A pool allocation failed.
  @retval EFI_BAD_BUFFER_SIZE   Tree is too small for the placeholder.
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
  EFI_STATUS         FirstError;
  EFI_FILE_PROTOCOL  *Dir;
  EFI_FILE_PROTOCOL  *Probe;
  UINT64             ProbeSize;
  UINTN              Index;
  UINTN              Decoded;
  UINT32             PropertyLength;
  UINT32             Files;
  UINT64             Copied;
  UINT64             Zeroed;
  UINT64             Target;
  VOID               *Scratch;

  if ((Root == NULL) || (Tree == NULL) || (Carrier == NULL) || (PropertyOffset == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((DEBUG_ERROR, "carrier: enter root=%p tree=%p size=%u\n", Root, Tree, (UINT32)TreeSize));
  ZeroMem (Carrier, sizeof (*Carrier));
  Carrier->Base     = MODEM_IMAGE_BASE;
  Carrier->Span     = MODEM_IMAGE_WINDOW;
  Carrier->Segments = MODEM_IMAGE_SEGMENTS;
  *PropertyOffset   = 0;
  if (TreeSize < sizeof (UINT64) * 2) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = CarrierCheckTable ();
  DEBUG ((DEBUG_ERROR, "carrier: table status=%r\n", Status));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CarrierScanMap (Carrier);
  DEBUG ((DEBUG_ERROR, "carrier: map status=%r abl=0x%lx\n", Status, Carrier->AblEnd));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The D3 lines are printed by the caller, which owns the log order and the
  // chosen DTB address.  One owner per literal: the acceptance gate greps each
  // line and requires exactly one hit.
  //
  //
  // The directory is opened before the placeholder is examined, and that order
  // is load bearing.  This payload is shared with the display, audio and Wi-Fi
  // images, which carry no modem property at all; refusing them because the
  // placeholder is missing would turn every other image into a brick.  A
  // payload without EFI\UniXNU\MODEM is the one shape that continues to
  // start (doc 15 section 4.4), so the placeholder is only required once the
  // carrier directory proves this payload really carries a modem image.
  //
  DEBUG ((DEBUG_ERROR, "carrier: opening modem dir\n"));
  Status = Root->Open (Root, &Dir, (CHAR16 *)mModemDir, EFI_FILE_MODE_READ, 0);
  DEBUG ((DEBUG_ERROR, "carrier: dir status=%r\n", Status));
  if (Status == EFI_NOT_FOUND) {
    Print (L"modem-image: no carrier in payload, skipped\n");
    *PropertyOffset = 0;
    Carrier->Files  = 0;
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    Print (L"modem-image: open EFI\\UniXNU\\MODEM failed: %r\n", Status);
    return EFI_DEVICE_ERROR;
  }

  //
  // A carrier without a property would be copied into a window nothing reads,
  // so the placeholder is validated before a single byte is moved.
  //
  Status = AdtFindMemoryMapProperty (Tree, TreeSize, "K60ModemImage", PropertyOffset, &PropertyLength);
  if (Status == EFI_NOT_FOUND) {
    Print (L"modem-image: K60ModemImage placeholder missing\n");
    Dir->Close (Dir);
    return EFI_COMPROMISED_DATA;
  }
  if (EFI_ERROR (Status)) {
    Print (L"modem-image: ADT memory-map is malformed\n");
    Dir->Close (Dir);
    return EFI_COMPROMISED_DATA;
  }
  if (PropertyLength != sizeof (UINT64) * 2) {
    Print (L"modem-image: K60ModemImage placeholder is %d bytes, expected 16\n", (UINTN)PropertyLength);
    Dir->Close (Dir);
    return EFI_COMPROMISED_DATA;
  }
  if ((ReadUnaligned64 ((CONST UINT64 *)(UINTN)((UINT8 *)Tree + *PropertyOffset)) != 0) ||
      (ReadUnaligned64 ((CONST UINT64 *)(UINTN)((UINT8 *)Tree + *PropertyOffset + 8)) != 0)) {
    Print (L"modem-image: range already published\n");
    Dir->Close (Dir);
    return EFI_COMPROMISED_DATA;
  }

  DEBUG ((DEBUG_ERROR, "carrier: placeholder ok\n"));

  //
  // A missing sentinel is the one failure that cannot be confused with an
  // empty directory, so it is checked once before the loop starts reporting
  // per-segment errors.
  //
  DEBUG ((DEBUG_ERROR, "carrier: sentinel %a\n", CARRIER_SENTINEL));
  Status = CarrierOpenFile (Dir, CARRIER_SENTINEL, &Probe, &ProbeSize);
  DEBUG ((DEBUG_ERROR, "carrier: sentinel status=%r size=0x%lx\n", Status, ProbeSize));
  if (Status == EFI_NOT_FOUND) {
    Print (L"modem-image: %a missing\n", CARRIER_SENTINEL);
    Dir->Close (Dir);
    return EFI_NOT_FOUND;
  }
  if (EFI_ERROR (Status)) {
    Print (L"modem-image: read %a failed: %r\n", CARRIER_SENTINEL, Status);
    Dir->Close (Dir);
    return EFI_DEVICE_ERROR;
  }
  Probe->Close (Probe);

  Scratch = AllocatePool (CARRIER_SCRATCH_SIZE);
  if (Scratch == NULL) {
    Dir->Close (Dir);
    return EFI_OUT_OF_RESOURCES;
  }

  Files      = 0;
  Copied     = 0;
  Zeroed     = 0;
  FirstError = EFI_SUCCESS;
  DEBUG ((DEBUG_ERROR, "carrier: segments begin\n"));
  for (Index = 0; Index < MODEM_IMAGE_SEGMENTS; Index++) {
    Target = k60_modem_fw_segments[Index].paddr;
    if (k60_modem_fw_segments[Index].bytes == 0) {
      ZeroMem ((VOID *)(UINTN)Target, k60_modem_fw_segments[Index].bss);
      Zeroed += k60_modem_fw_segments[Index].bss;
      continue;
    }
    DEBUG ((DEBUG_ERROR, "carrier: seg %u %a\n", (UINT32)Index, k60_modem_fw_segments[Index].name));
    Status = CarrierLoadSegment (
               Dir,
               k60_modem_fw_segments[Index].name,
               k60_modem_fw_segments[Index].bytes,
               Target,
               Scratch,
               &Decoded
               );
    DEBUG ((DEBUG_ERROR, "carrier: seg %u status=%r decoded=0x%lx\n", (UINT32)Index, Status, (UINT64)Decoded));
    if (EFI_ERROR (Status)) {
      //
      // Keep scanning: one bad file must not hide the state of the other 26.
      // Nothing is published as long as any segment failed.
      //
      if (FirstError == EFI_SUCCESS) {
        FirstError = Status;
      }
      continue;
    }
    ZeroMem ((VOID *)(UINTN)(Target + k60_modem_fw_segments[Index].bytes), k60_modem_fw_segments[Index].bss);
    Copied += k60_modem_fw_segments[Index].bytes;
    Zeroed += k60_modem_fw_segments[Index].bss;
    Files++;
  }

  DEBUG ((DEBUG_ERROR, "carrier: metadata begin first=%r\n", FirstError));
  if (!EFI_ERROR (FirstError)) {
    Status = CarrierLoadMetadata (Carrier);
    DEBUG ((DEBUG_ERROR, "carrier: metadata status=%r\n", Status));
    if (EFI_ERROR (Status)) {
      FirstError = Status;
    }
  }

  FreePool (Scratch);
  Dir->Close (Dir);

  DEBUG ((DEBUG_ERROR, "carrier: finish files=%u copied=0x%lx\n", Files, Copied));
  Carrier->Files  = Files;
  Carrier->Copied = (UINT32)Copied;
  Carrier->Zeroed = (UINT32)Zeroed;
  Carrier->Gaps   = (UINT32)(MODEM_IMAGE_WINDOW - Copied - Zeroed);

  if (EFI_ERROR (FirstError)) {
    Print (L"modem-image: carrier incomplete, %d of %d files placed, nothing published\n", Files, MODEM_IMAGE_SEGMENTS);
    return FirstError;
  }

  WriteUnaligned64 ((UINT64 *)(UINTN)((UINT8 *)Tree + *PropertyOffset), MODEM_IMAGE_BASE);
  WriteUnaligned64 ((UINT64 *)(UINTN)((UINT8 *)Tree + *PropertyOffset + 8), MODEM_IMAGE_WINDOW);

  Print (
    L"modem-image: %d segments, copied %d, zeroed %d, gaps %d untouched\n",
    MODEM_IMAGE_SEGMENTS,
    (UINTN)Copied,
    (UINTN)Zeroed,
    (UINTN)Carrier->Gaps
    );
  return EFI_SUCCESS;
}
