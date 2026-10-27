/** @file
  Stand-alone Advanced Logger producer for mondrian.

  The pinned mu_basecore feed's DxeCore does not consume AdvancedLoggerLib,
  so nobody installs gAdvancedLoggerProtocolGuid and UniXNU's AdvancedLogDxe
  menu plugin (Tools -> Advanced Logger) never dispatches. This driver hosts
  the official AdvLoggerPkg DxeCore-instance sources as a DXE_DRIVER so the
  constructor allocates the log buffer and installs the protocol during DXE
  dispatch. Wiring per docs/porting/log-capture.md step 1-2.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>

//
// Defined by the AdvLoggerPkg DxeCore AdvancedLoggerLib source compiled into
// this driver. The EDK2 build only auto-calls constructors of libraries
// listed in [LibraryClasses]; these sources are in [Sources], so AutoGen.c
// does NOT call it. Do it explicitly before the driver entry body.
//
EFI_STATUS
EFIAPI
DxeCoreAdvancedLoggerLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

EFI_STATUS
EFIAPI
AdvLogDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = DxeCoreAdvancedLoggerLibConstructor (ImageHandle, SystemTable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ADVLOG: producer constructor failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "ADVLOG: producer ready, protocol installed\n"));
  return EFI_SUCCESS;
}
