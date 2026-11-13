// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Illusions-DS emulator is offered as-is, without any warranty. Please see readme.md
//
// This file is our bridge between the Z80 CPU core and the rest of the system.
// IllusionDS currently supports the CZ80 CPU core.
// =====================================================================================
#include <nds.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Z80_interface.h"
#include "../../IllusionDS.h"
#include "../../MSX_generic.h"
#include "../../fdc.h"
#include "../../printf.h"

u8 FMPAC_SRAM_in_view   = 0; // Is the FMPAC SRAM in view?
u8 FMPAC_Key1           = 0; // FM-PAC SRAM requires a special key write sequence to open up SRAM access

// Some mapper storage for the really BIG mapper carts like ASCII16-X and NEO8/16 which can store more data
u8 ascii16xFlashCmdState[3] = {0};
u8 ascii16xFlashReadMode[3] = {0};
u8 ascii16xCfiBackup[3][ASCII16X_CFI_SIZE];

// -----------------------------------------------------------------------------------------------
// All memory fetches run through this except OP codes which are read directly from memory
// for maximum speed (an Op Code fetch should never result in accessing special memory registers).
// -----------------------------------------------------------------------------------------------
ITCM_CODE u8 cpu_readmem16(u16 address)
{
    if ((special_memory_access & SPEC_MEM_DISK_CONTROLLER) && (address >= 0x7FF8) && (address <= 0x7FFF))
    {
        if (address == 0x7FFF) return fdc_read(4);
        else return fdc_read(address & 7);
    }
    else if ((special_memory_access & (SPEC_MEM_SUBSLOT0_ACTIVE | SPEC_MEM_SUBSLOT_ACTIVE)) && (address == 0xFFFF)) // Subslot check... only when page 3 is mapped to an expanded slot
    {
        if (special_memory_access & SPEC_MEM_SUBSLOT0_ACTIVE) 
            return (u8)~msx_subslot0; // Compliment is returned.
        else
            return (u8)~msx_subslot;  // Compliment is returned.
    }

    // Otherwise normal read - just index into the 8K memory block and fetch the byte...
    return (MemoryMap[address>>13][address]);
}



// -------------------------------------------------------------------------
// The ASCII 16K Mapper on steroids... has mirrors and can allow 4MB+ ROMs.
// -------------------------------------------------------------------------
static const u16 ascii16xAutoSelectData[16] =
{
    0x0001, 0x227E, 0x0000, 0xFF0A,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0x2210, 0x2200
};
static const u16 ascii16xCfiIdData[16] =
{
    0x0001, 0x227E, 0xFFFF, 0xFF0A,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0x2210, 0x2200
};

