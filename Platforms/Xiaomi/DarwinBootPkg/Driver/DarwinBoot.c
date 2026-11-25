// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Protocol/UniXNULoader.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include "BootArgs.h"
#include "BootVideo.h"
#include "PackedRamdisk.h"
#include "WindowSize.h"
#include "../Library/ImageCarrierLib/ImageCarrierLib.h"

#define WINDOW_SIZE  SIZE_256MB
#define BLOCK_SIZE   SIZE_2MB
#define MAX_SEGMENTS 64

typedef struct {
  UINT64  Address;
  UINT64  Size;
  UINT64  FileOffset;
  UINT64  FileSize;
  UINT32  Protection;
} SEGMENT;

typedef struct {
  SEGMENT  Segments[MAX_SEGMENTS];
  UINTN    Count;
  UINT64   Base;
  UINT64   End;
  UINT64   Entry;
} KERNEL;

STATIC EFI_HANDLE  mImage;
STATIC EFI_HANDLE  mReplacedHandle;
STATIC VOID        *mReplacedLoader;
UINT64 EFIAPI DarwinCurrentEl (VOID);
VOID EFIAPI DarwinEnter (UINT64 Args, UINT64 Entry, UINT64 Memory, UINT64 PopulatedBytes, UINT64 VideoBase, UINT64 VideoBytes);
VOID EFIAPI DarwinEnterEnd (VOID);

STATIC
VOID
FlushRange (UINT64 Base, UINT64 Bytes)
{
  UINT64 Ctr, Line, Address;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (Ctr));
  Line = 4ULL << ((Ctr >> 16) & 15);
  __asm__ volatile ("dsb sy" ::: "memory");
  for (Address = Base & ~(Line - 1); Address < Base + Bytes; Address += Line) {
    __asm__ volatile ("dc civac, %0" :: "r" (Address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

#define READ_SYSREG(Name)  ({ UINT64 Value_; __asm__ volatile ("mrs %0, " #Name : "=r" (Value_)); Value_; })

STATIC UINT32 U32 (CONST VOID *P) { return ReadUnaligned32 ((CONST UINT32 *)P); }
STATIC UINT64 U64 (CONST VOID *P) { return ReadUnaligned64 ((CONST UINT64 *)P); }

STATIC
EFI_STATUS
ReadBootVideo (UINT64 Video[6])
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL           *Gop;
  EFI_STATUS                            Status;

  ZeroMem (Video, 6 * sizeof (*Video));
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }
  if (Status == EFI_NOT_FOUND) { return EFI_SUCCESS; }
  if (EFI_ERROR (Status)) { return Status; }
  return DarwinBootVideoFromGop (Gop, Video);
}

STATIC
EFI_STATUS
ReadFile (
  EFI_FILE_PROTOCOL *Root,
  CONST CHAR16      *Path,
  UINTN             Limit,
  VOID              **Data,
  UINTN             *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              Actual;

  *Data = NULL;
  *Size = 0;
  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Info = NULL;
  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) { goto Done; }
  Info = AllocatePool (InfoSize);
  if (Info == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) { goto Done; }
  if ((Info->Attribute & EFI_FILE_DIRECTORY) || (Info->FileSize == 0) || (Info->FileSize > Limit)) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  *Size = (UINTN)Info->FileSize;
  *Data = AllocatePool (*Size);
  if (*Data == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Actual = *Size;
  Status = File->Read (File, &Actual, *Data);
  if (!EFI_ERROR (Status) && (Actual != *Size)) { Status = EFI_DEVICE_ERROR; }
Done:
  if (Info != NULL) { FreePool (Info); }
  File->Close (File);
  if (EFI_ERROR (Status) && (*Data != NULL)) { FreePool (*Data); *Data = NULL; }
  return Status;
}

STATIC
EFI_STATUS
ParseKernelHeader (CONST UINT8 *Data, UINTN Size, UINTN Header, KERNEL *Kernel)
{
  UINTN    Offset;
  UINTN    End;
  UINTN    Index;
  UINTN    Other;
  UINT32   Count;
  UINT32   Command;
  UINT32   Length;
  SEGMENT  *Segment;
  BOOLEAN  HasEntry;
  BOOLEAN  Executable;

  //
  // Header is where the image starts inside Data: 0 for a kernel that is a
  // single image, and the LC_FILESET_ENTRY offset for a member of a
  // collection. Every bound below is relative to Header, so Size - Header is
  // tested before it is used and never underflows.
  //
  if ((Header > Size) || (Size - Header < 32) ||
      (U32 (Data + Header) != 0xfeedfacf) || (U32 (Data + Header + 4) != 0x100000c) ||
      (((U32 (Data + Header + 8) & 0x00ffffff) != 0) &&
       ((U32 (Data + Header + 8) & 0x00ffffff) != 2)) ||
      ((U32 (Data + Header + 12) != 2) &&
       ((Header != 0) || (U32 (Data + Header + 12) != 12)))) {
    return EFI_UNSUPPORTED;
  }
  if (U32 (Data + Header + 20) > Size - Header - 32) { return EFI_LOAD_ERROR; }
  ZeroMem (Kernel, sizeof (*Kernel));
  Count = U32 (Data + Header + 16);
  End = Header + 32 + U32 (Data + Header + 20);
  Offset = Header + 32;
  HasEntry = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (End - Offset < 8) { return EFI_LOAD_ERROR; }
    Command = U32 (Data + Offset);
    Length = U32 (Data + Offset + 4);
    if ((Length < 8) || (Length & 7) || (Length > End - Offset)) { return EFI_LOAD_ERROR; }
    if (Command == 0x19) {
      if ((Length < 72) || (U32 (Data + Offset + 64) > (Length - 72) / 80) ||
          (Kernel->Count == MAX_SEGMENTS)) { return EFI_LOAD_ERROR; }
      if (U64 (Data + Offset + 32) != 0) {
        Segment = &Kernel->Segments[Kernel->Count++];
        Segment->Address = U64 (Data + Offset + 24);
        Segment->Size = U64 (Data + Offset + 32);
        Segment->FileOffset = U64 (Data + Offset + 40);
        Segment->FileSize = U64 (Data + Offset + 48);
        Segment->Protection = U32 (Data + Offset + 60);
        if ((Segment->Size > WINDOW_SIZE) || (Segment->Address > MAX_UINT64 - Segment->Size) ||
            (Segment->FileSize > Segment->Size) || (Segment->FileOffset > Size) ||
            (Segment->FileSize > Size - Segment->FileOffset)) { return EFI_LOAD_ERROR; }
      }
    } else if (Command == 5) {
      if (HasEntry || (Length != 288) || (U32 (Data + Offset + 8) != 6) ||
          (U32 (Data + Offset + 12) != 68)) { return EFI_LOAD_ERROR; }
      Kernel->Entry = U64 (Data + Offset + 272);
      HasEntry = TRUE;
    }
    Offset += Length;
  }
  if ((Offset != End) || !HasEntry || (Kernel->Count == 0)) { return EFI_LOAD_ERROR; }
  Kernel->Base = MAX_UINT64;
  Executable = FALSE;
  for (Index = 0; Index < Kernel->Count; Index++) {
    Segment = &Kernel->Segments[Index];
    Kernel->Base = MIN (Kernel->Base, Segment->Address);
    Kernel->End = MAX (Kernel->End, Segment->Address + Segment->Size);
    if ((Kernel->Entry >= Segment->Address) && (Kernel->Entry - Segment->Address < Segment->FileSize) &&
        ((Segment->Protection & 4) != 0)) { Executable = TRUE; }
    for (Other = 0; Other < Index; Other++) {
      if ((Segment->Address < Kernel->Segments[Other].Address + Kernel->Segments[Other].Size) &&
          (Kernel->Segments[Other].Address < Segment->Address + Segment->Size)) { return EFI_LOAD_ERROR; }
    }
  }
  if (!Executable || (Kernel->Entry & 3) || (Kernel->Base & EFI_PAGE_MASK) ||
      (Kernel->Base > MAX_UINT64 - WINDOW_SIZE) ||
      (Kernel->End - Kernel->Base > WINDOW_SIZE - SIZE_2MB)) { return EFI_LOAD_ERROR; }
  return EFI_SUCCESS;
}

