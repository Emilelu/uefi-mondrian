/** @file
  K60ButtonsDxe.c v3 — auto-detecting SPMI arbiter version.

  boot36 dump evidence: v2-style map table (core+0x900) reads all-zero with
  4 junk entries at core+0x1000, and every OBS transaction times out — the
  arbiter is NOT the v2 layout sweet uses (SM7150). K60 is SM8475; the Linux
  driver (drivers/spmi/spmi-pmic-arb.c) auto-detects the version from
  core+0x0 and picks per-version layouts. This driver does the same:

    version >= 0x70000000 -> v7: map core+0x2000+4n, obs 0x8000*ee+0x20*apid,
                                   owner cnfg+4n
    version >= 0x50000000 -> v5: map core+0x900+4n, obs 0x10000*ee+0x80*apid,
                                   owner cnfg+0x700+4n
    otherwise             -> v2: obs 0x1000*ee+0x8000*apid, owner cnfg+0x700+4n

  Buttons (Qualcomm standard):
    Power       = PON periph INT_RT_STS (reg 0x10) bit0 (pwrkey)
    Volume down = PON periph INT_RT_STS bit1 (resin)
    Volume up   = PMIC GPIO periph INT_RT_STS bit0 (active low)

  Diagnosis output: version + apid_count + a probe of the first 8 map words
  at BOTH candidate map locations, then the family-filtered peripheral dump
  (PON = PPID family 0x12, GPIO = PPID family 0xC0..0xCF), 2 s hold per line.

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
// (cnfg 0xc42d000, core 0xc400000, obsrvr 0xc440000, chnls 0xc500000)
//
#define SPMI_CORE_PA   0x0C400000u
#define SPMI_OBS_PA    0x0C440000u
#define SPMI_CNFG_PA   0x0C42D000u
#define SPMI_CHNLS_PA  0x0C500000u

#define SPMI_REG_VERSION    0x0000u
#define SPMI_REG_FEATURES   0x0004u
#define SPMI_FEATURES_MASK  0x7FFu
#define SPMI_NO_APID        0xFFFFu

#define PON_REG_STATUS   0x10u         /* INT_RT_STS: bit0 pwrkey, bit1 resin */
#define GPIO_REG_STATUS  0x10u         /* INT_RT_STS bit0 = pin (active low) */

typedef enum {
  ARB_V2,
  ARB_V5,
  ARB_V7,
} ARB_VERSION;

typedef struct {
  UINT32 Core;
  UINT32 Obs;
  UINT32 Cnfg;
  UINT32 Chnls;
  ARB_VERSION Ver;
  UINT32 MapBase;      /* core-relative map table offset */
  UINT32 ObsStride;    /* per-apid OBS channel stride    */
  UINT32 OwnerBase;    /* cnfg-relative owner table offset */
  UINT16 ApidCount;
} SPMI_CTX;

STATIC SPMI_CTX                       mCtx;
STATIC UINT16                         mPonApid  = SPMI_NO_APID;
STATIC UINT16                         mGpioApid = SPMI_NO_APID;
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