void __attribute__((noinline)) HandleAscii16X(u8 media_id, u16 address, u8 value)
{
    // ASCII-16X: bank number uses address bits A11-A8 plus the data byte.
    // The mapper mask limits this to the ROM size supported by this cartridge.
    // We support 4MB and 8MB here only...
    u32 block = ((address & 0x0F00) | value) & MyMedia[media_id].mapperMask;
    u32 msx_offset = block * MyMedia[media_id].blockSize;
    u8 *src = (u8*)ROM_Memory + msx_offset + (media_id ? ((MAX_CART_SIZE_KB / 2) * 1024) : 0);

    u32 cart_capacity = media_id ? ((MAX_CART_SIZE_KB / 2) * 1024) : (MAX_CART_SIZE_KB * 1024);
    u32 flash_size = MyMedia[media_id].filesize;
    if (flash_size > cart_capacity)
        flash_size = cart_capacity;
    if (flash_size > (8 * 1024 * 1024))
        flash_size = 8 * 1024 * 1024;
    u8 *flash_base = (u8*)ROM_Memory + (media_id ? ((MAX_CART_SIZE_KB / 2) * 1024) : 0);

    // Query modes temporarily replace the start of the flash image, allowing
    // ordinary memory reads to return the identification data directly.
    if (ascii16xFlashReadMode[media_id])
    {
        if (value == 0xF0)
        {
            memcpy(flash_base, ascii16xCfiBackup[media_id], ASCII16X_CFI_SIZE);
            ascii16xFlashReadMode[media_id] = 0;
            ascii16xFlashCmdState[media_id] = 0;
        }
    }
    else if ((value == 0xF0) && (ascii16xFlashCmdState[media_id] != 3))
    {
        ascii16xFlashCmdState[media_id] = 0;
    }
    else if ((value == 0x98) && ((address & 0x0FFF) == 0x00AA) &&
             (flash_size >= ASCII16X_CFI_SIZE) && (ascii16xFlashCmdState[media_id] != 3))
    {
        u8 cfi[ASCII16X_CFI_SIZE] = {0};
        u32 sector_count = (flash_size > 0x10000) ? ((flash_size - 0x10000) / 0x10000) : 0;
        u32 size_bytes = 1;
        u8 size_exponent = 0;

        while ((size_bytes < flash_size) && (size_exponent < 31))
        {
            size_bytes <<= 1;
            size_exponent++;
        }

        for (u32 i = 0; i < 16; i++)
        {
            u16 word = ascii16xCfiIdData[i];
            cfi[ASCII16X_CFI_BYTE(i)] = word;
            cfi[ASCII16X_CFI_BYTE(i) + 1] = word >> 8;
        }

        cfi[ASCII16X_CFI_BYTE(0x10)] = 'Q';
        cfi[ASCII16X_CFI_BYTE(0x11)] = 'R';
        cfi[ASCII16X_CFI_BYTE(0x12)] = 'Y';
        cfi[ASCII16X_CFI_BYTE(0x13)] = 0x02; // AMD/Fujitsu command set.
        cfi[ASCII16X_CFI_BYTE(0x15)] = 0x40; // Primary extended query table.
        cfi[ASCII16X_CFI_BYTE(0x1B)] = 0x27; // Vcc range: 2.7V to 3.6V.
        cfi[ASCII16X_CFI_BYTE(0x1C)] = 0x36;
        cfi[ASCII16X_CFI_BYTE(0x1F)] = 0x04; // Typical byte-program timeout: 16 us.
        cfi[ASCII16X_CFI_BYTE(0x20)] = 0x0C;
        cfi[ASCII16X_CFI_BYTE(0x23)] = 0x0A; // Typical sector-erase timeout: 1 s.
        cfi[ASCII16X_CFI_BYTE(0x24)] = 0x03;
        cfi[ASCII16X_CFI_BYTE(0x27)] = size_exponent;
        cfi[ASCII16X_CFI_BYTE(0x2C)] = (sector_count > 0) ? 2 : 1;
        cfi[ASCII16X_CFI_BYTE(0x2D)] = 7;    // Eight 8K boot sectors.
        cfi[ASCII16X_CFI_BYTE(0x2F)] = 0x20; // 8K in units of 256 bytes.

        if (sector_count > 0)
        {
            u32 large_sectors_minus_one = sector_count - 1;
            cfi[ASCII16X_CFI_BYTE(0x31)] = large_sectors_minus_one & 0xFF;
            cfi[ASCII16X_CFI_BYTE(0x32)] = (large_sectors_minus_one >> 8) & 0xFF;
            cfi[ASCII16X_CFI_BYTE(0x33)] = 0x00;
            cfi[ASCII16X_CFI_BYTE(0x34)] = 0x01; // 64K in units of 256 bytes.
        }

        cfi[ASCII16X_CFI_BYTE(0x40)] = 'P';
        cfi[ASCII16X_CFI_BYTE(0x41)] = 'R';
        cfi[ASCII16X_CFI_BYTE(0x42)] = 'I';
        cfi[ASCII16X_CFI_BYTE(0x43)] = '1';
        cfi[ASCII16X_CFI_BYTE(0x44)] = '3';

        memcpy(ascii16xCfiBackup[media_id], flash_base, ASCII16X_CFI_SIZE);
        memcpy(flash_base, cfi, ASCII16X_CFI_SIZE);
        ascii16xFlashReadMode[media_id] = 1;
        ascii16xFlashCmdState[media_id] = 0;
    }
    else
    {
        u16 command_address = ASCII16X_COMMAND_ADDRESS(address);
        u8 *mapped = MSXCartPtr[media_id][address >> 13];
        uintptr_t cart_start = (uintptr_t)flash_base;
        uintptr_t dest = (uintptr_t)mapped + (address & 0x1FFF);

        switch (ascii16xFlashCmdState[media_id])
        {
            case 0:
                if ((command_address == 0x0AAA) && (value == 0xAA))
                    ascii16xFlashCmdState[media_id] = 1;
                break;

            case 1:
                if ((command_address == 0x0554) && (value == 0x55))
                    ascii16xFlashCmdState[media_id] = 2;
                else
                    ascii16xFlashCmdState[media_id] = ((command_address == 0x0AAA) && (value == 0xAA)) ? 1 : 0;
                break;

            case 2:
                if ((command_address == 0x0AAA) && (value == 0x90))
                {
                    memcpy(ascii16xCfiBackup[media_id], flash_base, ASCII16X_CFI_SIZE);
                    for (u32 i = 0; i < ASCII16X_CFI_SIZE; i++)
                    {
                        u16 word = ascii16xAutoSelectData[(i >> 1) & 0x0F];
                        flash_base[i] = (i & 1) ? (word >> 8) : word;
                    }
                    ascii16xFlashReadMode[media_id] = 2;
                    ascii16xFlashCmdState[media_id] = 0;
                }
                else if ((command_address == 0x0AAA) && (value == 0xA0))
                    ascii16xFlashCmdState[media_id] = 3;
                else if ((command_address == 0x0AAA) && (value == 0x80))
                    ascii16xFlashCmdState[media_id] = 4;
                else
                    ascii16xFlashCmdState[media_id] = 0;
                break;

            case 3: // Program one byte.
                if ((mapped != NULL) && (dest >= cart_start) && (dest - cart_start < flash_size))
                {
                    u8 *flash_byte = (u8*)dest;
                    *flash_byte &= value;
                    flashDirtyBlocks[(dest - cart_start) / FLASH_DIRTY_BLOCK_SIZE] = 1;
                    flash_show_status = 4;
                }
                ascii16xFlashCmdState[media_id] = 0;
                break;

            case 4: // Erase setup: second unlock sequence.
                if ((command_address == 0x0AAA) && (value == 0xAA))
                    ascii16xFlashCmdState[media_id] = 5;
                else
                    ascii16xFlashCmdState[media_id] = 0;
                break;

            case 5:
                if ((command_address == 0x0554) && (value == 0x55))
                    ascii16xFlashCmdState[media_id] = 6;
                else
                    ascii16xFlashCmdState[media_id] = 0;
                break;

            case 6: // Sector erase.
                if ((value == 0x30) && (mapped != NULL) &&
                    (dest >= cart_start) && (dest - cart_start < flash_size))
                {
                    u32 offset = (u32)(dest - cart_start);
                    u32 sector_start = (offset < 0x10000)
                        ? (offset & ~0x1FFF)
                        : (0x10000 + ((offset - 0x10000) & ~0xFFFF));
                    u32 sector_size = (sector_start < 0x10000) ? 0x2000 : 0x10000;
                    if (sector_size > flash_size - sector_start)
                        sector_size = flash_size - sector_start;

                    memset(flash_base + sector_start, 0xFF, sector_size);
                    u32 first_dirty_block = sector_start / FLASH_DIRTY_BLOCK_SIZE;
                    u32 last_dirty_block = (sector_start + sector_size - 1) / FLASH_DIRTY_BLOCK_SIZE;
                    for (u32 dirty_block = first_dirty_block; dirty_block <= last_dirty_block; dirty_block++)
                    {
                        flashDirtyBlocks[dirty_block] = 1;
                    }
                    flash_show_status = 4;
                }
                ascii16xFlashCmdState[media_id] = 0;
                break;

            default:
                ascii16xFlashCmdState[media_id] = 0;
                break;
        }
    }

    // OpenMSX accepts writes when A13 is set within either 16 KB region.
    if ((address & 0x3FFF) >= 0x2000)
    {
        // A12 selects the bank register.
        if (((address >> 12) & 1) == 0)
        {
            // First 16 KB window: 4000-7FFF, mirrored at C000-FFFF.
            MSXCartPtr[media_id][2] = src;
            MSXCartPtr[media_id][3] = src + 0x2000;

            MSXCartPtr[media_id][6] = src;
            MSXCartPtr[media_id][7] = src + 0x2000;

            if (bCartInPage[1] == (media_id + 1))
            {
                MemoryMap[2] = MSXCartPtr[media_id][2] - 0x4000;
                MemoryMap[3] = MSXCartPtr[media_id][3] - 0x6000;
            }

            if (bCartInPage[3] == (media_id + 1))
            {
                MemoryMap[6] = MSXCartPtr[media_id][6] - 0xC000;
                MemoryMap[7] = MSXCartPtr[media_id][7] - 0xE000;
            }
        }

        #undef ASCII16X_CFI_BYTE
        #undef ASCII16X_COMMAND_ADDRESS
        else
        {
            // Second 16 KB window: 8000-BFFF, mirrored at 0000-3FFF.
            MSXCartPtr[media_id][4] = src;
            MSXCartPtr[media_id][5] = src + 0x2000;

            MSXCartPtr[media_id][0] = src;
            MSXCartPtr[media_id][1] = src + 0x2000;

            if (bCartInPage[2] == (media_id + 1))
            {
                MemoryMap[4] = MSXCartPtr[media_id][4] - 0x8000;
                MemoryMap[5] = MSXCartPtr[media_id][5] - 0xA000;
            }

            if (bCartInPage[0] == (media_id + 1))
            {
                MemoryMap[0] = MSXCartPtr[media_id][0] - 0x0000;
                MemoryMap[1] = MSXCartPtr[media_id][1] - 0x2000;
            }
        }
    }
}