//
// From the 27 collection on, the kernel is an MH_FILESET (filetype 12): the
// collection only describes the address space, and every real image hangs off
// an LC_FILESET_ENTRY command that names it and gives its header offset. XNU
// itself is the "com.apple.kernel" member.
//
// The collection's own segments are the roots that already cover the member,
// so they stay the copy plan; the member is parsed only to prove that the
// roots really do map it, at the address it advertises, before the loader
// copies a byte.
//
STATIC
EFI_STATUS
ParseKernel (CONST UINT8 *Data, UINTN Size, KERNEL *Kernel)
{
  EFI_STATUS Status;
  KERNEL     Member;
  UINTN      Offset;
  UINTN      End;
  UINTN      Header;
  UINTN      Name;
  UINTN      Index;
  UINTN      Other;
  UINT32     Length;
  BOOLEAN    Found;
  BOOLEAN    Covered;
  SEGMENT    *Child;
  SEGMENT    *Parent;

  Status = ParseKernelHeader (Data, Size, 0, Kernel);
  if (EFI_ERROR (Status) || (U32 (Data + 12) != 12)) { return Status; }
  Offset = 32;
  End = 32 + U32 (Data + 20);
  Found = FALSE;
  while (Offset < End) {
    //
    // ParseKernelHeader() has just proved that this list is well formed and
    // that walking it lands exactly on End. Check it again here anyway: a
    // command with Length 0 spins this loop forever, and a command that
    // overruns the list reads past End - and past the buffer when End is Size.
    //
    if (End - Offset < 8) { return EFI_LOAD_ERROR; }
    Length = U32 (Data + Offset + 4);
    if ((Length < 8) || (Length & 7) || (Length > End - Offset)) { return EFI_LOAD_ERROR; }
    if (U32 (Data + Offset) == 0x80000035) {
      if (Length < 40) { return EFI_LOAD_ERROR; }
      Name = U32 (Data + Offset + 24);
      if ((Name < 32) || (Name >= Length)) { return EFI_LOAD_ERROR; }
      for (Index = Name; (Index < Length) && (Data[Offset + Index] != 0); Index++) {}
      if (Index == Length) { return EFI_LOAD_ERROR; }
      Header = U64 (Data + Offset + 16);
      if ((Header > Size) || (Size - Header < 32) || (Header & 7)) { return EFI_LOAD_ERROR; }
      //
      // Every indexed header must be loaded at the virtual address it claims:
      // that is the property the loader's copy loop relies on.
      //
      Covered = FALSE;
      for (Other = 0; Other < Kernel->Count; Other++) {
        Parent = &Kernel->Segments[Other];
        if ((Header >= Parent->FileOffset) &&
            (Header - Parent->FileOffset <= Parent->FileSize) &&
            (32 <= Parent->FileSize - (Header - Parent->FileOffset)) &&
            (U64 (Data + Offset + 8) == Parent->Address + Header - Parent->FileOffset)) {
          Covered = TRUE;
        }
      }
      if (!Covered) { return EFI_LOAD_ERROR; }
      if ((Index - Name == sizeof ("com.apple.kernel") - 1) &&
          (CompareMem (Data + Offset + Name, "com.apple.kernel", sizeof ("com.apple.kernel")) == 0)) {
        if (Found || (U32 (Data + Header + 12) != 2) ||
            (U32 (Data + Header + 8) != U32 (Data + 8)) ||
            ((U32 (Data + Header + 24) & 0x80000000) == 0)) { return EFI_LOAD_ERROR; }
        Found = TRUE;
        Status = ParseKernelHeader (Data, Size, Header, &Member);
        if (EFI_ERROR (Status) || (Member.Entry != Kernel->Entry)) { return EFI_LOAD_ERROR; }
        for (Index = 0; Index < Member.Count; Index++) {
          Child = &Member.Segments[Index];
          Covered = FALSE;
          for (Other = 0; Other < Kernel->Count; Other++) {
            Parent = &Kernel->Segments[Other];
            if ((Child->Address >= Parent->Address) &&
                (Child->Address - Parent->Address <= Parent->Size) &&
                (Child->Size <= Parent->Size - (Child->Address - Parent->Address)) &&
                ((Child->Protection & ~Parent->Protection) == 0) &&
                ((Child->FileSize == 0) ||
                 ((Child->Address - Parent->Address <= Parent->FileSize) &&
                  (Child->FileSize <= Parent->FileSize - (Child->Address - Parent->Address)) &&
                  (Child->FileOffset == Parent->FileOffset + Child->Address - Parent->Address)))) {
              Covered = TRUE;
            }
          }
          if (!Covered) { return EFI_LOAD_ERROR; }
        }
      }
    }
    Offset += Length;
  }
  return Found ? EFI_SUCCESS : EFI_LOAD_ERROR;
}

STATIC UINTN mAudioDmaOffset = MAX_UINTN;
#define PONG_RAM_MAX 64
#define PONG_RAM_WIRE_SIZE (32 + PONG_RAM_MAX * 16)
STATIC UINTN mRamWireOffset = MAX_UINTN, mRamAllowedOffset = MAX_UINTN, mRamAllowedCount;
typedef struct { UINT64 Start, Size; } PONG_RAM_BANK;

