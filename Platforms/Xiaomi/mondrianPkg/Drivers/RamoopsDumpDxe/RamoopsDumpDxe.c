/** @file
  Report what the previous boot left in the pong ramoops console mirror.

  The kernel writes console text into a 2 MiB window at 0xA7000000 and records a
  12-byte header (sig, start, size). Whether that window survives a power-on
  reset cannot be decided from the host side, and the kernel has no working
  console yet, so this driver is the only reader that exists: it runs before
  anything else touches the window and dumps the header plus the head and tail
  of the ring to the firmware console.

  It never writes. If firmware or the previous boot left no magic, it says so
  and returns; an empty window is a result, not a failure.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/ArmMmuLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#define RAMOOPS_PHYS_BASE  0xA7000000ULL
#define RAMOOPS_BYTES      0x00200000U
#define RAMOOPS_HEADER     12U
#define RAMOOPS_DATA       (RAMOOPS_BYTES - RAMOOPS_HEADER)
#define RAMOOPS_MAGIC      0x43474244U

// How much of the ring is worth showing: the console ends with the interesting
// line and starts with the boot banner, so show both and elide the middle.
#define SHOW_HEAD          1200U
#define SHOW_TAIL          2400U
#define LINE_WIDTH         96U

#pragma pack(1)
typedef struct {
  UINT32 Sig;
  UINT32 Start;
  UINT32 Size;
} RAMOOPS_HEADER;
#pragma pack()

STATIC UINT16 mColumn;

STATIC VOID Emit (CONST CHAR8 *Text, UINTN Length) {
  UINTN Index;
  for (Index = 0; Index < Length; Index++) {
    CHAR8 C = Text[Index];
    if (C == '\r') { continue; }
    if (C == '\n' || mColumn >= LINE_WIDTH) {
      AsciiPrint ("\n");
      mColumn = 0;
      if (C == '\n') { continue; }
    }
    if (C < 0x20 || C > 0x7E) { C = '.'; }
    AsciiPrint ("%c", C);
    mColumn++;
  }
}

// A bootloader-owned window need not be covered by the firmware's static map,
// and a translation fault would take the whole boot down, so probe first and
// only map when the read would fault.
STATIC EFI_STATUS MakeReadable (UINT64 Address, UINTN Size) {
  UINT64 Base, End, Page, El, Par;
  if (Address < SIZE_1MB || Size == 0 || Address >= LShiftU64 (1, 48) ||
      Size > LShiftU64 (1, 48) - Address) { return EFI_INVALID_PARAMETER; }
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

/**
  Dump the mirror the previous boot left behind.

  @param[in] ImageHandle  Standard entry point.
  @param[in] SystemTable  Standard entry point.

  @retval EFI_SUCCESS     The window was read and reported, empty or not.
**/
EFI_STATUS
EFIAPI
RamoopsDumpEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS     Status;
  RAMOOPS_HEADER Header;
  CONST CHAR8    *Data;
  UINT32         Start, Size, First, Second;

  Status = MakeReadable (RAMOOPS_PHYS_BASE, RAMOOPS_BYTES);
  if (EFI_ERROR (Status)) {
    AsciiPrint ("RAMOOPS: cannot read 0x%lx (%r)\n", (UINT64)RAMOOPS_PHYS_BASE, Status);
    return EFI_SUCCESS;
  }

  CopyMem (&Header, (VOID *)(UINTN)RAMOOPS_PHYS_BASE, sizeof (Header));
  if (Header.Sig != RAMOOPS_MAGIC) {
    AsciiPrint ("RAMOOPS: no signature at 0x%lx (read 0x%x), the window is empty\n",
                (UINT64)RAMOOPS_PHYS_BASE, Header.Sig);
    return EFI_SUCCESS;
  }
  if (Header.Size > RAMOOPS_DATA || Header.Start >= RAMOOPS_DATA) {
    AsciiPrint ("RAMOOPS: signature found but header is out of range (start 0x%x size 0x%x)\n",
                Header.Start, Header.Size);
    return EFI_SUCCESS;
  }

  AsciiPrint ("RAMOOPS: sig ok, start 0x%x, size 0x%x of 0x%x\n",
              Header.Start, Header.Size, (UINT32)RAMOOPS_DATA);
  if (Header.Size == 0) {
    AsciiPrint ("RAMOOPS: the mirror is initialised but holds no text\n");
    return EFI_SUCCESS;
  }

  Data  = (CONST CHAR8 *)(UINTN)(RAMOOPS_PHYS_BASE + RAMOOPS_HEADER);
  Start = Header.Start;
  Size  = Header.Size;
  First = Size < SHOW_HEAD ? Size : SHOW_HEAD;
  if (Start + First <= RAMOOPS_DATA) {
    Emit (Data + Start, First);
  } else {
    Emit (Data + Start, RAMOOPS_DATA - Start);
    Emit (Data, First - (RAMOOPS_DATA - Start));
  }

  if (Size > SHOW_HEAD + SHOW_TAIL) {
    Second = SHOW_TAIL;
    AsciiPrint ("\nRAMOOPS: ... %u bytes elided ...\n", Size - SHOW_HEAD - Second);
  } else if (Size > SHOW_HEAD) {
    Second = Size - SHOW_HEAD;
  } else {
    Second = 0;
  }
  if (Second != 0) {
    UINT32 Tail = (Start + Size - Second) % RAMOOPS_DATA;
    if (Tail + Second <= RAMOOPS_DATA) {
      Emit (Data + Tail, Second);
    } else {
      Emit (Data + Tail, RAMOOPS_DATA - Tail);
      Emit (Data, Second - (RAMOOPS_DATA - Tail));
    }
  }
  AsciiPrint ("\nRAMOOPS: end of mirror\n");
  return EFI_SUCCESS;
}
