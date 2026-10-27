/** @file
  K60ButtonsDxe.c — Open-source polled buttons driver for Redmi K60 (SM8475).

  v7 SPMI arbiter register layout (K60 boot36 evidence: v2-style map table at
  core+0x900 read all-zeros and every OBS transaction timed out; SM8475 is a
  waipio-family 2022 SoC). Layout per Linux drivers/spmi/spmi-pmic-arb.c v7:

    APID map table : core + 0x2000 + 4*apid        (PPID = (val >> 8) & 0xFFF)
    OBS channel    : obs + 0x8000*ee + 0x20*apid   (EE0 -> 0x20*apid)
    APID owner     : cnfg + 0x4*apid               (owner = val & 0x7)
    command        : per-channel 0x00 cmd / 0x08 status / 0x18 rdata0
    APID count     : (core + 0x4 FEATURES) & 0x7FF

  Buttons (Qualcomm standard):
    Power       = PON periph status bit 0 (pwrkey)   [PPID 0x012, pon@1200 PMK8350]
    Volume down = PON periph status bit 1 (resin)
    Volume up   = PMIC GPIO pin status (active low)  [PPID 0x0C5, pmk8350 gpio6 @c500]

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/DebugLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/SimpleTextIn.h>

//
// K60 (SM8475) SPMI PMIC arbiter — from live FDT
// (cnfg 0xc42d000, core 0xc400000, obsrvr 0xc440000)
//
#define SPMI_CORE_PA   0x0C400000u
#define SPMI_OBS_PA    0x0C440000u
#define SPMI_CNFG_PA   0x0C42D000u

/* v7 layout */
#define SPMI_REG_VERSION   0x0000u
#define SPMI_REG_FEATURES  0x0004u
#define SPMI_FEATURES_MASK 0x7FFu
#define SPMI_MAP_BASE      0x2000u     /* v7: core + 0x2000 + 4*apid */
#define SPMI_MAP_MAX       1024u
#define SPMI_OBS_CHN       0x20u       /* v7: obs channel stride */
#define SPMI_NO_APID       0xFFFFu

#define PON_REG_STATUS   0x10u         /* bit0 pwrkey, bit1 resin(vol-dn) */
#define GPIO_REG_STATUS  0x10u         /* bit0 = pin value (active low) */

typedef struct {
  UINT32 Core;
  UINT32 Obs;
  UINT32 Cnfg;
} SPMI_REGS;

STATIC SPMI_REGS                      mRegs;
STATIC UINT16                         mApidCount = 0;
STATIC UINT16                         mPonApid   = SPMI_NO_APID;
STATIC UINT16                         mGpioApid  = SPMI_NO_APID;
STATIC EFI_EVENT                      mPollEvent = NULL;
STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL mSimpleTextIn;
STATIC EFI_HANDLE                     mTextInHandle = NULL;
STATIC EFI_INPUT_KEY                  mPendingKey;
STATIC BOOLEAN                        mPendingValid = FALSE;

STATIC
UINT32
Rd32 (
  IN UINT32 Pa,
  IN UINT32 Off
  )
{
  UINT32 V = *(volatile UINT32 *)(UINTN)(Pa + Off);
  __asm__ volatile ("dmb oshld" ::: "memory");
  return V;
}

STATIC
VOID
Wr32 (
  IN UINT32 Pa,
  IN UINT32 Off,
  IN UINT32 Val
  )
{
  __asm__ volatile ("dmb oshst" ::: "memory");
  *(volatile UINT32 *)(UINTN)(Pa + Off) = Val;
  __asm__ volatile ("dsb osh" ::: "memory");
}