STATIC
BOOLEAN
AdtNode (CONST UINT8 *Data, UINTN Size, UINTN *Offset, UINTN Depth, UINTN ParentPath, UINTN *RamdiskOffset, UINTN *TrustCacheOffset, UINT64 *WindowSize, BOOLEAN *KeepPacked)
{
  UINT32 Properties;
  UINT32 Children;
  UINT32 Length;
  UINTN  Index;
  UINTN  Path;
  UINTN  Candidate;
  UINTN  TrustCandidate;
  UINTN  MemoryCandidate;
  UINTN  AudioCandidate, RamCandidate, AllowedCandidate, AllowedCount;
  CONST UINT8 *Name;
  UINTN  NameLength;

  Name = NULL;
  NameLength = 0;
  Candidate = MAX_UINTN;
  TrustCandidate = MAX_UINTN;
  MemoryCandidate = MAX_UINTN;
  AudioCandidate = MAX_UINTN;
  RamCandidate = AllowedCandidate = MAX_UINTN; AllowedCount = 0;
  if ((Depth > 64) || (Size - *Offset < 8)) { return FALSE; }
  Properties = U32 (Data + *Offset);
  Children = U32 (Data + *Offset + 4);
  *Offset += 8;
  if (Properties > (Size - *Offset) / 36) { return FALSE; }
  for (Index = 0; Index < Properties; Index++) {
    if (Size - *Offset < 36) { return FALSE; }
    if (ScanMem8 (Data + *Offset, 32, 0) == NULL) { return FALSE; }
    Length = U32 (Data + *Offset + 32);
    *Offset += 36;
    if (Length > Size - *Offset) { return FALSE; }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "name") == 0) {
      if (Name != NULL) { return FALSE; }
      Name = Data + *Offset;
      NameLength = Length;
    }
    if ((AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "RAMDisk") == 0) ||
        (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "PackedRAMDisk") == 0)) {
      if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "PackedRAMDisk") == 0) {
        if (*KeepPacked) { return FALSE; }
        *KeepPacked = TRUE;
      }
      if ((Candidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      Candidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-memory-ranges") == 0) {
      if ((mRamWireOffset != MAX_UINTN) || (RamCandidate != MAX_UINTN) || (Length != PONG_RAM_WIRE_SIZE)) { return FALSE; }
      RamCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "pong-allocatable-ram") == 0) {
      if ((mRamAllowedOffset != MAX_UINTN) || (AllowedCandidate != MAX_UINTN) || !Length || Length % 16 || Length / 16 > PONG_RAM_MAX) { return FALSE; }
      AllowedCandidate = *Offset;
      AllowedCount = Length / 16;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "pong-audio-dma") == 0) {
      if ((AudioCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      AudioCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "TrustCache") == 0) {
      if ((TrustCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      TrustCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-memory-size") == 0) {
      if ((MemoryCandidate != MAX_UINTN) || (Length != 8)) { return FALSE; }
      MemoryCandidate = *Offset;
    }
    *Offset += Length;
    if (ALIGN_VALUE (*Offset, 4) > Size) { return FALSE; }
    *Offset = ALIGN_VALUE (*Offset, 4);
  }
  Path = 3;
  if (Depth == 0) { Path = 0; }
  if ((Depth == 1) && (ParentPath == 0) && (NameLength == 7) && (CompareMem (Name, "chosen", 7) == 0)) { Path = 1; }
  if ((Depth == 2) && (ParentPath == 1) && (NameLength == 11) && (CompareMem (Name, "memory-map", 11) == 0)) { Path = 2; }
  if ((Path == 2) && (Candidate != MAX_UINTN)) {
    if (*RamdiskOffset != MAX_UINTN) { return FALSE; }
    *RamdiskOffset = Candidate;
  }
  if ((Path == 2) && (TrustCandidate != MAX_UINTN)) {
    if (*TrustCacheOffset != MAX_UINTN) { return FALSE; }
    *TrustCacheOffset = TrustCandidate;
  }
  if (AudioCandidate != MAX_UINTN) {
    if ((Path != 1) || (mAudioDmaOffset != MAX_UINTN)) { return FALSE; }
    mAudioDmaOffset = AudioCandidate;
  }
  if (RamCandidate != MAX_UINTN || AllowedCandidate != MAX_UINTN) {
    if (Path != 1 || RamCandidate == MAX_UINTN || AllowedCandidate == MAX_UINTN) { return FALSE; }
    mRamWireOffset = RamCandidate; mRamAllowedOffset = AllowedCandidate; mRamAllowedCount = AllowedCount;
  }
  if (MemoryCandidate != MAX_UINTN) {
    if ((Path != 1) || (*WindowSize != 0)) { return FALSE; }
    *WindowSize = U64 (Data + MemoryCandidate);
    if (!DarwinWindowSizeValid (*WindowSize)) { return FALSE; }
  }
  if (Children > (Size - *Offset) / 8) { return FALSE; }
  for (Index = 0; Index < Children; Index++) {
    if (!AdtNode (Data, Size, Offset, Depth + 1, Path, RamdiskOffset, TrustCacheOffset, WindowSize, KeepPacked)) { return FALSE; }
  }
  return TRUE;
}

STATIC
BOOLEAN
ValidTrustCaches (CONST UINT8 *Data, UINTN Size)
{
  UINTN Count;
  UINTN Index;
  UINTN Start;
  UINTN End;
  UINTN Entries;
  UINTN Entry;

  if (Size < 8) { return FALSE; }
  Count = U32 (Data);
  if ((Count == 0) || (Count > 128) || (Count > (Size - 4) / 4)) { return FALSE; }
  for (Index = 0; Index < Count; Index++) {
    Start = U32 (Data + 4 + Index * 4);
    End = (Index + 1 < Count) ? U32 (Data + 8 + Index * 4) : Size;
    if ((Start < 4 + Count * 4) || (Start & 3) || (End > Size) ||
        (End < Start) || (End - Start < 24) || (U32 (Data + Start) != 2)) { return FALSE; }
    Entries = U32 (Data + Start + 20);
    if ((Entries > (End - Start - 24) / 24) || (End - Start != 24 + Entries * 24)) { return FALSE; }
    for (Entry = 0; Entry < Entries; Entry++) {
      CONST UINT8 *Hash = Data + Start + 24 + Entry * 24;
      if ((Hash[20] < 1) || (Hash[20] > 4) ||
          ((Entry != 0) && (CompareMem (Hash - 24, Hash, 20) >= 0))) { return FALSE; }
    }
  }
  return TRUE;
}

STATIC
EFI_STATUS
GetMap (EFI_MEMORY_DESCRIPTOR **Map, UINTN *Capacity, UINTN *Size, UINTN *Key, UINTN *Stride)
{
  EFI_STATUS Status;
  UINT32     Version;
  UINTN      Attempt;

  for (Attempt = 0; Attempt < 3; Attempt++) {
    *Size = *Capacity;
    Status = gBS->GetMemoryMap (Size, *Map, Key, Stride, &Version);
    if (Status != EFI_BUFFER_TOO_SMALL) { return Status; }
    if (*Map != NULL) { FreePool (*Map); }
    *Capacity = *Size + 32 * sizeof (EFI_MEMORY_DESCRIPTOR);
    *Map = AllocatePool (*Capacity);
    if (*Map == NULL) { return EFI_OUT_OF_RESOURCES; }
  }
  return EFI_OUT_OF_RESOURCES;
}

// End of the run of EfiConventionalMemory that begins at Start: the UEFI map often lists physically
// adjacent free ranges as separate descriptors, so extend over every descriptor that continues the run.
STATIC
UINT64
RunEnd (EFI_MEMORY_DESCRIPTOR *Map, UINTN Size, UINTN Stride, UINT64 Start, UINT64 End)
{
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  UINTN                 Offset;
  BOOLEAN               Extended;

  do {
    Extended = FALSE;
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->PhysicalStart == End) &&
          (Descriptor->NumberOfPages != 0) && (Descriptor->NumberOfPages < (MAX_UINT64 - End) / EFI_PAGE_SIZE)) {
        End += EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);
        Extended = TRUE;
      }
    }
  } while (Extended);
  (VOID)Start;
  return End;
}

// Large diagnostic text straight into the GOP framebuffer: the UEFI text console font is too small to
// photograph. 5x12 glyphs from DiagFont.h scaled 4x, white on a black box; wraps at the right edge.
#include "DiagFont.h"
STATIC CONST UINT64 mDiagFont[] = GLYPH_FONT;

STATIC
VOID
DiagDrawVideo (CONST CHAR16 *Text, UINTN Row, CONST UINT64 Video[6])
{
  UINTN                        Scale = 4, X = 0, Y, Index, Gx, Gy, Dx, Dy, Cell, Line;
  UINT32                       *Fb;
  UINT64                       Glyph, Data;
  CHAR16                       Ch;

  Cell = 6 * Scale;
  Line = 14 * Scale;
  Y = Video[4] / 4 + Row * Line;
  Fb = (UINT32 *)(UINTN)Video[0];
  for (Index = 0; Text[Index] != 0; Index++) {
    Ch = Text[Index];
    if ((Ch < 32) || (Ch > 126)) { continue; }
    if (X + Cell > Video[3]) { X = 0; Y += Line; }
    if (Y + Line > Video[4]) { return; }
    Glyph = mDiagFont[Ch - 32];
    for (Gy = 0; Gy < 14 * Scale; Gy++) {
      for (Gx = 0; Gx < Cell; Gx++) {
        Fb[(Y + Gy) * (Video[2] / 4) + X + Gx] = 0xFF000000;
      }
    }
    for (Gy = 0; Gy < 12; Gy++) {
      Data = (Gy < 6) ? ((Glyph >> 32) >> (Gy * 5)) : ((Glyph & 0xFFFFFFFFULL) >> ((Gy - 6) * 5));
      for (Gx = 0; Gx < 5; Gx++) {
        if ((Data >> Gx) & 1) {
          for (Dy = 0; Dy < Scale; Dy++) {
            for (Dx = 0; Dx < Scale; Dx++) {
              Fb[(Y + Gy * Scale + Dy) * (Video[2] / 4) + X + Gx * Scale + Dx] = 0xFFFFFFFF;
            }
          }
        }
      }
    }
    X += Cell;
  }
  FlushRange (Video[0], Video[2] * Video[4]);
}

STATIC
VOID
DiagDraw (CONST CHAR16 *Text, UINTN Row)
{
  UINT64 Video[6];
  if (!EFI_ERROR (ReadBootVideo (Video)) && Video[0]) { DiagDrawVideo (Text, Row, Video); }
}

