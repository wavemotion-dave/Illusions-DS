// =====================================================================================
// Copyright (c) 2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated
// readme files, with or without modification, are permitted in any medium without
// royalty provided this copyright notice is used and wavemotion-dave and
// Marat Fayzullin (fMSX core) are thanked profusely.
//
// The Illusions-DS emulator is offered as-is, without any warranty. Please see readme.md
// =====================================================================================
#include <nds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fat.h>
#include <dirent.h>

#include "IllusionDS.h"
#include "CRC32.h"
#include "cpu/z80/Z80_interface.h"
#include "MSX_generic.h"
#include "fdc.h"
#include "lzav.h"
#include "printf.h"

#define MSX_SAVE_VER   0x000F  // Change this if the basic format of the .SAV file changes. Invalidates older .sav files.

// -----------------------------------------------------------------------------------------------------
// Since the main MemoryMap[] can point to differt things (RAM, ROM, BIOS, etc) and since we can't rely
// on the memory being in the same spot on subsequent versions of the emulator... we need to save off
// the type and the offset so that we can patch it back together when we load back a saved state.
// -----------------------------------------------------------------------------------------------------
struct RomOffset
{
    u8   type;
    u32  offset;
};

struct RomOffset Offsets[8];

#define TYPE_ROM        0
#define TYPE_RAM        1
#define TYPE_FDC        2
#define TYPE_BIOS       3
#define TYPE_EBIOS      4
#define TYPE_FMPAC      5
#define TYPE_SRAM       6
#define TYPE_OTHER      9

static char szLoadFile[256];        // We build the filename out of the base filename and tack on .sav, .ee, etc.

/*********************************************************************************
 * Save the current state - save everything we need to a single .sav file.
 ********************************************************************************/

// --------------------------------------------------------------------------------------
// We use a 256K buffer on the back-end of the ROM_Memory to use for lzav compression.
// So we move that 256K block out to an unused VRAM area temporarily while we save/load.
// This saves us from having to allocate a large 256K buffer just for save/load use.
// --------------------------------------------------------------------------------------
void preserveCompressedMem(void)
{
    memcpy((u8*)0x06860000, COMPRESS_BUFFER, 256*1024);
}

void restoreCompressedMem(void)
{
    memcpy(COMPRESS_BUFFER, (u8*)0x06860000, 256*1024);
}

void msxSaveState(void)
{
}


/*********************************************************************************
 * Load the current state - read everything back from the .sav file.
 ********************************************************************************/
void msxLoadState(void)
{
}

// End of file
