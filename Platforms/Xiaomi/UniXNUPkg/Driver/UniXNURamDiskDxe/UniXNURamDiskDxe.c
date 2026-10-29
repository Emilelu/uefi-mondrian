/** @file
  Publish the FAT initrd from a Mu-Silicium FDT handoff as a read-only block device.

  SPDX-License-Identifier: BSD-2-Clause
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskIo.h>

// Mu-Silicium SEC stores the incoming x0 value in this HOB as a UINT64.
STATIC CONST EFI_GUID mEntryArgHobGuid = {
  0xDAEE82E6, 0x8090, 0x4E72, { 0xBF, 0xB9, 0x78, 0xC1, 0x3C, 0xD4, 0xE3, 0x59 }
};

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

#define RAM_BLOCK_SIZE  512

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0xA9C6827C, 0x1BD8, 0x410C, { 0x88, 0x5A, 0xC4, 0x65, 0x00, 0xF7, 0xD8, 0x36 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8                    *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA       mMedia;

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

//
// boot45: reject initrd ranges that the platform memory map does not
// describe as mapped (AddMem) memory. ABL hands us /chosen initrd values
// pointing at its own staging areas (e.g. 0xB5760000) which are NOT mapped
// by our MMU -> Data Abort on the first block read.
//
STATIC
BOOLEAN
IsRangeMapped (
  IN UINT64  Start,
  IN UINT64  End
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR *Map;
  UINT8                         Count;
  UINT8                         i;

  GetMemoryMap (&Map, &Count);

  for (i = 0; i < Count; i++) {
    if (Map[i].HobOption != AddMem) {
      continue;
    }

    if ((Start >= Map[i].Address) && (End <= Map[i].Address + Map[i].Length)) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS  Status;
  INT32       Chosen;
  CONST VOID  *StartProp;
  CONST VOID  *EndProp;
  INT32       StartLength;
  INT32       EndLength;
  UINT64      Start;
  UINT64      End;
  EFI_HANDLE  Handle;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no /chosen\n"));
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no initrd in /chosen\n"));
    return;
  }

  Start = ReadCells (StartProp, StartLength / 4);
  End   = ReadCells (EndProp, EndLength / 4);
  if (End <= Start) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: initrd 0x%lx-0x%lx is empty\n", Start, End));
    return;
  }

  //
  // Unconditional range dump: the alignment check below used to bail out
  // through DEBUG_WARN, which disappears as soon as DEBUG_WARN is masked
  // off, leaving no trace of what the bootloader actually handed over.
  //
  DEBUG ((
    DEBUG_ERROR,
    "UNIXNURAMDISK: initrd start=0x%lx end=0x%lx size=0x%lx rem512=0x%lx clen=%d/%d\n",
    Start,
    End,
    End - Start,
    (UINT64)((End - Start) % RAM_BLOCK_SIZE),
    StartLength,
    EndLength
    ));

  //
  // The kernel gets ONE initrd assembled from several ramdisk images and
  // /chosen points at the whole thing: on K60 a 22.6 MiB vendor_boot
  // ramdisk sits in FRONT of our 192 MiB payload FAT, so sector 0 of Start
  // is a cpio header (observed sig=A0CD bps=14128) and the FAT driver
  // refused the volume. Walk forward for a real boot sector instead.
  //
  {
    UINT64  Probe;
    UINT8   *Boot;
    UINT32  Bps;

    //
    // Byte by byte: the boundary between the concatenated ramdisk images is
    // not sector aligned (the total is 0x3C bytes past a sector), so a
    // sector-stepped scan steps over the boot sector entirely.
    //
    for (Probe = Start; Probe < End; Probe++) {
      if ((Probe + RAM_BLOCK_SIZE) > End) {
        break;
      }

      Boot = (UINT8 *)(UINTN)Probe;
      if ((Boot[0] != 0xEB) && (Boot[0] != 0xE9)) {
        continue;
      }

      if ((Boot[510] != 0x55) || (Boot[511] != 0xAA)) {
        continue;
      }

      Bps = (UINT32)Boot[11] | ((UINT32)Boot[12] << 8);
      if ((Bps != 512) && (Bps != 1024) && (Bps != 2048) && (Bps != 4096)) {
        continue;
      }

      if ((CompareMem (Boot + 0x36, "FAT", 3) != 0) &&
          (CompareMem (Boot + 0x52, "FAT", 3) != 0))
      {
        continue;
      }

      break;
    }

    if ((Probe + RAM_BLOCK_SIZE) > End) {
      DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no FAT boot sector in 0x%lx bytes\n", End - Start));
      return;
    }

    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: FAT boot sector at start+0x%lx\n", Probe - Start));
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: initrd range 0x%lx-0x%lx, FAT starts at 0x%lx\n",
            Start, End, Probe));
    //
    // Only the front of the range is vendor data: our FAT runs from its boot
    // sector to the very end of the initrd, so End must not move with Start.
    // Moving it truncated the volume by the vendor length and the FAT driver
    // then failed every read past that point (FatDiskIo: Invalid Parameter).
    //
    Start = Probe;
  }
  //
  // A real payload FAT volume is sector aligned. ABL can still point
  // linux,initrd-* at its own staging buffer (observed 0xB6961000 on K60);
  // publishing a partial sector would hand FAT a truncated volume. Keep
  // the skip, but make it a warning: this is an environment fact, not a
  // firmware error. Payload builds must pad the FAT to 4 KiB.
  //
  if (((End - Start) % RAM_BLOCK_SIZE) != 0) {
    //
    // Android 12+ ABL concatenates the boot.img ramdisk (our payload FAT,
    // first) with the vendor_boot ramdisk (a ~22 MiB tail). The combined
    // length is not sector aligned, so skipping here dropped a perfectly
    // good FAT volume. Round the tail down: the FAT's own BPB decides how
    // many sectors are real, the leftover bytes are never addressed.
    //
    DEBUG ((
      DEBUG_WARN,
      "UNIXNURAMDISK: initrd size 0x%lx not 512-aligned; rounding end 0x%lx -> 0x%lx\n",
      End - Start,
      End,
      End & ~(UINT64)(RAM_BLOCK_SIZE - 1)
      ));
    End &= ~(UINT64)(RAM_BLOCK_SIZE - 1);
    if (End <= Start) {
      return;
    }
  }

  if (!IsRangeMapped (Start, End)) {
    DEBUG ((
      DEBUG_ERROR,
      "UNIXNURAMDISK: initrd 0x%lx-0x%lx not inside mapped memory, skipping\n",
      Start,
      End
      ));
    return;
  }

  mRamBase                = (UINT8 *)(UINTN)Start;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (End - Start) / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  //
  // A raw BlockIo nobody binds is a dead end: DiskIoDxe has to wrap it and
  // the FAT driver has to mount it before any application can see a volume.
  // Who refused the device is otherwise invisible.
  //
  {
    EFI_STATUS       Connect;
    EFI_STATUS       Second;
    EFI_STATUS       DiskIo;
    EFI_STATUS       Volumes;
    EFI_STATUS       SelfRead;
    UINT8            *Sector;
    VOID             *DiskIoProtocol;
    EFI_HANDLE       *VolumeHandles;
    UINTN            VolumeCount;

    DiskIoProtocol = NULL;
    VolumeHandles  = NULL;
    VolumeCount    = 0;
    //
    // Two passes: the FAT driver only claims the handle once DiskIo exists,
    // and driver order inside a single ConnectController pass is not
    // guaranteed to put DiskIoDxe first.
    //
    Connect = gBS->ConnectController (Handle, NULL, NULL, TRUE);
    Second  = gBS->ConnectController (Handle, NULL, NULL, TRUE);
    DiskIo  = gBS->HandleProtocol (Handle, &gEfiDiskIoProtocolGuid, &DiskIoProtocol);

    //
    // Prove the device serves the boot sector we expect (0x55AA + "MTOO");
    // if this fails, FAT was never given a chance to work.
    //
    Sector   = AllocatePool (RAM_BLOCK_SIZE);
    SelfRead = EFI_OUT_OF_RESOURCES;
    if (Sector != NULL) {
      ZeroMem (Sector, RAM_BLOCK_SIZE);
      SelfRead = mBlockIo.ReadBlocks (&mBlockIo, mMedia.MediaId, 0, RAM_BLOCK_SIZE, Sector);
      DEBUG ((
        DEBUG_ERROR,
        "UNIXNURAMDISK: selfread=%r sig=%02x%02x bps=%u oem=%c%c%c%c\n",
        SelfRead,
        Sector[510],
        Sector[511],
        (UINT32)(Sector[11] | (Sector[12] << 8)),
        Sector[3],
        Sector[4],
        Sector[5],
        Sector[6]
        ));
      FreePool (Sector);
    }
    Volumes = gBS->LocateHandleBuffer (
                             ByProtocol,
                             &gEfiSimpleFileSystemProtocolGuid,
                             NULL,
                             &VolumeCount,
                             &VolumeHandles
                             );
    DEBUG ((
      DEBUG_ERROR,
      "UNIXNURAMDISK: connect=%r second=%r diskio=%r volumes=%u (%r)\n",
      Connect,
      Second,
      DiskIo,
      (UINT32)VolumeCount,
      Volumes
      ));
    if (!EFI_ERROR (Volumes) && (VolumeHandles != NULL)) {
      FreePool (VolumeHandles);
    }
  }
  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: initrd block device 0x%lx size 0x%lx %r\n", Start, End - Start, Status));
}

//
// XNU keeps a console/panic mirror in reserved DRAM (patch
// zzzzz-pong-ramoops-minimal.patch): 12-byte header then a 2 MiB ring.
// A warm reset keeps the contents, so a panic is readable from here.
//
#define K60_RAMOOPS_BASE     0xA7000000ULL
#define K60_RAMOOPS_BYTES    0x00200000U
#define K60_RAMOOPS_HEADER   12U
#define K60_RAMOOPS_MAGIC    0x43474244U

STATIC
VOID
DumpRamoops (
  VOID
  )
{
  volatile UINT32  *Header;
  UINT32           Sig;
  UINT32           Start;
  UINT32           Size;
  UINT32           Capacity;
  UINT32           Take;
  UINT32           Index;
  CHAR8            Line[160];
  UINTN            Used;

  Header = (volatile UINT32 *)(UINTN)K60_RAMOOPS_BASE;
  Sig    = Header[0];
  Start  = Header[1];
  Size   = Header[2];
  if ((Sig != K60_RAMOOPS_MAGIC) || (Size == 0)) {
    DEBUG ((DEBUG_ERROR, "RAMOOPS: sig=0x%08x size=0x%x (no xnu log)\n", Sig, Size));
    return;
  }

  Capacity = K60_RAMOOPS_BYTES - K60_RAMOOPS_HEADER;
  if ((Start >= Capacity) || (Size > Capacity)) {
    Start = 0;
    Size  = (Size > Capacity) ? Capacity : Size;
  }

  Take = (Size > 4096) ? 4096 : Size;
  DEBUG ((DEBUG_ERROR, "RAMOOPS: size=0x%x start=0x%x, tail 0x%x\n", Size, Start, Take));

  Used = 0;
  for (Index = 0; Index < Take; Index++) {
    UINT32  Offset;
    CHAR8   C;

    Offset = (Start + Size - Take + Index) % Capacity;
    C      = *(volatile CHAR8 *)(UINTN)(K60_RAMOOPS_BASE + K60_RAMOOPS_HEADER + Offset);
    if ((C < 0x20) || (C > 0x7E)) {
      C = ((C == '\n') || (C == '\r') || (C == '\t')) ? ' ' : '.';
    }

    Line[Used++] = C;
    if (Used == sizeof (Line) - 1) {
      Line[Used] = '\0';
      DEBUG ((DEBUG_ERROR, "RAMOOPS| %a\n", Line));
      Used = 0;
    }
  }

  if (Used != 0) {
    Line[Used] = '\0';
    DEBUG ((DEBUG_ERROR, "RAMOOPS| %a\n", Line));
  }
}

EFI_STATUS
EFIAPI
UniXNURamDiskDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             Dtb;

  //
  // First thing: whatever XNU left in the reserved console mirror, before
  // any of our own allocations can disturb it.
  //
  DumpRamoops ();

  Hob = GetFirstGuidHob (&mEntryArgHobGuid);
  if (Hob == NULL) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: no entry argument HOB\n"));
    return EFI_SUCCESS;
  }

  if (GET_GUID_HOB_DATA_SIZE (Hob) < sizeof (UINT64)) {
    return EFI_COMPROMISED_DATA;
  }

  Dtb = ReadUnaligned64 ((CONST UINT64 *)GET_GUID_HOB_DATA (Hob));
  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: checking DTB at 0x%lx\n", Dtb));
  if ((Dtb == 0) || ((Dtb & 3) != 0) || (FdtCheckHeader ((VOID *)(UINTN)Dtb) != 0)) {
    DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: x0 0x%lx is not a device tree\n", Dtb));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "UNIXNURAMDISK: device tree at 0x%lx, %u bytes\n", Dtb, (UINT32)FdtTotalSize ((VOID *)(UINTN)Dtb)));

  PublishInitrd ((VOID *)(UINTN)Dtb);
  return EFI_SUCCESS;
}