/**
  Paint the whole screen through the firmware's GOP Blt, which is the call the
  graphics menu draws with.

  Every direct write into Video[0] from this loader has so far reached the panel
  zero times, while the menu - drawn with Blt - is shown normally.  Blt is
  therefore the only drawing call with a proven record on this board, so the
  hand-off witness has to go through it instead of through FillScreen.
**/
STATIC
EFI_STATUS
GopFill (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Colour
  )
{
  return Gop->Blt (Gop, &Colour, EfiBltVideoFill, 0, 0, 0, 0,
                   Gop->Mode->Info->HorizontalResolution,
                   Gop->Mode->Info->VerticalResolution, 0);
}

/**
  The same fill restricted to one vertical bar; used for the striped frame.
**/
STATIC
EFI_STATUS
GopFillBar (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Colour,
  IN UINTN                          X,
  IN UINTN                          Width
  )
{
  return Gop->Blt (Gop, &Colour, EfiBltVideoFill, 0, 0, X, 0, Width,
                   Gop->Mode->Info->VerticalResolution, 0);
}

/**
  Draw a ruler the panel cannot fake: four bars on the four edges of the mode
  the firmware believes it has, plus a cross at its centre.  If the bars hug the
  panel edges the GOP geometry matches the visible area; if a bar floats inside
  the panel the mode is wider or taller than the glass.

  The cross is drawn as two thin Blt fills so it needs no font.
**/
STATIC
VOID
GopRuler (
  VOID
  )
{
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Blue    = { 0xFF, 0x00, 0x00, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Red     = { 0x00, 0x00, 0xFF, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Yellow  = { 0x00, 0xFF, 0xFF, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Green   = { 0x00, 0xFF, 0x00, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Magenta = { 0xFF, 0x00, 0xFF, 0 };
  UINTN                          Width, Height;

  Gop = NULL;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop == NULL) || (Gop->Mode == NULL)) {
    return;
  }
  Width  = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;

  /* left blue, right red */
  GopFillBar (Gop, Blue, 0, 8);
  GopFillBar (Gop, Red, (Width > 8) ? (Width - 8) : 0, 8);
  /* top yellow, bottom green, drawn as horizontal fills */
  Gop->Blt (Gop, &Yellow, EfiBltVideoFill, 0, 0, 0, 0, Width, 8, 0);
  if (Height > 8) {
    Gop->Blt (Gop, &Green, EfiBltVideoFill, 0, 0, 0, Height - 8, Width, 8, 0);
  }
  /* centre cross: one vertical bar and one horizontal bar, both magenta */
  GopFillBar (Gop, Magenta, Width / 2, 8);
  Gop->Blt (Gop, &Magenta, EfiBltVideoFill, 0, 0, 0, Height / 2, Width, 8, 0);
  gBS->Stall (3000000);
  DEBUG ((DEBUG_ERROR, "GOP RULER: width %lu height %lu pitch %lu base 0x%lx\n",
          Width, Height, Gop->Mode->Info->PixelsPerScanLine,
          Gop->Mode->FrameBufferBase));
}

/**
  The hand-off witness, drawn the way the menu draws: red, then blue, then eight
  black and white bars, one second each, and the bars held for two more seconds.

  Three different pictures rather than one, because a single colour cannot be
  told apart from the last frame the menu left behind: red -> blue -> bars can
  only happen if Blt ran three times and the panel followed it.  Nothing here
  may stop the boot; a missing protocol or a refused blit prints one line and
  returns to the hand-off.
**/
/**
  Leave four quadrants on the panel through GOP Blt.

  This is the last picture before the hand-off and the only three-way
  discriminator available without a console: the quadrants survive
  ExitBootServices and the kernel writes nothing -> the panel is alive and the
  kernel never reached its console; the screen goes black -> the display dies at
  ExitBootServices, which is firmware's problem, not the kernel's; anything else
  -> the kernel reached its console and we have our channel back.

  Drawn with Blt because that is the one call with a proven record here: the
  menu uses it and shows, while every direct write into Video[0] (FillScreen,
  the trampoline's green bar, the DiagDraw stage text) has reached the panel
  zero times in four flights.
**/
STATIC
VOID
GopQuadrants (VOID)
{
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Green = { 0x00, 0xFF, 0x00, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Yellow = { 0x00, 0xFF, 0xFF, 0 };
  UINTN                          Width, Height;

  Gop = NULL;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return;
  }
  Width  = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;
  if ((Width < 2) || (Height < 2)) {
    return;
  }
  GopFillBar (Gop, Green, 0, Width);
  GopFillBar (Gop, Yellow, 0, Width / 2);
  gBS->Stall (1000000);
  DEBUG ((DEBUG_ERROR, "GOP QUADRANTS: blitted %lux%lu, then the hand-off\n", Width, Height));
}

STATIC
VOID
GopWitness (VOID)
{
  EFI_STATUS                     Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Red   = { 0x00, 0x00, 0xFF, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Blue  = { 0xFF, 0x00, 0x00, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black = { 0x00, 0x00, 0x00, 0 };
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  White = { 0xFF, 0xFF, 0xFF, 0 };
  CHAR16                         Line[128];
  UINTN                          Width, Height, Bar, X, Next;

  Gop = NULL;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    UnicodeSPrint (Line, sizeof (Line), L"GOP WITNESS: no graphics output protocol (%r), nothing drawn\r\n", Status);
    gST->ConOut->OutputString (gST->ConOut, Line);
    DEBUG ((DEBUG_ERROR, "GOP WITNESS: no graphics output protocol, nothing drawn\n"));
    return;
  }
  Width  = Gop->Mode->Info->HorizontalResolution;
  Height = Gop->Mode->Info->VerticalResolution;
  UnicodeSPrint (Line, sizeof (Line), L"GOP WITNESS: blt %lux%lu, red blue bars\r\n", Width, Height);
  gST->ConOut->OutputString (gST->ConOut, Line);
  DEBUG ((DEBUG_ERROR, "GOP WITNESS: blt mode %u %lux%lu\n", (UINT32)Gop->Mode->Mode, Width, Height));

  Status = GopFill (Gop, Red);
  if (EFI_ERROR (Status)) {
    UnicodeSPrint (Line, sizeof (Line), L"GOP WITNESS: blt refused (%r), continuing to the hand-off\r\n", Status);
    gST->ConOut->OutputString (gST->ConOut, Line);
    return;
  }
  gBS->Stall (1000000);
  GopFill (Gop, Blue);
  gBS->Stall (1000000);
  GopFill (Gop, Black);
  if (Width >= 8) {
    for (Bar = 1; Bar < 8; Bar += 2) {
      X    = (Width / 8) * Bar;
      Next = (Bar == 7) ? Width : (Width / 8) * (Bar + 1);
      GopFillBar (Gop, White, X, Next - X);
    }
  }
  gBS->Stall (1000000);
  gBS->Stall (2000000);
  DEBUG ((DEBUG_ERROR, "GOP WITNESS: red, blue and the eight bars were blitted\n"));
  return;
}

STATIC
VOID
HandoffStage (CONST UINT64 Video[6], CONST CHAR16 *Text, UINT64 Status)
{
  CHAR16 Line[80];
  if (!Video[0]) { return; }
  UnicodeSPrint (Line, sizeof (Line), L"HANDOFF %s %016lx", Text, Status);
  DiagDrawVideo (Line, 14, Video);
}

STATIC
EFI_STATUS
AllocateWindow (KERNEL *Kernel, UINT64 WindowSize, EFI_PHYSICAL_ADDRESS *Address)
{
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  EFI_STATUS            Status;
  UINTN                 Capacity;
  UINTN                 Size;
  UINTN                 Key;
  UINTN                 Stride;
  UINTN                 Offset;
  UINT64                End;
  UINT64                Candidate;
  UINT64                BestStart;
  UINT64                BestBytes;
  UINT64                RunBytes;

  *Address = 0;
  Map = NULL;
  Capacity = 0;
  BestStart = 0;
  BestBytes = 0;
  Status = GetMap (&Map, &Capacity, &Size, &Key, &Stride);
  if (EFI_ERROR (Status)) { if (Map != NULL) { FreePool (Map); } return Status; }
  Status = EFI_OUT_OF_RESOURCES;
  for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    if ((Descriptor->Type != EfiConventionalMemory) || (Descriptor->NumberOfPages == 0)) { continue; }
    End = RunEnd (Map, Size, Stride, Descriptor->PhysicalStart,
                  Descriptor->PhysicalStart + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
    RunBytes = End - Descriptor->PhysicalStart;
    if (RunBytes > BestBytes) { BestBytes = RunBytes; BestStart = Descriptor->PhysicalStart; }
    Candidate = ALIGN_VALUE (Descriptor->PhysicalStart, BLOCK_SIZE) + (Kernel->Base & (BLOCK_SIZE - 1));
    if ((Candidate < Descriptor->PhysicalStart) || (End < Candidate) || (End - Candidate < WindowSize)) { continue; }
    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (WindowSize), &Candidate);
    if (!EFI_ERROR (Status)) { *Address = Candidate; break; }
  }
  if (EFI_ERROR (Status)) {
    CHAR16 Line[96];
    UINTN  Row = 1;
    UnicodeSPrint (Line, sizeof (Line), L"NO WINDOW %lx BIG RUN %lx @ %lx", WindowSize, BestBytes, BestStart);
    DiagDraw (Line, 0);
    //
    // K60 port delta: the panel text below is gone the moment the screen is
    // reused, and this board has no serial, no network and no writable
    // volume.  Mirror the same numbers into the Advanced Logger so they end
    // up in the QR pages that Tools -> Advanced Logger renders.
    //
    DEBUG ((DEBUG_ERROR, "DARWINBOOT: no window 0x%lx, biggest run 0x%lx at 0x%lx\n",
            WindowSize, BestBytes, BestStart));
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->NumberOfPages >= 0x400) && (Row < 8)) {
        UnicodeSPrint (Line, sizeof (Line), L"FREE %lx + %lx", Descriptor->PhysicalStart, EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
        DiagDraw (Line, Row++);
        DEBUG ((DEBUG_ERROR, "DARWINBOOT: free 0x%lx + 0x%lx\n",
                Descriptor->PhysicalStart, EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages)));
      }
    }

  }
  FreePool (Map);
  return Status;
}