// -------------------------------------------------------------------
// The FM-PAC presents 8K of SRAM for a game to use to save progress.
// This handler is called when any byte in the 0x4000-0x5FFF range is
// written and will determine if/when the SRAM is available.
// -------------------------------------------------------------------
#define FMPAC_KEY1 0x4D
#define FMPAC_KEY2 0x69
void __attribute__((noinline)) HandleFMPAC(u8 media_id, u16 address, u8 value)
{
    if (address == 0x5FFE)
    {
        // Key register
        SRAM_Memory[0x1FFE] = value;

        if ((SRAM_Memory[0x1FFF] == 0x69) && (value == 0x4D))
        {
            FMPAC_SRAM_in_view = 1;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)SRAM_Memory;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        else
        {
            FMPAC_SRAM_in_view = 0;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)TURBO_FMPAC;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        return;
    }
    else if (address == 0x5FFF)
    {
        // Key register
        SRAM_Memory[0x1FFF] = value;

        if ((SRAM_Memory[0x1FFE] == 0x4D) && (value == 0x69))
        {
            FMPAC_SRAM_in_view = 1;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)SRAM_Memory;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        else
        {
            FMPAC_SRAM_in_view = 0;
            MSXCartPtr[MEDIA_CART2][2] = (u8*)TURBO_FMPAC;
            MemoryMap[2] = MSXCartPtr[MEDIA_CART2][2] - 0x4000;
        }
        return;
    }

    // Otherwise see if we are able to write into the SRAM
    if (FMPAC_SRAM_in_view && (address >= 0x4000) && (address < 0x5FFE))
    {
        SRAM_Memory[address & 0x1FFF] = value;
        sram_show_status = 5;
    }
}

