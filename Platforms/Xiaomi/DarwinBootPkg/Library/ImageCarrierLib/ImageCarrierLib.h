/** @file
  Load the modem firmware carrier into its carveout before XNU starts.

  Contract and rationale: k60-cellular docs/research/cellular/
  15-FIRMWARE-CARRIER-LOADER.md.  The window is reserved by the platform
  memory map and is not a Boot Services resource, so nothing is allocated
  for it and nothing is released on failure.
**/

#ifndef IMAGE_CARRIER_LIB_H_
#define IMAGE_CARRIER_LIB_H_

#include <Uefi.h>

typedef struct {
  UINT64  Base;      // MODEM_IMAGE_BASE, 0x8bc00000
  UINT64  Span;      // MODEM_IMAGE_WINDOW, 0x13200000
  UINT64  AblEnd;    // Display Reserved start, 0 if the map lacks it
  UINT32  Files;     // 27 when a carrier was loaded, 0 when skipped
  UINT32  Copied;    // sum of bytes, expected 109060761
  UINT32  Zeroed;    // sum of bss, expected 211012967
  UINT32  Gaps;      // Window - Copied - Zeroed, expected 790528
  UINT32  Segments;  // MODEM_IMAGE_SEGMENTS, 30
  UINT32  Reserved;
  BOOLEAN MetadataCovered;  // a reserved descriptor covers the PAS page
  BOOLEAN IpaCovered;       // a reserved descriptor covers ipa_fw_region
} IMAGE_CARRIER;

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

  @retval EFI_SUCCESS           Carrier loaded, or the payload carries no
                                EFI\UniXNU\MODEM directory (Files == 0).
  @retval EFI_NOT_FOUND         The directory exists but modem.b01 is gone.
  @retval EFI_COMPROMISED_DATA  Size, table, range or placeholder mismatch.
  @retval EFI_DEVICE_ERROR      Read error, or the window is not reserved.
  @retval EFI_OUT_OF_RESOURCES  A scratch allocation failed.
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
  );

#define K60_IPA_IMAGE_BASE   0x8B900000ULL
#define K60_IPA_IMAGE_BYTES  0x0000E000ULL

/**
  Copy the IPA firmware from the payload root into the reserved
  ipa_fw_region.

  The image is a plain root file, IPA\ipa_fws.mbn, exactly where the build
  helper injects it.  The destination is the 64 KiB ipa_fw_region at
  0x8b900000; only the first 0xE000 bytes are used because the PAS metadata
  page starts at 0x8b90e000.

  @param[in]  Root     Volume root, opened by the caller.
  @param[in]  Carrier  Filled by ImageCarrierLoad; IpaCovered must be true.

  @retval EFI_SUCCESS           The image was copied, or no IPA directory
                                exists (pong_ipa=0 images).
  @retval EFI_NOT_FOUND         The IPA directory exists but the file is gone.
  @retval EFI_COMPROMISED_DATA  The destination range is not reserved.
  @retval EFI_DEVICE_ERROR      A read or GetInfo call failed.
  @retval EFI_BAD_BUFFER_SIZE   The file is empty or larger than 0xE000.
**/
EFI_STATUS
EFIAPI
ImageIpaLoad (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST IMAGE_CARRIER *Carrier
  );

#endif