// Claim currently free pages, after all image allocations are complete. The primary
// contiguous window remains unchanged: additional DRAM is passed as separate banks.
STATIC
EFI_STATUS
ClaimPongRam (CONST UINT8 *Tree, EFI_MEMORY_DESCRIPTOR *Map, UINTN MapSize, UINTN Stride,
              PONG_RAM_BANK *Owned, UINTN *Count, CONST UINT64 Video[6])
{
  UINTN Offset, I, J;
  UINT64 PoolStart = 0, PoolEnd = 0, Largest = 0;
  UINTN Skipped = 0;
  EFI_STATUS Status;
  if (mRamWireOffset == MAX_UINTN || mRamAllowedOffset == MAX_UINTN) { return EFI_SUCCESS; }
  // Validate the complete contract before taking ownership of any pages.
  if (Stride < sizeof (EFI_MEMORY_DESCRIPTOR) || !Stride || MapSize % Stride) { return EFI_COMPROMISED_DATA; }
  for (I = 0; I < mRamAllowedCount; I++) {
    UINT64 Base = U64 (Tree + mRamAllowedOffset + I * 16);
    UINT64 Bytes = U64 (Tree + mRamAllowedOffset + I * 16 + 8);
    if (!Bytes || Base > MAX_UINT64 - Bytes || ((Base | Bytes) & EFI_PAGE_MASK)) { return EFI_COMPROMISED_DATA; }
    for (J = 0; J < I; J++) {
      UINT64 Other = U64 (Tree + mRamAllowedOffset + J * 16);
      UINT64 OtherBytes = U64 (Tree + mRamAllowedOffset + J * 16 + 8);
      if (Base < Other + OtherBytes && Other < Base + Bytes) { return EFI_COMPROMISED_DATA; }
    }
  }
  // The qualified FD's DXE heap is below 4 GiB. Do not drain that heap or
  // repurpose low device/uncached aliases while firmware drivers still run.
  // Keep an additional high-memory pool too: final map/pool allocations may
  // be made by firmware callbacks before ExitBootServices has completed.
  for (Offset = 0; Offset + Stride <= MapSize; Offset += Stride) {
    EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    if (D->Type != EfiConventionalMemory || !(D->Attribute & EFI_MEMORY_WB) ||
        !D->NumberOfPages || D->NumberOfPages > (MAX_UINT64 - D->PhysicalStart) / EFI_PAGE_SIZE) { continue; }
    for (I = 0; I < mRamAllowedCount; I++) {
      UINT64 Base = U64 (Tree + mRamAllowedOffset + I * 16);
      UINT64 Bytes = U64 (Tree + mRamAllowedOffset + I * 16 + 8);
      UINT64 Start = MAX (MAX (Base, D->PhysicalStart), 0x100000000ULL);
      UINT64 Stop = MIN (Base + Bytes, D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages));
      if (Stop > Start && Stop - Start > Largest) { Largest = Stop - Start; PoolEnd = Stop; }
    }
  }
  if (Largest > SIZE_256MB) { PoolStart = PoolEnd - SIZE_256MB; }
  else { return EFI_SUCCESS; } // no expendable large bank; retain the primary window
  for (Offset = 0; Offset + Stride <= MapSize; Offset += Stride) {
    EFI_MEMORY_DESCRIPTOR *D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    UINT64 End;
    if (D->Type != EfiConventionalMemory || !(D->Attribute & EFI_MEMORY_WB) || !D->NumberOfPages ||
        D->NumberOfPages > (MAX_UINT64 - D->PhysicalStart) / EFI_PAGE_SIZE) { continue; }
    End = D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages);
    for (I = 0; I < mRamAllowedCount; I++) {
      UINT64 Base = U64 (Tree + mRamAllowedOffset + I * 16);
      UINT64 Bytes = U64 (Tree + mRamAllowedOffset + I * 16 + 8);
      UINT64 Start, Stop;
      Start = MAX (MAX (Base, D->PhysicalStart), 0x100000000ULL); Stop = MIN (Base + Bytes, End);
      if (Start >= Stop) { continue; }
      if (Start < PoolEnd && PoolStart < Stop) { Stop = PoolStart; }
      if (Start >= Stop) { continue; }
      // The video allocation is never general RAM, even if faulty firmware calls it free.
      if (Video[0] && Video[2] && Video[4]) {
        UINT64 Fb = Video[0] & ~(UINT64)EFI_PAGE_MASK;
        UINT64 FbEnd = ALIGN_VALUE (Video[0] + Video[2] * Video[4], EFI_PAGE_SIZE);
        if (Start < FbEnd && Fb < Stop) { continue; }
      }
      while (Start < Stop) {
        UINT64 Address = Start, BytesNow = MIN (Stop - Start, SIZE_1GB);
        if (*Count >= PONG_RAM_MAX) { Skipped++; break; }
        Status = gBS->AllocatePages (AllocateAddress, EfiLoaderData, EFI_SIZE_TO_PAGES (BytesNow), &Address);
        if (EFI_ERROR (Status)) { Skipped++; break; }
        // Coalesce adjacent leases without changing the primary bank. Keep
        // firmware calls bounded even when one descriptor exceeds 4 GiB.
        if (*Count > 1 && Owned[*Count - 1].Start + Owned[*Count - 1].Size == Address) {
          Owned[*Count - 1].Size += BytesNow;
        } else { Owned[(*Count)++] = (PONG_RAM_BANK){Address, BytesNow}; }
        Start += BytesNow;
      }
    }
  }
  if (Skipped) { Print (L"DARWINBOOT: %u additional RAM ranges left with firmware\n", (UINT32)Skipped); }
  return EFI_SUCCESS;
}
STATIC
UINT64
WritePongRam (UINT64 TreeAddress, PONG_RAM_BANK *Owned, UINTN Count)
{
  UINT64 Total = 0;
  UINT8 *Wire = (UINT8 *)(UINTN)(TreeAddress + mRamWireOffset);
  ZeroMem (Wire, PONG_RAM_WIRE_SIZE);
  WriteUnaligned64 ((UINT64 *)Wire, 1);
  WriteUnaligned64 ((UINT64 *)(Wire + 8), Count);
  for (UINTN I = 0; I < Count; I++) {
    WriteUnaligned64 ((UINT64 *)(Wire + 32 + I * 16), Owned[I].Start);
    WriteUnaligned64 ((UINT64 *)(Wire + 40 + I * 16), Owned[I].Size);
    Total += Owned[I].Size;
  }
  WriteUnaligned64 ((UINT64 *)(Wire + 16), Total);
  return Total;
}

