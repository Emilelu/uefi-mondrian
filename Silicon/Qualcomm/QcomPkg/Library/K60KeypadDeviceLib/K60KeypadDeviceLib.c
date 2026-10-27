/** @file
  K60 (SM8475 / Redmi K60) KeypadDeviceLib — SPMI PMIC button sampling.

  Replaces the retired K60ButtonsDxe (v7) with the official Silicium
  KeypadDevice chain:

    KeypadDeviceDxe (Depex TRUE; publishes KEYPAD_DEVICE_PROTOCOL)
      -> KeypadDxe (driver binding; installs SimpleTextIn/Ex on the
         KeypadDevicePath handle)
      -> ConPlatform/ConSplitter adopt the handle -> UniXNU menu ConIn.

  Key production is dual-channel inside KeypadDxe and needs no timer
  dependency here:
    a) its own 20 ms periodic timer calls KeypadTimerHandler -> GetKeys;
    b) KeypadWaitForKey (the WaitForKey notify) ALSO calls
       KeypadTimerHandler (KeypadTextIn.c:302), so the menu's
       WaitForEvent cascade samples on every pass (the v6-proven path).

  Hardware facts carried over from K60ButtonsDxe v7 (boot39/40 verified):
    - SPMI arbiter: core 0xC400000, observer 0xC440000 (live FDT)
      map table @ core+0x2000, apid_count 758, obs stride 0x20 (ee=0)
    - one-byte read: cmd = (opc<<27)|((reg&0xFF)<<4)|bc, bc=len-1;
      opcode 15 (PMIC_ARB_OP_READ) works live; fallback EXT_READL(1);
      obs channel: cmd @+0x00, status @+0x08 (bit0 DONE, 0x0E error),
      rdata @+0x18
    - PON  = pmk8350@0 pon@1300        -> PPID 0x013 (sid 0)
      GPIO = pm8350@1 pinctrl@8800(+5) -> PPID 0x18D (sid 1)
    - buttons (live DT dtbo0): Power = PON INT_RT_STS(0x10) bit7 (pwrkey IRQ7)
                               VolDn = PON INT_RT_STS(0x10) bit6 (resin IRQ6)
                               VolUp = pm8350 gpio6 RT_STS bit0, ACTIVE LOW
                               (DT flags=1; idle reads 0x01)
    - RT_STS bit index == periph IRQ number (spmi-pmic-arb qpnpint rule)
    - boot40 VERIFY ground truth: pon rtsts=0x10, gpio rtsts=0x01

  Auto-reset safety net (hands-free test cycling): if no electrical key
  activity is ever observed within 60 s of GetKeys polling, write the
  Xiaomi "bootloader" restart reason 0x77665500 to IMEM 0x146AA65C
  (live DT qcom,msm-imem@146aa000 restart_reason@65c; mapped by
  "IMEM Cookie Base" AddDev/NS_DEVICE) and issue a PSCI cold reset
  (EfiResetCold -> SYSTEM_RESET; EfiResetWarm's SYSTEM_RESET2 path
  returns on this platform and aborts in ResetSystemRuntimeDxe).
  ABL re-enters fastboot by itself and the host watcher boots the next
  build. The keypad starts polling during DXE, about 20 s before the
  UniXNU menu appears; 60 s leaves roughly 40 s of menu time. If the
  reset returns, the autoreset is disabled instead of halting, so the
  menu stays usable for logs/QR capture.

  Zero boot-time holds: every probe is a plain DEBUG line (photo-
  verifiable without delay), no 2 s MAP dumps, no heartbeat spam.
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SimpleTextInEx.h>

//
// SPMI PMIC arbiter (live FDT).
//
#define SPMI_CORE_PA    0x0C400000u
#define SPMI_OBS_PA     0x0C440000u

#define SPMI_MAP_BASE   0x2000u     /* core-relative map table offset */
#define SPMI_APID_COUNT 758u        /* boot39: version 70000000        */
#define SPMI_OBS_STRIDE 0x20u       /* per-apid observer channel stride */
#define SPMI_NO_APID    0xFFFFu

/* Arbiter command opcodes (drivers/spmi/spmi-pmic-arb.c enum). */
#define ARB_OP_EXT_READL 1u
#define ARB_OP_READ      15u

#define PON_REG_STATUS   0x10u      /* INT_RT_STS */
#define GPIO_REG_STATUS  0x10u

/* Live-DTB peripheral PPIDs (sid<<8 | periph). */
#define PPID_PON  0x013u            /* pmk8350@0 pon@1300   */
#define PPID_GPIO 0x18Du            /* pm8350@1 gpio block  */

/* IMEM restart reason (live DT: qcom,msm-imem@146aa000 restart_reason@65c).
   Xiaomi sm8450-family kernel writes 0x77665500 for cmd "bootloader". */
#define IMEM_RESTART_REASON_PA    0x146AA65Cu
#define RESTART_REASON_BOOTLOADER 0x77665500u