/* One-byte EXT_READL through the observer channel (v7: obs + 0x20*apid). */
STATIC
EFI_STATUS
SpmiRead8 (
  IN  UINT16 Apid,
  IN  UINT8  Addr,
  OUT UINT8  *Out
  )
{
  UINT32 Off = SPMI_OBS_CHN * Apid;
  UINT32 Cmd = (1u << 27) | ((UINT32)Addr << 4);
  UINT32 St;
  UINTN  Spin;

  Wr32 (mRegs.Obs, Off, Cmd);

  for (Spin = 0; Spin < 1000; Spin++) {
    St = Rd32 (mRegs.Obs, Off + 0x08u);
    if (St & 1u) {
      if (St & 0x0eu) {
        return EFI_DEVICE_ERROR;
      }

      *Out = (UINT8)Rd32 (mRegs.Obs, Off + 0x18u);
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

/* PPID -> APID via the v7 map table (apps-owned entries preferred). */
STATIC
UINT16
FindApid (
  IN UINT16 Ppid
  )
{
  UINT16 Found = SPMI_NO_APID;

  for (UINT32 i = 0; i < mApidCount; i++) {
    UINT32 Map = Rd32 (mRegs.Core, SPMI_MAP_BASE + 4u * i);

    if ((Map == 0) || (((Map >> 8) & 0xFFFu) != Ppid)) {
      continue;
    }

    UINT32 Owner = Rd32 (mRegs.Cnfg, 4u * i) & 7u;
    if ((Found == SPMI_NO_APID) || (Owner == 0)) {
      Found = (UINT16)i;
    }
  }

  return Found;
}

/**
  Enumerate EE0-owned APIDs: dump PPID + type/subtype for each, so PON/GPIO
  PPIDs can be pinned from the boot log. Only apps-owned channels are touched
  (OBS traffic on foreign-EE channels raises bus errors — boot26 evidence).
**/
STATIC
VOID
EnumeratePeripherals (
  VOID
  )
{
  for (UINT32 i = 0; i < mApidCount; i++) {
    UINT32 Map = Rd32 (mRegs.Core, SPMI_MAP_BASE + 4u * i);

    if (Map == 0) {
      continue;
    }

    UINT32 Owner = Rd32 (mRegs.Cnfg, 4u * i) & 7u;
    if (Owner != 0) {
      continue;
    }

    UINT16 Ppid = (UINT16)((Map >> 8) & 0xFFFu);
    UINT16 Apid = (UINT16)i;
    UINT8  Sub  = 0;
    UINT8  Type = 0;

    EFI_STATUS S2 = SpmiRead8 (Apid, 0x104u, &Type);
    EFI_STATUS S1 = SpmiRead8 (Apid, 0x105u, &Sub);

    DEBUG ((
      DEBUG_ERROR,
      "K60BTN: apid=%3u ppid=%03x type=%02x subtype=%02x (%r/%r)\n",
      Apid,
      Ppid,
      Type,
      Sub,
      S2,
      S1
      ));
  }
}

/**
  Locate the PON peripheral (PMK8350 pon@1200 -> PPID 0x012) and the
  volume-up GPIO peripheral (pmk8350 gpio6 @c500 -> PPID 0x0C5).
**/
STATIC
VOID
LocateButtons (
  VOID
  )
{
  STATIC CONST UINT16 PonCandidates[]  = { 0x012, 0x008 };
  STATIC CONST UINT16 GpioCandidates[] = { 0x0C5, 0x0C0, 0x1C5, 0x1C0, 0x4C5, 0x4C1 };

  for (UINTN i = 0; i < ARRAY_SIZE (PonCandidates); i++) {
    UINT16 Apid = FindApid (PonCandidates[i]);

    if (Apid != SPMI_NO_APID) {
      DEBUG ((DEBUG_ERROR, "K60BTN: PON ppid=%03x apid=%u\n", PonCandidates[i], Apid));
      mPonApid = Apid;
      break;
    }
  }

  for (UINTN i = 0; i < ARRAY_SIZE (GpioCandidates); i++) {
    UINT16 Apid = FindApid (GpioCandidates[i]);

    if (Apid != SPMI_NO_APID) {
      DEBUG ((DEBUG_ERROR, "K60BTN: GPIO ppid=%03x apid=%u\n", GpioCandidates[i], Apid));
      mGpioApid = Apid;
      break;
    }
  }
}

/**
  Poll callback: read PON/GPIO status, stage pending key on change.
**/
STATIC
VOID
EFIAPI
PollButtons (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  static BOOLEAN State[3] = { FALSE, FALSE, FALSE };
  UINT8   Pon = 0, Gpio = 0;
  BOOLEAN PonOk  = !EFI_ERROR (SpmiRead8 (mPonApid, PON_REG_STATUS, &Pon));
  BOOLEAN GpioOk = !EFI_ERROR (SpmiRead8 (mGpioApid, GPIO_REG_STATUS, &Gpio));

  if (!PonOk || !GpioOk) {
    return;
  }

  /* bit0 pwrkey, bit1 resin(vol-down); vol-up is active low on the GPIO */
  INTN Now[3] = {
    (Pon & 1u) ? 1 : 0,
    (Gpio & 1u) ? 0 : 1,
    (Pon & 2u) ? 1 : 0
  };
  UINT16 Scan[3] = { SCAN_NULL, SCAN_UP, SCAN_DOWN };  /* Enter = UnicodeChar 0x0D */

  for (INTN k = 0; k < 3; k++) {
    BOOLEAN Down = Now[k] != 0;

    if ((Down != State[k]) && Down) {
      State[k] = Down;

      mPendingKey.ScanCode    = Scan[k];
      mPendingKey.UnicodeChar = (k == 0) ? 0x0D : 0;
      mPendingValid           = TRUE;
    } else {
      State[k] = Down;
    }
  }
}

/**
  SimpleTextInput.Reset
**/
STATIC
EFI_STATUS
EFIAPI
KbdReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  mPendingValid = FALSE;
  return EFI_SUCCESS;
}

/**
  SimpleTextInput.ReadKeyStroke — consumes the pending key.
**/
STATIC
EFI_STATUS
EFIAPI
KbdReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
  OUT EFI_INPUT_KEY                  *Key
  )
{
  if (!mPendingValid) {
    return EFI_NOT_READY;
  }

  *Key          = mPendingKey;
  mPendingValid = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
K60ButtonsDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  mRegs.Core = SPMI_CORE_PA;
  mRegs.Obs  = SPMI_OBS_PA;
  mRegs.Cnfg = SPMI_CNFG_PA;

  //
  // Identify the arbiter version and APID count from the hardware itself.
  //
  UINT32 Version  = Rd32 (mRegs.Core, SPMI_REG_VERSION);
  mApidCount      = (UINT16)(Rd32 (mRegs.Core, SPMI_REG_FEATURES) & SPMI_FEATURES_MASK);

  DEBUG ((
    DEBUG_ERROR,
    "K60BTN: arbiter version=%08x apid_count=%u (v7 layout assumed)\n",
    Version,
    mApidCount
    ));

  //
  // Full EE0-owned peripheral dump (appears in the log once).
  //
  EnumeratePeripherals ();
  LocateButtons ();

  if ((mPonApid == SPMI_NO_APID) || (mGpioApid == SPMI_NO_APID)) {
    DEBUG ((
      DEBUG_ERROR,
      "K60BTN RESULT: NOT LOCATED (pon=%x gpio=%x)\n",
      mPonApid,
      mGpioApid
      ));

    /* freeze 30 s so the dump above is photographable */
    for (volatile UINT64 Pause = 0; Pause < 25000000000ULL; Pause++) {
    }

    return EFI_NOT_FOUND;
  }

  DEBUG ((
    DEBUG_ERROR,
    "K60BTN RESULT: located ponapid=%u gpioapid=%u\n",
    mPonApid,
    mGpioApid
    ));

  //
  // Install SimpleTextInput and start polling (10 ms).
  //
  mSimpleTextIn.Reset         = KbdReset;
  mSimpleTextIn.ReadKeyStroke = KbdReadKeyStroke;
  mSimpleTextIn.WaitForKey    = NULL;

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  PollButtons,
                  NULL,
                  &mPollEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mSimpleTextIn.WaitForKey = mPollEvent;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mTextInHandle,
                  &gEfiSimpleTextInProtocolGuid,
                  &mSimpleTextIn,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->SetTimer (mPollEvent, TimerPeriodic, 100000); /* 10 ms */

  DEBUG ((
    DEBUG_ERROR,
    "K60BTN: buttons driver ready (pon apid=%u gpio apid=%u)\n",
    mPonApid,
    mGpioApid
    ));

  return Status;
}