STATIC
EFI_STATUS
EFIAPI
Boot (
  IN UNIXNU_LOADER_PROTOCOL    *This,
  IN CONST UNIXNU_LOADER_BOOT_REQUEST *Request
  )
{
  EFI_STATUS                      Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL               *Root;
  VOID                            *Image;
  VOID                            *Tree;
  VOID                            *TrustCaches;
  UINTN                           ImageSize;
  UINTN                           TreeSize;
  UINTN                           TrustCacheSize;
  CHAR16                          *TreePath;
  UINTN                           Length;
  UINTN                           Index;
  UINTN                           Offset;
  KERNEL                          Kernel;
  EFI_PHYSICAL_ADDRESS            Address;
  EFI_PHYSICAL_ADDRESS            AudioAddress = 0;
  UINT64                          ArgsAddress;
  UINT64                          TreeAddress;
  UINT64                          Used;
  UINT64                          WindowSize;
  UINT64                          RamdiskAddress;
  UINTN                           RamdiskOffset;
  UINTN                           RamdiskBytes;
  UINTN                           RamdiskExpandedSize;
  BOOLEAN                         RamdiskPacked;
  BOOLEAN                         KeepPacked;
  UINTN                           TrustCacheOffset;
  UINT64                          TrustCacheAddress;
  UINT64                          TrampolineAddress;
  UINTN                           TrampolineBytes;
  VOID (EFIAPI *Enter)(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64);
  DARWIN_BOOT_ARGS                *Args;
  EFI_MEMORY_DESCRIPTOR           *Map;
  UINTN                           Capacity;
  UINTN                           MapSize;
  UINTN                           Key;
  UINTN                           Stride;
  UINT32                          Version;
  UINTN                           Attempt;
  CHAR8                           CommandLine[DARWIN_BOOT_LINE_LENGTH];
  UINT64                          Video[6];
  PONG_RAM_BANK                   Owned[PONG_RAM_MAX];
  UINTN                           OwnedCount = 0;
  CONST CHAR16                    *Stage = L"REQUEST";
  CHAR16                          Failure[96];
  IMAGE_CARRIER                   Carrier;
  UINTN                           CarrierOffset;

  (VOID)This;
  if ((Request == NULL) || (Request->Volume == NULL) || (Request->KernelPath == NULL)) { return EFI_INVALID_PARAMETER; }
  DiagDraw (L"DARWINBOOT REQUEST", 0);
  if ((Request->KextCount != 0) || (Request->DeviceTree != NULL) || (Request->DeviceTreeSize != 0) ||
      (DarwinCurrentEl () != 4)) {
    Print (L"DARWINBOOT: unsupported inputs or exception level; firmware retained\n");
    return EFI_UNSUPPORTED;
  }
  if (!RamdiskLayout (Request->Ramdisk, Request->RamdiskSize, &RamdiskExpandedSize, &RamdiskPacked)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  ZeroMem (CommandLine, sizeof (CommandLine));
  if (Request->BootArgs != NULL) {
    for (Index = 0; Request->BootArgs[Index] != 0; Index++) {
      if ((Index >= sizeof (CommandLine) - 1) || (Request->BootArgs[Index] > 0x7f)) { return EFI_BAD_BUFFER_SIZE; }
      CommandLine[Index] = (CHAR8)Request->BootArgs[Index];
    }
  }
  Root = NULL;
  Image = NULL;
  Tree = NULL;
  TrustCaches = NULL;
  TreePath = NULL;
  Map = NULL;
  Address = 0;
  Capacity = 0;
  Status = gBS->HandleProtocol (Request->Volume, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) { return Status; }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) { return Status; }
  Stage = L"READ KERNEL";
  DiagDraw (Stage, 0);
  Status = ReadFile (Root, Request->KernelPath, SIZE_128MB, &Image, &ImageSize);
  if (EFI_ERROR (Status)) { goto Fail; }
  Stage = L"PARSE KERNEL";
  Status = ParseKernel (Image, ImageSize, &Kernel);
  if (EFI_ERROR (Status)) { goto Fail; }
  Length = StrSize (Request->KernelPath);
  TreePath = AllocatePool (Length + 11 * sizeof (CHAR16));
  if (TreePath == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Fail; }
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".adt");
  Stage = L"READ ADT";
  Status = ReadFile (Root, TreePath, SIZE_16MB, &Tree, &TreeSize);
  if (EFI_ERROR (Status)) { goto Fail; }
  Offset = 0;
  RamdiskOffset = MAX_UINTN;
  TrustCacheOffset = MAX_UINTN;
  mAudioDmaOffset = MAX_UINTN;
  mRamWireOffset = mRamAllowedOffset = MAX_UINTN;
  mRamAllowedCount = 0;
  WindowSize = 0;
  KeepPacked = FALSE;
  Stage = L"PARSE ADT";
  if (!AdtNode (Tree, TreeSize, &Offset, 0, 3, &RamdiskOffset, &TrustCacheOffset, &WindowSize, &KeepPacked) || (Offset != TreeSize)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  if (KeepPacked && (!RamdiskPacked || (RamdiskOffset == MAX_UINTN))) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  //
  // The modem carrier goes in before the ramdisk and trustcache checks so a
  // malformed ADT is still reported first; the carveout is reserved memory,
  // not a Boot Services resource, so nothing here needs unwinding on failure.
  //
  Stage = L"MODEM CARRIER";
  Status = ImageCarrierLoad (Root, Tree, TreeSize, &Carrier, &CarrierOffset);
  if (EFI_ERROR (Status)) { goto Fail; }
  Stage = L"IPA IMAGE";
  Status = ImageIpaLoad (Root, &Carrier);
  if (EFI_ERROR (Status)) { goto Fail; }
  if (WindowSize == 0) { WindowSize = WINDOW_SIZE; }
  if (Kernel.Base > MAX_UINT64 - WindowSize) { Status = EFI_LOAD_ERROR; goto Fail; }
  if (Request->Ramdisk != NULL) {
    if ((RamdiskOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + RamdiskOffset) != 0) ||
        (U64 ((UINT8 *)Tree + RamdiskOffset + 8) != 0)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  } else if (RamdiskOffset != MAX_UINTN) {
    Status = EFI_UNSUPPORTED;
    goto Fail;
  }
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".trustcache");
  Stage = L"READ TRUSTCACHE";
  Status = ReadFile (Root, TreePath, SIZE_16MB, &TrustCaches, &TrustCacheSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (TrustCaches != NULL) {
    if ((TrustCacheOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + TrustCacheOffset) != 0) ||
        (U64 ((UINT8 *)Tree + TrustCacheOffset + 8) != 0) || !ValidTrustCaches (TrustCaches, TrustCacheSize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (TrustCacheOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  Status = ReadBootVideo (Video);
  if (EFI_ERROR (Status)) { goto Fail; }
  Stage = L"PRIMARY RAM";
  DiagDrawVideo (Stage, 0, Video);
  Status = AllocateWindow (&Kernel, WindowSize, &Address);
  if (EFI_ERROR (Status)) { goto Fail; }
  // Reserve real conventional low RAM for the DSP's 32-bit period buffer.
  // This allocation follows the owned XNU window, so they cannot overlap.
  // Never borrow a hard-coded Android carveout or an arbitrary low address.
  if (mAudioDmaOffset != MAX_UINTN) {
    AudioAddress = 0xffffffffULL;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 16, &AudioAddress);
    if (EFI_ERROR (Status)) { AudioAddress = 0; }
    else {
      ZeroMem ((VOID *)(UINTN)AudioAddress, 65536);
      FlushRange (AudioAddress, 65536);
    }
  }
  ArgsAddress = ALIGN_VALUE (Address + Kernel.End - Kernel.Base, SIZE_16KB);
  TreeAddress = ArgsAddress + EFI_PAGE_SIZE;
  RamdiskAddress = ALIGN_VALUE (TreeAddress + TreeSize, EFI_PAGE_SIZE);
  RamdiskBytes = ALIGN_VALUE (KeepPacked ? Request->RamdiskSize : RamdiskExpandedSize, EFI_PAGE_SIZE);
  TrustCacheAddress = RamdiskAddress + RamdiskBytes;
  Used = ALIGN_VALUE (TrustCacheAddress + TrustCacheSize, SIZE_16KB) - Address;
  TrampolineAddress = Address + Used;
  TrampolineBytes = (UINTN)DarwinEnterEnd - (UINTN)DarwinEnter;
  if ((TrampolineBytes == 0) || (TrampolineBytes > EFI_PAGE_SIZE)) { Status = EFI_LOAD_ERROR; goto Fail; }
  Used += EFI_PAGE_SIZE;
  if (Used > WindowSize - BLOCK_SIZE) { Status = EFI_BAD_BUFFER_SIZE; goto Fail; }
  Stage = L"COPY PAYLOAD";
  DiagDrawVideo (Stage, 0, Video);
  ZeroMem ((VOID *)(UINTN)Address, (UINTN)Used);
  // The MMU-off transition must run inside the identity-mapped owned window,
  // independently of the firmware's dynamically loaded PE image placement.
  CopyMem ((VOID *)(UINTN)TrampolineAddress, (VOID *)(UINTN)DarwinEnter, TrampolineBytes);
  FlushRange (TrampolineAddress, TrampolineBytes);
  __asm__ volatile ("ic iallu\n\tdsb sy\n\tisb" ::: "memory");
  Enter = (VOID (EFIAPI *)(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64))(UINTN)TrampolineAddress;
  for (Index = 0; Index < Kernel.Count; Index++) {
    CopyMem ((VOID *)(UINTN)(Address + Kernel.Segments[Index].Address - Kernel.Base),
             (UINT8 *)Image + Kernel.Segments[Index].FileOffset, (UINTN)Kernel.Segments[Index].FileSize);
  }
  Args = (DARWIN_BOOT_ARGS *)(UINTN)ArgsAddress;
  Args->Revision = 2;
  Args->Version = 2;
  Args->VirtualBase = Kernel.Base;
  Args->PhysicalBase = Address;
  Args->MemorySize = WindowSize;
  Args->MemorySizeActual = WindowSize;
  Args->TopOfKernelData = Address + Used;
  CopyMem (Args->Video, Video, sizeof (Video));
  Args->DeviceTree = Kernel.Base + TreeAddress - Address;
  Args->DeviceTreeLength = (UINT32)TreeSize;
  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));
  CopyMem ((VOID *)(UINTN)TreeAddress, Tree, TreeSize);
  Print (L"modem-image: chosen dtb at 0x%016lx\n", TreeAddress);
  Print (L"modem-image: window 0x8bc00000..0x9ee00000\n");
  Print (L"modem-image: abl staging ends at 0x%016lx\n", Carrier.AblEnd);
  if (Carrier.Files != 0) {
    //
    // Read the placeholder back out of the copy XNU will parse, so a tree that
    // lost the property between the library and the handoff is caught here
    // instead of in the kernel.
    //
    if ((ReadUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + CarrierOffset)) != Carrier.Base) ||
        (ReadUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + CarrierOffset + 8)) != Carrier.Span)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
    Print (L"modem-image: K60ModemImage holds 0x%016lx..0x%016lx\n", Carrier.Base, Carrier.Base + Carrier.Span);
    Print (L"modem-image: range 0x8bc00000..0x9ee00000 published as K60ModemImage\n");
  }
  if (mAudioDmaOffset != MAX_UINTN) {
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + mAudioDmaOffset), AudioAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + mAudioDmaOffset + 8), AudioAddress ? 65536 : 0);
  }
  if (Request->Ramdisk != NULL) {
    if (KeepPacked) { CopyMem ((VOID *)(UINTN)RamdiskAddress, Request->Ramdisk, Request->RamdiskSize); }
    else { CopyRamdisk ((VOID *)(UINTN)RamdiskAddress, Request->Ramdisk, Request->RamdiskSize, RamdiskPacked); }
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset), RamdiskAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset + 8), KeepPacked ? Request->RamdiskSize : RamdiskBytes);
  }
  if (TrustCaches != NULL) {
    CopyMem ((VOID *)(UINTN)TrustCacheAddress, TrustCaches, TrustCacheSize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + TrustCacheOffset), TrustCacheAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + TrustCacheOffset + 8), TrustCacheSize);
    FreePool (TrustCaches); TrustCaches = NULL;
  }
  FreePool (Image); Image = NULL;
  FreePool (Tree); Tree = NULL;
  FreePool (TreePath); TreePath = NULL;
  Root->Close (Root); Root = NULL;
  Stage = L"MEMORY MAP";
  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);
  if (EFI_ERROR (Status)) { goto Fail; }
  if (mRamWireOffset != MAX_UINTN && AsciiStrStr (CommandLine, "pong_ram_banks=0") == NULL) {
    Stage = L"EXTRA RAM";
    DiagDrawVideo (Stage, 0, Video);
    Owned[0] = (PONG_RAM_BANK){Address, WindowSize}; OwnedCount = 1;
    Status = ClaimPongRam ((CONST UINT8 *)(UINTN)TreeAddress, Map, MapSize, Stride, Owned, &OwnedCount, Video);
    if (EFI_ERROR (Status)) {
      // Contract validation precedes all extra allocations. A malformed
      // optional bank description must not reject the known primary boot.
      Print (L"DARWINBOOT: extra RAM SKIP %r; primary window retained\n", Status);
      OwnedCount = 1;
    }
    Args->MemorySizeActual = WritePongRam (TreeAddress, Owned, OwnedCount);
    Print (L"DARWINBOOT: %u owned RAM banks, %lu MiB usable before kernel reservations\n",
           (UINT32)OwnedCount, Args->MemorySizeActual >> 20);
    // Claiming free descriptors can split the map. Leave room before ExitBootServices.
    Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);
    if (EFI_ERROR (Status)) { goto Fail; }
  }
  Stage = L"READY TO ENTER";
  DiagDrawVideo (Stage, 0, Video);
  Print (L"DARWINBOOT: ARM64 va=0x%lx pa=0x%lx args=0x%lx entry=0x%lx owned=0x%lx\n",
         Kernel.Base, Address, ArgsAddress, Address + Kernel.Entry - Kernel.Base, (UINT64)WindowSize);
  Print (L"DARWINBOOT: video base=0x%lx display=%lu row=%lu width=%lu height=%lu depth=%lu\n",
         Video[0], Video[1], Video[2], Video[3], Video[4], Video[5]);
  Print (L"DARWINBOOT: command line: %a\n", CommandLine);
  Print (L"DARWINBOOT: inputs validated; exiting boot services\n");
  Print (L"DARWINBOOT: boot_args size 0x%lx, command line %u bytes\n",
         (UINT64)sizeof (*Args), (UINT32)sizeof (Args->CommandLine));

  //
  // The same facts in large type over the whole screen. The firmware's log
  // draws far larger than the text console, and the form feed clears the
  // screen first. This is what stays visible if the kernel never draws.
  //
  DEBUG ((DEBUG_ERROR, "\fDARWINBOOT: hand-off to the kernel\n\n"));
  DEBUG ((DEBUG_ERROR, "kernel   va 0x%lx  pa 0x%lx\n", Kernel.Base, Address));
  DEBUG ((DEBUG_ERROR, "entry    pa 0x%lx\n", Address + Kernel.Entry - Kernel.Base));
  DEBUG ((DEBUG_ERROR, "args     pa 0x%lx  window 0x%lx bytes\n", ArgsAddress, (UINT64)WindowSize));
  DEBUG ((DEBUG_ERROR, "video    base 0x%lx  %lux%lu  row %lu bytes  %lu bpp\n", Video[0], Video[3], Video[4], Video[2], Video[5]));
  DEBUG ((DEBUG_ERROR, "entry words %08x %08x %08x %08x\n",
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Address + Kernel.Entry - Kernel.Base)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Address + Kernel.Entry - Kernel.Base + 4)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Address + Kernel.Entry - Kernel.Base + 8)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Address + Kernel.Entry - Kernel.Base + 12))));
  DEBUG ((DEBUG_ERROR, "CurrentEL %lu  SCTLR_EL1 0x%lx  TCR_EL1 0x%lx\n", DarwinCurrentEl () >> 2, READ_SYSREG (sctlr_el1), READ_SYSREG (tcr_el1)));
  DEBUG ((DEBUG_ERROR, "ID_AA64MMFR0_EL1 0x%lx  (PARange %lu, 4 KiB granule %a)\n", READ_SYSREG (id_aa64mmfr0_el1),
          READ_SYSREG (id_aa64mmfr0_el1) & 0xF, (((READ_SYSREG (id_aa64mmfr0_el1) >> 28) & 0xF) == 0xF) ? "NOT supported" : "supported"));
  DEBUG ((DEBUG_ERROR, "ID_AA64PFR0_EL1  0x%lx\n", READ_SYSREG (id_aa64pfr0_el1)));
  DEBUG ((DEBUG_ERROR, "owned trampoline pa 0x%lx (%lu bytes)\n", TrampolineAddress, (UINT64)TrampolineBytes));
  DEBUG ((DEBUG_ERROR, "\nExiting boot services. A green bar means the loader\ntrampoline ran; payload entry is not yet confirmed.\n"));
  //
  // The same three facts through ConOut, not through direct Video[0]
  // writes. The graphics menu draws via GOP Blt directly (Ui.c) and
  // ConOut is only its text-mode fallback, but ConOut is a distinct
  // rendering path from DiagDrawVideo / FillScreen / HandoffStage, so
  // this block is the control: if it shows up, the display still works
  // up to ExitBootServices and the loss is on the other side of the
  // hand-off. Entry words stay in the DEBUG block above, which the
  // Advanced Logger captures.
  //
  {
    CHAR16 Buffer[160];
    UnicodeSPrint (Buffer, sizeof (Buffer),
                   L"HANDOFF VA=0x%lx\r\n"
                   L"ENTRY PA=0x%lx\r\n"
                   L"VIDEO BASE=0x%lx\r\n",
                   Kernel.Base, Address + Kernel.Entry - Kernel.Base, Video[0]);
    gST->ConOut->OutputString (gST->ConOut, Buffer);
  }
  //
  // Last picture before the hand-off, through the menu's own drawing call: red,
  // blue, then eight bars, five seconds in total.  Direct writes have been
  // invisible here three times; if these frames show, Blt is the difference and
  // the panel is still alive at ExitBootServices.
  //
  GopWitness ();
  for (Attempt = 0; Attempt < 3; Attempt++) {
    MapSize = Capacity;
    Status = gBS->GetMemoryMap (&MapSize, Map, &Key, &Stride, &Version);
    if (EFI_ERROR (Status)) {
      HandoffStage (Video, L"GETMAP ERROR", Status);
      if (Attempt == 0) { goto Fail; }
      CpuDeadLoop ();
    }
    //
    // The last picture goes through Blt.  FillScreen, HandoffStage and the
    // trampoline's green bar all write Video[0] directly and none of them has
    // ever reached this panel; drawing them here only hides the Blt picture
    // underneath.  If the quadrants are still on screen after the hand-off, the
    // display survived ExitBootServices and the kernel is what never wrote.
    //
    GopRuler ();
  GopQuadrants ();
    Status = gBS->ExitBootServices (mImage, Key);
    if (!EFI_ERROR (Status)) {
      // All writes by this loader are in [Address, Address + Used): Mach-O
      // segments/BSS, args, ADT, initrd, trustcache and copied trampoline.
      // Untouched free pages are not boot input. Clean only populated data,
      // while boot_args still advertises the complete owned window/banks.
      Enter (ArgsAddress, Address + Kernel.Entry - Kernel.Base, Address, Used, Video[0], Video[2] * Video[4]);
      CpuDeadLoop ();
    }
    if (Status != EFI_INVALID_PARAMETER) { CpuDeadLoop (); }
  }
  CpuDeadLoop ();
  return EFI_ABORTED;
