/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/generic/reboot.c
 * PURPOSE:         Reboot functions
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 *                  Eric Kohl
 */

/* INCLUDES ******************************************************************/

#include <hal.h>

#define NDEBUG
#include <debug.h>

/* PRIVATE FUNCTIONS *********************************************************/

/* How long each reset method gets to take effect before the next is tried. */
#define HALP_RESET_SETTLE_US        100000
/* How long to wait for the keyboard controller to accept a command. */
#define HALP_KBC_READY_TIMEOUT_US   100000

static VOID
HalpWriteResetCommand(VOID)
{
    ULONG Waited;

    /* Wait (bounded) until the 8042 input buffer is empty, so the command
     * is not dropped; on a machine without an 8042 the port reads 0xFF and
     * the wait simply times out. */
    for (Waited = 0; Waited < HALP_KBC_READY_TIMEOUT_US; Waited += 10)
    {
        if (!(READ_PORT_UCHAR((PUCHAR)0x64) & 0x02))
            break;
        KeStallExecutionProcessor(10);
    }

    /* Generate RESET signal via keyboard controller */
    DPRINT1("HAL: reboot: keyboard controller reset\n");
    WRITE_PORT_UCHAR((PUCHAR)0x64, 0xFE);
    KeStallExecutionProcessor(HALP_RESET_SETTLE_US);
}

/*
 * The PCI reset control register at I/O port 0xCF9 (Intel chipsets and
 * their emulations): bit 1 selects a hard reset, setting bit 2 starts it.
 * A byte access does not touch the dword CONFIG_ADDRESS register at 0xCF8.
 */
static VOID
HalpResetControlRegisterReset(VOID)
{
    DPRINT1("HAL: reboot: reset control register 0xCF9\n");
    WRITE_PORT_UCHAR((PUCHAR)0xCF9, 0x02);
    KeStallExecutionProcessor(10);
    WRITE_PORT_UCHAR((PUCHAR)0xCF9, 0x06);
    KeStallExecutionProcessor(HALP_RESET_SETTLE_US);
}

DECLSPEC_NORETURN
VOID
HalpReboot(VOID)
{
    PHYSICAL_ADDRESS PhysicalAddress;
    UCHAR Data;
    PVOID ZeroPageMapping;

    /*
     * Try the reset methods in turn, each with time to take effect: the ACPI
     * reset register when the FADT declares one, then the PCI reset control
     * register, then the keyboard controller. Modern machines need not have
     * an 8042 keyboard controller.
     */
#ifndef _MINIHAL_
    DPRINT1("HAL: reboot: ACPI reset register\n");
    HalpAcpiReset();
#endif
    HalpResetControlRegisterReset();

    /* Map the first physical page */
    PhysicalAddress.QuadPart = 0;
    ZeroPageMapping = HalpMapPhysicalMemory64(PhysicalAddress, 1);

    /* Enable warm reboot (a failed mapping must not fault this late) */
    if (ZeroPageMapping)
        ((PUSHORT)ZeroPageMapping)[0x239] = 0x1234;

    /* Lock CMOS Access (and disable interrupts) */
    HalpAcquireCmosSpinLock();

    /* Setup control register B */
    WRITE_PORT_UCHAR((PUCHAR)0x70, 0x0B);
    KeStallExecutionProcessor(1);

    /* Read periodic register and clear the interrupt enable */
    Data = READ_PORT_UCHAR((PUCHAR)0x71);
    WRITE_PORT_UCHAR((PUCHAR)0x71, Data & ~0x40);
    KeStallExecutionProcessor(1);

    /* Setup control register A */
    WRITE_PORT_UCHAR((PUCHAR)0x70, 0x0A);
    KeStallExecutionProcessor(1);

    /* Read divider rate and reset it */
    Data = READ_PORT_UCHAR((PUCHAR)0x71);
    WRITE_PORT_UCHAR((PUCHAR)0x71, (Data & ~0x9) | 0x06);
    KeStallExecutionProcessor(1);

    /* Reset neutral CMOS address */
    WRITE_PORT_UCHAR((PUCHAR)0x70, 0x15);
    KeStallExecutionProcessor(1);

    /* Flush write buffers and send the reset command */
    KeFlushWriteBuffer();
    HalpWriteResetCommand();

    /* Every method failed: halt the CPU */
    DPRINT1("HAL: reboot: no reset method worked, halting\n");
    for (;;) __halt();
    UNREACHABLE;
}

/* PUBLIC FUNCTIONS **********************************************************/

#ifndef _MINIHAL_
/*
 * @implemented
 */
VOID
NTAPI
HalReturnToFirmware(
    _In_ FIRMWARE_REENTRY Action)
{
    /* Check what kind of action this is */
    switch (Action)
    {
        /* ACPI power-off is performed by the system power driver. If it
         * returned, halt here; a failed power-off must not become a reboot. */
        case HalHaltRoutine:
        case HalPowerDownRoutine:
            _disable();
            for (;;) __halt();

        case HalRestartRoutine:
        case HalRebootRoutine:
        {
            /*
             * Acquire the display. This runs the display driver's reset
             * routine (HwVidResetHw), which must run before a soft reboot so
             * the adapter is left in a state the firmware can start from.
             */
            DPRINT1("HAL: reboot: taking the display back (IRQL %u, interrupts %s)\n",
                    KeGetCurrentIrql(), (__readeflags() & 0x200) ? "on" : "off");
            InbvAcquireDisplayOwnership();
            DPRINT1("HAL: reboot: display taken back\n");

            /* Call the internal reboot function */
            HalpReboot();
        }

        /* Anything else */
        default:
        {
            /* Print message and break */
            DbgPrint("HalReturnToFirmware called!\n");
            DbgBreakPoint();
        }
    }
}
#endif // _MINIHAL_

/* EOF */