//
// Auto-reset window. KeypadDxe polls at 50 Hz; warn at 50 s, reset at
// 60 s. The keypad starts polling during DXE, roughly 20 s before the
// menu appears, so this leaves about 40 s of visible menu time. Any key
// press cancels the countdown permanently (mKeyEverSeen).
//
#define AUTORESET_WARN_TICKS 2500u  /* 50 s */
#define AUTORESET_TICKS      3000u  /* 60 s */

typedef struct {
  BOOLEAN Baseline;  /* first read consumed            */
  BOOLEAN Down;      /* logical pressed state          */
} BTN_CTX;

/* 0 = power, 1 = volume up, 2 = volume down. */
STATIC BTN_CTX  mBtn[3];
STATIC UINT16   mPonApid     = SPMI_NO_APID;
STATIC UINT16   mGpioApid    = SPMI_NO_APID;
STATIC UINT32   mTicks       = 0;
STATIC BOOLEAN  mKeyEverSeen = FALSE;
STATIC BOOLEAN  mAutoResetDone = FALSE;
STATIC BOOLEAN  mInitDone    = FALSE;

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

/**
  One-byte SPMI read through the observer channel. Kernel fmt_cmd_v2:
  cmd = (opc<<27) | ((addr&0xFF)<<4) | bc, bc = len-1 = 0 for one byte.
  Tries PMIC_ARB_OP_READ(15) first, falls back to EXT_READL(1).
**/
STATIC
EFI_STATUS
SpmiRead8Opc (
  IN  UINT16 Apid,
  IN  UINT8  Reg,
  IN  UINT8  Opc,
  OUT UINT8  *Out
  )
{
  UINT32 Off = SPMI_OBS_STRIDE * Apid;
  UINT32 Cmd = ((UINT32)Opc << 27) | (((UINT32)Reg & 0xFFu) << 4) | 0x0u;
  UINT32 St;
  UINTN  Spin;

  Wr32 (SPMI_OBS_PA, Off, Cmd);

  for (Spin = 0; Spin < 1000; Spin++) {
    St = Rd32 (SPMI_OBS_PA, Off + 0x08u);

    if (St & 1u) {
      if (St & 0x0eu) {
        return EFI_DEVICE_ERROR;
      }

      *Out = (UINT8)Rd32 (SPMI_OBS_PA, Off + 0x18u);
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SpmiRead8 (
  IN  UINT16 Apid,
  IN  UINT8  Reg,
  OUT UINT8  *Out
  )
{
  EFI_STATUS Status = SpmiRead8Opc (Apid, Reg, ARB_OP_READ, Out);

  if (!EFI_ERROR (Status)) {
    return Status;
  }

  return SpmiRead8Opc (Apid, Reg, ARB_OP_EXT_READL, Out);
}

/* PPID -> APID via the v7 map table (ppid = (word>>8) & 0xFFF). */
STATIC
UINT16
FindApid (
  IN UINT16 Ppid
  )
{
  UINT32 i;

  for (i = 0; i < SPMI_APID_COUNT; i++) {
    UINT32 Map = Rd32 (SPMI_CORE_PA, SPMI_MAP_BASE + 4u * i);

    if ((Map != 0) && (((Map >> 8) & 0xFFFu) == Ppid)) {
      return (UINT16)i;
    }
  }

  return SPMI_NO_APID;
}

/**
  Safety net: write the Xiaomi "bootloader" restart reason and issue a
  PSCI cold reset so the phone walks back into fastboot on its own.

  EfiResetWarm is deliberately not used: Mu's ArmPsciResetSystemLib
  probes SYSTEM_RESET2 first and, when that call returns, does not fall
  back to SYSTEM_RESET. ResetSystemRuntimeDxe then hits
  ResetSystem.c:334 ASSERT(FALSE), and the caller used to halt in wfi.
  EfiResetCold maps directly to SYSTEM_RESET (the Linux reboot path).
**/
STATIC
VOID
AutoResetToFastboot (
  VOID
  )
{
  if (mAutoResetDone) {
    return;
  }

  mAutoResetDone = TRUE;

  DEBUG ((DEBUG_ERROR, "K60KEY AUTO: cold reset to fastboot (reason=77665500)\n"));

  Wr32 (IMEM_RESTART_REASON_PA, 0, RESTART_REASON_BOOTLOADER);

  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);

  DEBUG ((
    DEBUG_ERROR,
    "K60KEY AUTO: ResetSystem returned - autoreset disabled, menu stays alive\n"
    ));
}

/**
  Publish a press edge into the KeypadDxe key queue. The menu consumes
  ScanCode: Up/Down move the cursor, SCAN_NULL+0x0D acts as Enter.
**/
STATIC
VOID
PushKey (
  IN KEYPAD_RETURN_API *Api,
  IN UINT16             ScanCode,
  IN UINT16             UnicodeChar
  )
{
  EFI_KEY_DATA KeyData;

  KeyData.Key.ScanCode               = ScanCode;
  KeyData.Key.UnicodeChar            = UnicodeChar;
  KeyData.KeyState.KeyShiftState     = 0;
  KeyData.KeyState.KeyToggleState    = 0;

  Api->PushEfikeyBufTail (Api, &KeyData);
}

/**
  KeypadDevice reset: locate the PON and GPIO APIDs once and run one
  live verification read of each status register. No holds. Always
  returns EFI_SUCCESS (a failed verify is reported via DEBUG and the
  poll loop stays armed for the auto-reset safety net).
**/
EFI_STATUS
EFIAPI
KeypadDeviceReset (
  KEYPAD_DEVICE_PROTOCOL *This
  )
{
  UINT8 Pon  = 0;
  UINT8 Gpio = 0;

  if (mInitDone) {
    return EFI_SUCCESS;
  }

  mPonApid  = FindApid (PPID_PON);
  mGpioApid = FindApid (PPID_GPIO);

  DEBUG ((
    DEBUG_ERROR,
    "K60KEY: pon apid=%u gpio apid=%u\n",
    mPonApid,
    mGpioApid
    ));

  if ((mPonApid == SPMI_NO_APID) || (mGpioApid == SPMI_NO_APID)) {
    DEBUG ((DEBUG_ERROR, "K60KEY: apid locate failed\n"));
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (SpmiRead8 (mPonApid, PON_REG_STATUS, &Pon)) ||
      EFI_ERROR (SpmiRead8 (mGpioApid, GPIO_REG_STATUS, &Gpio)))
  {
    DEBUG ((DEBUG_ERROR, "K60KEY: verify read failed\n"));
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_ERROR,
    "K60KEY VERIFY: pon=%02x gpio=%02x (boot40 ground truth: 10/01)\n",
    Pon,
    Gpio
    ));

  mInitDone = TRUE;

  return EFI_SUCCESS;
}

/**
  Sample pass, driven at ~50 Hz by KeypadDxe (timer channel and/or the
  menu WaitForEvent cascade through KeypadWaitForKey). Reads both RT_STS
  registers, latches press edges into the key queue, and arms the
  auto-reset countdown while no key activity has ever been seen.
**/
EFI_STATUS
KeypadDeviceGetKeys (
  KEYPAD_DEVICE_PROTOCOL *This,
  KEYPAD_RETURN_API      *KeypadReturnApi,
  UINT64                  Delta
  )
{
  UINT8   Pon  = 0;
  UINT8   Gpio = 0;
  UINT8   Rtsts[3];
  BOOLEAN Down[3];
  UINTN   k;

  if (!mInitDone) {
    /* Sampling before a successful reset: fall through to the
       countdown so a dead SPMI still walks back to fastboot. */
    if (!mAutoResetDone) {
      mTicks++;

      if (mTicks >= AUTORESET_TICKS) {
        AutoResetToFastboot ();
      }
    }

    return EFI_SUCCESS;
  }

  if (EFI_ERROR (SpmiRead8 (mPonApid, PON_REG_STATUS, &Pon))) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (SpmiRead8 (mGpioApid, GPIO_REG_STATUS, &Gpio))) {
    return EFI_SUCCESS;
  }

  Rtsts[0] = Pon;   /* power     */
  Rtsts[1] = Gpio;  /* volume up */
  Rtsts[2] = Pon;   /* volume down */

  /* Live DT: PON pwrkey=IRQ7 -> bit7, resin=IRQ6 -> bit6. pm8350 gpio6
     is active low (DT flags=1): idle 0x01, pressed clears bit0. */
  Down[0] = (Pon  & 0x80u) != 0;
  Down[1] = (Gpio & 1u) == 0;
  Down[2] = (Pon  & 0x40u) != 0;

  for (k = 0; k < 3; k++) {
    BTN_CTX *B = &mBtn[k];

    if (!B->Baseline) {
      B->Baseline = TRUE;
      continue;
    }

    if (Down[k] && !B->Down) {
      B->Down = TRUE;
      mKeyEverSeen = TRUE;

      DEBUG ((
        DEBUG_ERROR,
        "K60KEY: btn%u down (rtsts=%02x)\n",
        k,
        Rtsts[k]
        ));

      PushKey (
        KeypadReturnApi,
        (k == 1) ? SCAN_UP : ((k == 2) ? SCAN_DOWN : SCAN_NULL),
        (k == 0) ? 0x0D : 0x00
        );
    } else if (!Down[k]) {
      B->Down = FALSE;
    }
  }

  if (!mKeyEverSeen && !mAutoResetDone) {
    mTicks++;

    if (mTicks == AUTORESET_WARN_TICKS) {
      DEBUG ((
        DEBUG_ERROR,
        "K60KEY AUTO: no key activity - cold reset to fastboot in 10s\n"
        ));
    } else if (mTicks >= AUTORESET_TICKS) {
      AutoResetToFastboot ();
    }
  }

  return EFI_SUCCESS;
}