Fail:
  UnicodeSPrint (Failure, sizeof (Failure), L"BOOT FAILED %s %016lx", Stage, Status);
  DiagDraw (Failure, 0);
  if (Root != NULL) { Root->Close (Root); }
  if (Image != NULL) { FreePool (Image); }
  if (Tree != NULL) { FreePool (Tree); }
  if (TrustCaches != NULL) { FreePool (TrustCaches); }
  if (TreePath != NULL) { FreePool (TreePath); }
  if (Map != NULL) { FreePool (Map); }
  for (Index = 1; Index < OwnedCount; Index++) {
    gBS->FreePages (Owned[Index].Start, EFI_SIZE_TO_PAGES (Owned[Index].Size));
  }
  if (AudioAddress != 0) { gBS->FreePages (AudioAddress, 16); }
  if (Address != 0) { gBS->FreePages (Address, EFI_SIZE_TO_PAGES (WindowSize)); }
  Print (L"DARWINBOOT: boot refused: %r; firmware retained\n", Status);
  return Status;
}

STATIC UNIXNU_LOADER_PROTOCOL mLoader = { UNIXNU_LOADER_REVISION, Boot };

STATIC
EFI_STATUS
EFIAPI
Unload (EFI_HANDLE ImageHandle)
{
  if (mReplacedHandle != NULL) {
    return gBS->ReinstallProtocolInterface (mReplacedHandle, &gUniXNULoaderProtocolGuid, &mLoader, mReplacedLoader);
  }
  return gBS->UninstallMultipleProtocolInterfaces (ImageHandle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
}

EFI_STATUS
EFIAPI
DarwinBootEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_STATUS               Status;
  VOID                     *Existing;
  EFI_HANDLE               *Handles;
  UINTN                    Count;

  (VOID)SystemTable;
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { return Status; }
  Status = gBS->LocateProtocol (&gUniXNULoaderProtocolGuid, NULL, &Existing);
  if (!EFI_ERROR (Status)) {
    // Explicit, volatile replacement for the qualified FD's older loader.
    // Unloading this driver restores it; no firmware or phone storage changes.
    if (((UNIXNU_LOADER_PROTOCOL *)Existing)->Revision != UNIXNU_LOADER_REVISION) { return EFI_UNSUPPORTED; }
    Status = gBS->LocateHandleBuffer (ByProtocol, &gUniXNULoaderProtocolGuid, NULL, &Count, &Handles);
    if (EFI_ERROR (Status)) { return Status; }
    if (Count != 1) { FreePool (Handles); return EFI_UNSUPPORTED; }
    Status = gBS->ReinstallProtocolInterface (Handles[0], &gUniXNULoaderProtocolGuid, Existing, &mLoader);
    if (!EFI_ERROR (Status)) {
      mReplacedHandle = Handles[0];
      mReplacedLoader = Existing;
      mImage = ImageHandle;
      Loaded->Unload = Unload;
      Print (L"DARWINBOOT: replaced loader in RAM; Darwin 22 ABI, EL%u\n", (UINT32)(DarwinCurrentEl () >> 2));
    }
    FreePool (Handles);
    return Status;
  }
  if (Status != EFI_NOT_FOUND) { return Status; }
  mImage = ImageHandle;
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
  if (!EFI_ERROR (Status)) {
    Loaded->Unload = Unload;
    Print (L"DARWINBOOT: Darwin 22 loader installed EL%u\n", (UINT32)(DarwinCurrentEl () >> 2));
  }
  return Status;
}
