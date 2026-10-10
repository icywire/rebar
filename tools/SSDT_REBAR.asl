/*
 * SSDT_REBAR.asl - gives the PCI root bridge a 64-bit MMIO window so a Resizable BAR (8 GB VRAM on the
 * Radeon Pro 5500M) can be placed above 4 GB. MacBook Pro 2019 (T2).
 *
 * Why: on this Mac the firmware leaves M64L (length of the 64-bit window) at 0. \_SB.PCI0._CRS
 * (dsdt.dsl:2163, DSDT) then zeroes its QWordMemory descriptor, and Windows sees only 0x80000000-0xFEAFFFFF
 * on the root (pnpdump.txt, ACPI\PNP0A08\0).
 *
 * How: _CRS builds that descriptor from M64B/M64L every time it runs (dsdt.dsl:2326-2338). M64B/M64L are
 * fields of the NVS region SANV (SaSsdt, ssdt5.dsl:141). \_SB.PCI0 has no _INI in the DSDT, so this file
 * adds one that fills M64B/M64L before the OS reads _CRS. No rename of _CRS is needed and nothing in
 * the DSDT/SaSsdt is replaced.
 *
 * This file only OFFERS the window. It does not resize any BAR: the Radeon's Resizable BAR capability
 * (ext. cap 0x15) and the bridge windows (Intel 1901, AMD 1478/1479) still have to be programmed by the loader.
 *
 * Build: iasl -p out/SSDT_REBAR tools/SSDT_REBAR.asl      (output goes to out/, not tools/)
 *
 * Reference machine: MacBook Pro 2019 (T2). Names below were confirmed only in the dump in acpi/.
 */
DefinitionBlock ("", "SSDT", 2, "T2GMUX", "REBAR", 0x00001000)
{
    External (_SB_.PCI0, DeviceObj)
    External (M64B, FieldUnitObj)       // 64-bit window base,   SaSsdt NVS
    External (M64L, FieldUnitObj)       // 64-bit window length, SaSsdt NVS

    Scope (\_SB.PCI0)
    {
        Method (_INI, 0, NotSerialized)
        {
            // Respect a window the firmware already set up.
            If (M64L == Zero)
            {
                M64B = 0x0000002000000000       // 128 GB: above DRAM, inside the 39-bit physical space
                M64L = 0x0000000400000000       // 16 GB: room for an 8 GB BAR, aligned on its own size
            }
        }
    }
}