// --------------------------------------------------------------------
// Handle writes to the Disk Controller (FDC) memory mapped registers.
// --------------------------------------------------------------------
void __attribute__((noinline)) HandleDiskControl(u16 address, u8 value)
{
    if (address <= 0x7FFB) fdc_write(address & 3, value);
    if (address == 0x7FFC) fdc_setSide((value & 1) ? 1:0);  // Side: [xxxxxxxS]
    if (address == 0x7FFD) fdc_setDrive((value & 1) ? 1:0); // Drive: [xxxxxxxD]
}

// ---------------------------------------------------------------------
// Write memory handles both normal writes and bankswitched since
// write is much less common than reads... All of the MSX cart mappers
// are vectored here so we can handle all the bank switching, etc.
// ---------------------------------------------------------------------
ITCM_CODE void cpu_writemem16(u16 address, u8 value)
{
    if ((special_memory_access & (SPEC_MEM_SUBSLOT0_ACTIVE | SPEC_MEM_SUBSLOT_ACTIVE)) && (address == 0xFFFF)) // Subslot check... only when page 3 is mapped to an expanded slot
    {
        if (special_memory_access & SPEC_MEM_SUBSLOT0_ACTIVE) msx_subslot0 = value;
        else msx_subslot = value;
        cpu_writeport_msx(0xA8, Port_PPI_A); // Enable the new map...
    }
    else if (bRAMInPage[address >> 14]) // RAM Exists in this slot... write it.
    {
        MemoryMap[address>>13][address]=value;
    }
    else if ((special_memory_access & SPEC_MEM_DISK_CONTROLLER) && (address >= 0x7FF8) && (address <= 0x7FFF)) // Disk controller memory map...
    {
        HandleDiskControl(address, value);
    }
    else if (bCartInPage[address>>14]) // Is there a Cartridge mapped into the write address area?
    {
        u8 media_id = bCartInPage[address>>14] - 1;     // Is this MEDIA_CART1 or MEDIA_CART2?
        
        // ----------------------------------------------------------
        // Illusion City is ASCII16-X - plus FM-PAC in slot 2
        // ----------------------------------------------------------
        if (media_id == MEDIA_CART1)
        {
            HandleAscii16X(media_id, address, value);
        }
        else // CART2 is FM-PAC with SRAM
        {
            HandleFMPAC(media_id, address, value);
        }
    }
    else
    {
        //debug_printf("Mem Wr %04X=%02X [A5=%02X]\n", address, value, Port_PPI_A);
    }
}

// -----------------------------------------------------------------
// Reset a few key variables needed for proper Z80 interface use...
// -----------------------------------------------------------------
void Z80_Interface_Reset(void)
{
    CPU.CycleDeficit      = 0;
    FMPAC_SRAM_in_view    = 0;
    FMPAC_Key1            = 0;
}

// -----------------------------------------------------------------
// Trap and report illegal opcodes to the IllusionDS debugger...
// -----------------------------------------------------------------
void Trap_Bad_Ops(char *prefix, byte I, word W)
{
    if (myGlobalConfig.debugger)
    {
        char tmp[32];
        sprintf(tmp, "ILLOP: %s %02X %04X", prefix, I, W);
        DSPrint(0,0,6, tmp);
    }
}

// End of file