/* One-byte EXT_READL through the observer channel (offset per version). */
STATIC
EFI_STATUS
SpmiRead8 (
  IN  UINT16 Apid,
  IN  UINT8  Addr,
  OUT UINT8  *Out
  )
{
  UINT32 Off = mCtx.ObsStride * Apid;
  UINT32 Cmd = (1u << 27) | ((UINT32)Addr << 4);
  UINT32 St;
  UINTN  Spin;

  Wr32 (mCtx.Obs, Off, Cmd);

  for (Spin = 0; Spin < 1000; Spin++) {
    St = Rd32 (mCtx.Obs, Off + 0x08u);
    if (St & 1u) {
      if (St & 0x0eu) {
        return EFI_DEVICE_ERROR;
      }

      *Out = (UINT8)Rd32 (mCtx.Obs, Off + 0x18u);
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

/* PPID -> APID via the version-appropriate map table. */
STATIC
UINT16
FindApid (
  IN UINT16 Ppid
  )
{
  UINT16 Found = SPMI_NO_APID;

  for (UINT32 i = 0; i < mCtx.ApidCount; i++) {
    UINT32 Map = Rd32 (mCtx.Core, mCtx.MapBase + 4u * i);

    if ((Map == 0) || (((Map >> 8) & 0xFFFu) != Ppid)) {
      continue;
    }

    UINT32 Owner = Rd32 (mCtx.Cnfg, mCtx.OwnerBase + 4u * i) & 7u;
    if ((Found == SPMI_NO_APID) || (Owner == 0)) {
      Found = (UINT16)i;
    }
  }

  return Found;
}

STATIC
VOID
Hold2s (
  VOID
  )
{
  for (volatile UINT64 Hold = 0; Hold < 5000000000ULL; Hold++) {
  }
}

/**
  Dump the first 8 map words at both candidate map locations — ground truth
  for which table is real, independent of the version guess.
**/
STATIC
VOID
DumpMapProbes (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "K60BTN MAP@0x900: 900:%08x 904:%08x 908:%08x 90c:%08x\n",
          Rd32 (mCtx.Core, 0x900u), Rd32 (mCtx.Core, 0x904u),
          Rd32 (mCtx.Core, 0x908u), Rd32 (mCtx.Core, 0x90cu)));
  DEBUG ((DEBUG_ERROR, "K60BTN MAP@0x2000: 2000:%08x 2004:%08x 2008:%08x 200c:%08x\n",
          Rd32 (mCtx.Core, 0x2000u), Rd32 (mCtx.Core, 0x2004u),
          Rd32 (mCtx.Core, 0x2008u), Rd32 (mCtx.Core, 0x200cu)));
  Hold2s ();
}

/**
  Enumerate EE0-owned APIDs in the button-relevant families:
    PON  = PPID low byte 0x12       (periph 0x12xx)
    GPIO = PPID low byte 0xC0..0xCF (periph 0xCx00)
  Each match prints with a 2 s hold so it is photographable.
**/
STATIC
VOID
EnumeratePeripherals (
  VOID
  )
{
  for (UINT32 i = 0; i < mCtx.ApidCount; i++) {
    UINT32 Map = Rd32 (mCtx.Core, mCtx.MapBase + 4u * i);

    if (Map == 0) {
      continue;
    }

    UINT32 Owner = Rd32 (mCtx.Cnfg, mCtx.OwnerBase + 4u * i) & 7u;
    if (Owner != 0) {
      continue;
    }

    UINT16 Ppid   = (UINT16)((Map >> 8) & 0xFFFu);
    UINT16 Family = Ppid & 0xFFu;
    BOOLEAN IsPon  = (Family == 0x12u);
    BOOLEAN IsGpio = ((Family >= 0xC0u) && (Family <= 0xCFu));

    if (!IsPon && !IsGpio) {
      continue;
    }

    UINT16 Apid = (UINT16)i;
    UINT8  Sub  = 0;
    UINT8  Type = 0;

    EFI_STATUS S2 = SpmiRead8 (Apid, 0x104u, &Type);
    EFI_STATUS S1 = SpmiRead8 (Apid, 0x105u, &Sub);

    DEBUG ((
      DEBUG_ERROR,
      "K60BTN %a: apid=%3u ppid=%03x type=%02x subtype=%02x (%r/%r)\n",
      IsPon ? "PON " : "GPIO",
      Apid,
      Ppid,
      Type,
      Sub,
      S2,
      S1
      ));
    Hold2s ();
  }
}

/**
  Locate the PON peripheral (PPID 0x012) and the volume-up GPIO peripheral
  (PMK8350 gpio6 @c500 -> PPID 0x0C5, plus fallbacks).
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

  mCtx.Core  = SPMI_CORE_PA;
  mCtx.Obs   = SPMI_OBS_PA;
  mCtx.Cnfg  = SPMI_CNFG_PA;
  mCtx.Chnls = SPMI_CHNLS_PA;

  //
  // Auto-detect the arbiter version exactly like the Linux driver does.
  //
  UINT32 Version = Rd32 (mCtx.Core, SPMI_REG_VERSION);

  if (Version >= 0x70000000u) {
    mCtx.Ver       = ARB_V7;
    mCtx.MapBase   = 0x2000u;
    mCtx.ObsStride = 0x20u;
    mCtx.OwnerBase = 0u;
  } else if (Version >= 0x50000000u) {
    mCtx.Ver       = ARB_V5;
    mCtx.MapBase   = 0x900u;
    mCtx.ObsStride = 0x80u;
    mCtx.OwnerBase = 0x700u;
  } else {
    mCtx.Ver       = ARB_V2;
    mCtx.MapBase   = 0x900u;
    mCtx.ObsStride = 0x8000u;
    mCtx.OwnerBase = 0x700u;
  }

  mCtx.ApidCount = (UINT16)(Rd32 (mCtx.Core, SPMI_REG_FEATURES) & SPMI_FEATURES_MASK);

  DEBUG ((
    DEBUG_ERROR,
    "K60BTN: arbiter version=%08x apid_count=%u map@%x obsStride=%x owner@cnfg+%x\n",
    Version,
    mCtx.ApidCount,
    mCtx.MapBase,
    mCtx.ObsStride,
    mCtx.OwnerBase
    ));
  Hold2s ();

  DumpMapProbes ();

  EnumeratePeripherals ();
  LocateButtons ();

  if ((mPonApid == SPMI_NO_APID) || (mGpioApid == SPMI_NO_APID)) {
    DEBUG ((
      DEBUG_ERROR,
      "K60BTN RESULT: NOT LOCATED (pon=%x gpio=%x)\n",
      mPonApid,
      mGpioApid
      ));
    Hold2s ();
    Hold2s ();
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
