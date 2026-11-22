/** @file
  Report what the previous boot left in the pong ramoops console mirror.

  The kernel writes console text into a 2 MiB window at 0xA7000000 and records a
  12-byte header (sig, start, size). Whether that window survives a power-on
  reset cannot be decided from the host side, and the kernel has no working
  console yet, so this driver is the only reader that exists: it runs before
  anything else touches the window and dumps the header plus the head and tail
  of the ring to the firmware console.

  It never writes. If the window carries no magic it says so and returns; an
  empty window means the kernel never reached its console init, which is itself
  the answer. The translation is probed before the first read because this pin's
  ArmPkg has no ArmMmuLib header to fix a fault with: an unmapped window is
  reported, not faulted on.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#define RAMOOPS_PHYS_BASE  0xA7000000ULL
#define RAMOOPS_BYTES      0x00200000U
#define RAMOOPS_HEADER     12U
#define RAMOOPS_DATA       (RAMOOPS_BYTES - RAMOOPS_HEADER)
#define RAMOOPS_MAGIC      0x43474244U

// The console ends with the interesting line and starts with the boot banner,
// so show both ends and elide the middle.
#define SHOW_HEAD          1200U
#define SHOW_TAIL          2400U
#define LINE_WIDTH         96U

#pragma pack(1)
typedef struct {
  UINT32 Sig;
  UINT32 Start;
  UINT32 Size;
} RAMOOPS_HDR;
#pragma pack()

STATIC UINT32 mColumn;

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
    AsciiPrint ("%c", (CHAR16)(UINT8)C);
    mColumn++;
  }
}

/**
  Report whether one page of the window is readable at the current exception
  level, using AT so an unmapped page is a PAR result and not a data abort.

  @param[in] Address  Page address to probe.

  @retval TRUE   The page translates.
  @retval FALSE  The page does not translate, or this EL cannot ask.
**/
STATIC BOOLEAN Readable (UINT64 Address) {
  UINT64 El, Par;
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (El));
  if (El == 4) {
    __asm__ volatile ("at s1e1r, %1; isb; mrs %0, par_el1" : "=r" (Par) : "r" (Address) : "memory");
  } else if (El == 8) {
    __asm__ volatile ("at s1e2r, %1; isb; mrs %0, par_el1" : "=r" (Par) : "r" (Address) : "memory");
  } else {
    return FALSE;
  }
  return (BOOLEAN)((Par & 1) == 0);
}

/**
  Dump the mirror the previous boot left behind.

  @param[in] ImageHandle  Standard entry point.
  @param[in] SystemTable  Standard entry point.

  @retval EFI_SUCCESS     The window was read and reported, empty or not.
**/
EFI_STATUS
EFIAPI
RamoopsDumpInner (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  RAMOOPS_HDR Header;
  CONST CHAR8    *Data;
  UINT32         Start, Size, First, Second;

  if (!Readable (RAMOOPS_PHYS_BASE)) {
    AsciiPrint ("RAMOOPS: 0x%lx does not translate here, nothing to report\n",
                (UINT64)RAMOOPS_PHYS_BASE);
    return EFI_SUCCESS;
  }

  CopyMem (&Header, (VOID *)(UINTN)RAMOOPS_PHYS_BASE, sizeof (Header));
  if (Header.Sig != RAMOOPS_MAGIC) {
    AsciiPrint ("RAMOOPS: no signature at 0x%lx (read 0x%08x), the window is empty\n",
                (UINT64)RAMOOPS_PHYS_BASE, Header.Sig);
    return EFI_SUCCESS;
  }
  if (Header.Size > RAMOOPS_DATA || Header.Start >= RAMOOPS_DATA) {
    AsciiPrint ("RAMOOPS: signature found but the header is out of range (start 0x%x size 0x%x)\n",
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

/**
  Keep the text on screen long enough for a human to read it.

  The kernel has no console yet, so this dump is the only instrument we have and
  it exists for twelve seconds during a manual boot.  A key press ends the wait.
**/
VOID
RamoopsHold (
  VOID
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Left;

  AsciiPrint ("RAMOOPS: holding this screen for 20 s (press a key to continue)\n");
  for (Left = 0; Left < 20; Left++) {
    gBS->Stall (1000 * 1000);
    if ((gST->ConIn != NULL) &&
        (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key) == EFI_SUCCESS))
    {
      break;
    }
  }
}

EFI_STATUS
EFIAPI
RamoopsDumpEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  RamoopsDumpInner (ImageHandle, SystemTable);
  RamoopsHold ();
  return EFI_SUCCESS;
}
