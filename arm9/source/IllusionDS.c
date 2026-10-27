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
#include <nds/fifomessages.h>

#include <ctype.h>
#include <fat.h>
#include <maxmod9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "CRC32.h"
#include "IllusionDS.h"
#include "MSX_generic.h"
#include "V9938.h"
#include "cpu/vdp9938/vdp9938.h"
#include "debug_ovl.h"
#include "fdc.h"
#include "mainmenu.h"
#include "minimenu.h"
#include "warning1.h"
#include "warning2.h"
#include "msx_kbd.h"
#include "msx_music.h"
#include "options.h"
#include "cpu/z80/Z80_interface.h"
#include "printf.h"
#include "screenshot.h"
#include "soundbank.h"
#include "soundbank_bin.h"

// ------------------------------------------------------------------
// These 16 debug registers plus indexes DX/DY are the lifeline for
// development of this emulator. We can use these to track all sorts
// of things going on and help to isolate and fix problems. The
// debugger can be turned on in options to see these values.
// ------------------------------------------------------------------
u32 debug[0x10] = {0};
u32 DX = 0;
u32 DY = 0;

u32 last_vsync_count    __attribute__((section(".dtcm"))) = 0xFEEDBEEF;
u8 slide_dampen         __attribute__((section(".dtcm"))) = 0;
u8 skip_render          __attribute__((section(".dtcm"))) = 0;
u8 zoom_screen          __attribute__((section(".dtcm"))) = 0;
u8 bClickRemoveNeeded   __attribute__((section(".dtcm"))) = 1;

// -------------------------------------------------------------------------------------------
// These memory buffers will be pointed to by the MemoryMap[] array. This array contains 8
// pointers that can break down the Z80 memory into 8k chunks.
// -------------------------------------------------------------------------------------------

u8 *ROM_Memory;                         // ROM Carts are 4MB to support Illusion City
u8 *DISK_Memory;                        // Disk Memory will be 720K to support DSDD disks
u8 *RAM_Memory;                         // RAM is 512K for this Turbo-R
u8 SRAM_Memory[0x2000] ALIGN(32) = {0}; // To hold up to 8K of SRAM for FM-PAC

u8 io_show_status = 0;     // Used to indicate a disk activity icons
u8 sram_show_status = 0;   // Used to show SRAM icon for SRAM-based carts
u8 flash_show_status = 0;  // Used to show Flash (lightning bolt) icon for Flash-based carts
Media_t MyMedia[3];        // Covering CART1, CART2 and DISK

// --------------------------------------------------------------------------
// For machines that have a full keybaord, we use the Left and Right
// shoulder buttons on the NDS to emulate the SHIFT and CTRL keys...
// --------------------------------------------------------------------------
u8 key_shift __attribute__((section(".dtcm"))) = false;
u8 key_ctrl  __attribute__((section(".dtcm"))) = false;
u8 key_kana  __attribute__((section(".dtcm"))) = false;
u8 key_graph __attribute__((section(".dtcm"))) = false;

// ---------------------------------------------------------------------------
// Some timing and frame rate computations to keep the emulation on pace...
// ---------------------------------------------------------------------------
u16 emuActFrames    __attribute__((section(".dtcm"))) = 0;  // Actual frames rendered - used for FPS calculation
u16 timingFrames    __attribute__((section(".dtcm"))) = 0;  // This one will cycle 0-59 for the 60Hz refresh
u8 render_blended   __attribute__((section(".dtcm"))) = 0;  // Set to 1 to alpha-blend using NDS hardware - useful for Screen 7 high-res
u8 soundEmuPause    __attribute__((section(".dtcm"))) = 1;  // Set to 1 to pause (mute) sound, 0 is sound unmuted (sound channels active)

u8 kbd_key          __attribute__((section(".dtcm"))) = 0;  // 0 if no key pressed, othewise the ASCII key (e.g. 'A', 'B', '3', etc)
u16 nds_key         __attribute__((section(".dtcm"))) = 0;  // 0 if no key pressed, othewise the NDS keys from keysCurrent() or similar
u8 last_mapped_key  __attribute__((section(".dtcm"))) = 0;  // The last mapped key which has been pressed - used for key click feedback
u8 kbd_keys_pressed __attribute__((section(".dtcm"))) = 0;  // Each frame we check for keys pressed - since we can map keyboard keys to the NDS, there may be several pressed at once
u8 kbd_keys[12]     __attribute__((section(".dtcm"))) = {0};// Up to 12 possible keys pressed at the same time (we have 12 NDS physical buttons though it's unlikely that more than 2 or maybe 3 would be pressed)

// ---------------------------------------------------------
// These less-used vars don't need to be in fast memory...
// ---------------------------------------------------------
u8 bStartSoundEngine = false;  // Set to true to unmute sound after 1 frame of rendering...
int bg0, bg1, bg0b, bg1b;      // Some vars for NDS background screen handling
volatile u16 vusCptVBL = 0;    // We use this as a basic timer for the random screenshots shown
volatile u32 dsVSyncCount = 0; // So compiler doesn't cache this as it changes dynamically with interrupt calls
u8 touch_debounce = 0;         // A bit of touch-screen debounce
u8 key_debounce = 0;           // A bit of key debounce
u8 msx_caps_lock = 0;          // Set to 1 when MSX caps lock is active
u8 msx_kana_lock = 0;          // Set to 1 when MSX kana lock is active

// The DS/DSi has 12 keys that can be mapped
u16 NDS_keyMap[12] __attribute__((section(".dtcm"))) = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_A, KEY_B, KEY_X, KEY_Y, KEY_R, KEY_L, KEY_START, KEY_SELECT};

// --------------------------------------------------------------------
// The key map for the MSX... mapped into the NDS controller
// --------------------------------------------------------------------
u32 keyCoresp[MAX_KEY_OPTIONS] __attribute__((section(".dtcm"))) = {
    JST_UP,
    JST_DOWN,
    JST_LEFT,
    JST_RIGHT,
    JST_FIRE1,
    JST_FIRE2,

    JST_UP << 16, // P2 versions of the above...
    JST_DOWN << 16,
    JST_LEFT << 16,
    JST_RIGHT << 16,
    JST_FIRE1 << 16,
    JST_FIRE2 << 16,

    META_KBD_A,
    META_KBD_B,
    META_KBD_C,
    META_KBD_D,
    META_KBD_E,
    META_KBD_F,
    META_KBD_G,
    META_KBD_H,
    META_KBD_I,
    META_KBD_J,
    META_KBD_K,
    META_KBD_L,
    META_KBD_M,
    META_KBD_N,
    META_KBD_O,
    META_KBD_P,
    META_KBD_Q,
    META_KBD_R,
    META_KBD_S,
    META_KBD_T,
    META_KBD_U,
    META_KBD_V,
    META_KBD_W,
    META_KBD_X,
    META_KBD_Y,
    META_KBD_Z,
    META_KBD_0,
    META_KBD_1,
    META_KBD_2,
    META_KBD_3,
    META_KBD_4,
    META_KBD_5,
    META_KBD_6,
    META_KBD_7,
    META_KBD_8,
    META_KBD_9,
    META_KBD_SHIFT,
    META_KBD_CTRL,
    META_KBD_KANA,
    META_KBD_GRAPH,
    META_KBD_SPACE,
    META_KBD_RETURN,
    META_KBD_ESC,
    META_KBD_HOME,
    META_KBD_UP,
    META_KBD_DOWN,
    META_KBD_LEFT,
    META_KBD_RIGHT,
    META_KBD_PERIOD,
    META_KBD_COMMA,
    META_KBD_COLON,
    META_KBD_SEMI,
    META_KBD_QUOTE,
    META_KBD_SLASH,
    META_KBD_CARET,
    META_KBD_MINUS,
    META_KBD_LBRACKET,
    META_KBD_RBRACKET,
    META_KBD_ATSIGN,
    META_KBD_YEN,
    META_KBD_BS,
    META_KBD_TAB,
    META_KBD_INS,
    META_KBD_DEL,
    META_KBD_STOP_BRK,
    META_KBD_F1,
    META_KBD_F2,
    META_KBD_F3,
    META_KBD_F4,
    META_KBD_F5,
    META_KBD_PANUP,
    META_KBD_PANDN,
    META_KBD_SHOWTOP,
    META_KBD_SHOWBOT,
};

static char tmpStr[64]; // For various sprintf() calls

// ------------------------------------------------------------
// Utility function to pause the sound...
// ------------------------------------------------------------
void SoundPause(void)
{
    soundEmuPause = 1;
}

// ------------------------------------------------------------
// Utility function to un pause the sound...
// ------------------------------------------------------------
void SoundUnPause(void)
{
    soundEmuPause = 0;
}

// --------------------------------------------------------------
// MAXMOD streaming setup and handling...
// --------------------------------------------------------------
#define sample_rate (31050)    // To match the sound driver - this is good enough quality for the DS
#define buffer_size (512 + 16) // Enough buffer that we don't have to fill it too often. Must be multiple of 16.

mm_ds_system sys   __attribute__((section(".dtcm")));
mm_stream myStream __attribute__((section(".dtcm")));

// -------------------------------------------------------------------------------------------
// maxmod will call this routine when the buffer is half-empty and requests that
// we fill the sound buffer with more samples. They will request 'len' samples and
// we will fill exactly that many. If the sound is paused, we fill with 'mute' samples.
// -------------------------------------------------------------------------------------------
s16 last_sample __attribute__((section(".dtcm"))) = 0;

// ------------------------------------------------------------------------------------------------------------
// On exit from Sound Pause and whenever the sound driver changes (AY -> AY+MSX-MUSIC), there is a short
// pop/click heard as the new mixer comes online. This routine is designed to perform a simple quick mixing
// and drive the sample output towards the new target to soften the pop/click. It's not perfect but it helps...
// ------------------------------------------------------------------------------------------------------------
mm_word ClickRemoval(mm_word len, mm_addr dest)
{
    s16 mix1[16];

    ay38910Mixer(16, mix1, &myAY); // Mix AY and give us something to move towards

    if (msx_music_capable_game)
    {
        YMMixer(len - 16, dest, &myYM); // Keep the YM stepping normally
        YMMixer(16, mix1, &myYM);       // Mix directly on top of AY
    }
    
    int target = mix1[15]; // This is what we are smoothing towards

    s16 *p = (s16 *)dest;
    for (int i = 0; i < len; i++)
    {
        if (last_sample < (target - 16))
        {
            last_sample += 8;
        }
        if (last_sample > (target + 16))
        {
            last_sample -= 8;
        }
        *p++ = last_sample;
    }

    if (--bClickRemoveNeeded == 0)
    {
        // Nothing to do...
    }

    return len;
}

// -----------------------------------------------------------------------------------------------
// This is our main sound mixer. It's called periodically by the maxmod handler when it needs
// more samples in the buffer. We mix AY + MSX-MUSIC as that's the sound setup for Illusion City.
// -----------------------------------------------------------------------------------------------
ITCM_CODE mm_word OurSoundMixer(mm_word len, mm_addr dest, mm_stream_formats format)
{
    if (soundEmuPause) // If paused, just "mix" in mute sound chip... all channels are OFF
    {
        s16 *p = (s16 *)dest;
        for (int i = 0; i < len; i++)
        {
            *p++ = last_sample; // To prevent pops and clicks... just keep outputting the last sample
        }
    }
    else
    {
        if (bClickRemoveNeeded)
        {
            return ClickRemoval(len, dest);
        }

        ay38910Mixer(len, dest, &myAY);
        if (msx_music_capable_game)
        {
            YMMixer(len, dest, &myYM);
        }

        last_sample = ((s16 *)dest)[len - 1];
    }

    return len;
}


// -------------------------------------------------------------------------------------------
// Setup the maxmod audio stream - this will be a 16-bit Stereo PCM output at 55KHz which
// sounds about right for the MSX audio data stream.
// -------------------------------------------------------------------------------------------
void setupStream(void)
{
    //----------------------------------------------------------------
    //  initialize maxmod with our small 4-effect soundbank
    //----------------------------------------------------------------
    mmInitDefaultMem((mm_addr)soundbank_bin);

    mmLoadEffect(SFX_CLICKNOQUIT);
    mmLoadEffect(SFX_KEYCLICK);
    mmLoadEffect(SFX_MUS_INTRO);

    //----------------------------------------------------------------
    //  open stream
    //----------------------------------------------------------------
    myStream.sampling_rate = sample_rate;                                   // sample_rate for the emulated MSX to match the AY/SCC drivers
    myStream.buffer_length = buffer_size;                                   // buffer length = (512+16)
    myStream.callback = OurSoundMixer;                                      // set callback function
    myStream.format = MM_STREAM_16BIT_MONO;                                 // format = mono 16-bit
    myStream.timer = MM_TIMER0;                                             // use hardware timer 0
    myStream.manual = false;                                                // use automatic filling
    mmStreamOpen(&myStream);

    //----------------------------------------------------------------
    //  when using 'automatic' filling, your callback will be triggered
    //  every time half of the wave buffer is processed.
    //
    //  so:
    //  25000 (rate)
    //  ----- = ~21 Hz for a full pass, and ~42hz for half pass
    //  1200  (length)
    //----------------------------------------------------------------
    //  with 'manual' filling, you must call mmStreamUpdate
    //  periodically (and often enough to avoid buffer underruns)
    //----------------------------------------------------------------
}

// ----------------------------------------------------------------------------------
// Ensure the sound chips are reset and associated helper variables are initialized.
// ----------------------------------------------------------------------------------
void sound_chip_reset()
{
    s16 mixbuf[16];

    SoundPause();

    bClickRemoveNeeded = 150; // When we re-enable... smooth it to avoid pops/clicks

    msx_music_capable_game = 0; // Until proven otherwise

    // -----------------------------------------------------------
    // The AY sound chip is for our baseline MSX sound handling.
    // -----------------------------------------------------------
    ay38910Reset(&myAY);            // Reset the "AY" sound chip
    ay38910IndexW(0x07, &myAY);     // Register 7 is ENABLE
    ay38910DataW(0x3F, &myAY);      // All OFF (negative logic)
    ay38910Mixer(8, mixbuf, &myAY); // Do an initial mix conversion to prime the output
    myAY.ayPortBOut = 0x80;         // Kana Lock off to start

    // -----------------------------------------------------------------
    // The YM2413 chip is the MSX MUSIC standard for late-era games...
    // -----------------------------------------------------------------
    YMReset(&myYM);
    YMMixer(8, mixbuf, &myYM); // Do an initial mix conversion to prime the output
}

// -----------------------------------------------------------------------
// We setup the sound chips - disabling all volumes to start.
// -----------------------------------------------------------------------
void dsInstallSoundEmuFIFO(void)
{
    sound_chip_reset();       // Reset the SN, AY and SCC chips
    setupStream();            // Setup maxmod stream...
    bStartSoundEngine = true; // Volume will 'unpause' after 1 frame in the main loop.
}

//*****************************************************************************
// Reset the MSX - mostly CPU and memory plus some timer vars for housekeeping.
//*****************************************************************************
void ResetMSX(void)
{
    JoyMode = JOYMODE_JOYSTICK; // Joystick mode key
    JoyState = 0x00000000;      // Nothing pressed to start

    Reset9938(); // Reset the video chip

    sound_chip_reset(); // Reset the SN, AY and SCC chips

    Z80_Interface_Reset(); // Reset the Z80 Interface
    ResetZ80(&CPU);        // Reset the Z80 CPU core

    msx_reset(); // Reset the MSX specific vars

    msx_caps_lock = 0; // MSX CAPS lock off
    msx_kana_lock = 0; // MSX KANA lock off

    zoom_screen = 0; // Assume no zoom to start

    msxWipeRAM();       // Wipe main RAM area (config chooses zero or random)
    msx_restore_bios(); // Put the BIOS back in place and point to it

    // -----------------------------------------------------------
    // Timer 1 is used to time frame-to-frame of actual emulation
    // -----------------------------------------------------------
    TIMER1_CR = 0;
    TIMER1_DATA = 0;
    TIMER1_CR = TIMER_ENABLE | TIMER_DIV_1024;

    // -----------------------------------------------------------
    // Timer 2 is used to time once per second events
    // -----------------------------------------------------------
    TIMER2_CR = 0;
    TIMER2_DATA = 0;
    TIMER2_CR = TIMER_ENABLE | TIMER_DIV_1024;
    timingFrames = 0;
    skip_render = 0;
    io_show_status = 0;
    sram_show_status = 0;
    flash_show_status = 0;
    render_blended = 0;
}

//*********************************************************************************
// A mini Z80 debugger of sorts. Put out some Z80, VDP and SGM/Bank info on
// screen every frame to help us debug some of the problem games. This is enabled
// via global configuration for debugger.
//*********************************************************************************
extern u8 *fake_heap_end;   // current heap start
extern u8 *fake_heap_start; // current heap end

u8 *getHeapStart() { return fake_heap_start; }
u8 *getHeapEnd() { return (u8 *)sbrk(0); }
u8 *getHeapLimit() { return fake_heap_end; }

int getMemUsed()
{ // returns the amount of used memory in bytes
    struct mallinfo mi = mallinfo();
    return mi.uordblks;
}

int getMemFree()
{ // returns the amount of free memory in bytes
    struct mallinfo mi = mallinfo();
    return mi.fordblks + (getHeapLimit() - getHeapEnd());
}

void ShowDebugZ80(void)
{
    u8 idx = 1;

    sprintf(tmpStr, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[0], VDP[1], VDP[2], VDP[3], VDP[4], VDP[5], VDP[6], VDP[7]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[8], VDP[9], VDP[10], VDP[11], VDP[12], VDP[13], VDP[14], VDP[15]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "VDP: %02X %02X %02X %02X %02X %02X %02X %02X", VDP[16], VDP[17], VDP[18], VDP[19], VDP[20], VDP[21], VDP[22], VDP[23]);
    DSPrint(0, idx++, 7, tmpStr);

    sprintf(tmpStr, "VStat %02X %02X %02X %02X Data=%02X", VDPStatus[0], VDPStatus[1], VDPStatus[2], VDPStatus[3], VDPDlatch);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "VAddr %08X", (VDP[14] << 14) + (int)VAddr);
    DSPrint(0, idx++, 7, tmpStr);
    idx++;

    sprintf(tmpStr, "Z80PC %04X", CPU.PC.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Z80SP %04X", CPU.SP.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Z80AF %04X", CPU.AF.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Z80BC %04X", CPU.BC.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Z80DE %04X", CPU.DE.W);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "IRQ %04X %d", CPU.IRequest, (CPU.NumInts % 99999));
    DSPrint(0, idx++, 7, tmpStr);
    idx++;

    sprintf(tmpStr, "AY:%02X %02X %02X %02X", myAY.ayRegs[0], myAY.ayRegs[1], myAY.ayRegs[2], myAY.ayRegs[3]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "AY:%02X %02X %02X %02X", myAY.ayRegs[4], myAY.ayRegs[5], myAY.ayRegs[6], myAY.ayRegs[7]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "AY:%02X %02X %02X %02X", myAY.ayRegs[8], myAY.ayRegs[9], myAY.ayRegs[10], myAY.ayRegs[11]);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "AY:%02X %02X %02X %02X", myAY.ayRegs[12], myAY.ayRegs[13], myAY.ayRegs[14], myAY.ayRegs[15]);
    DSPrint(0, idx++, 7, tmpStr);

    idx++;
    sprintf(tmpStr, "S%d  A8=%02X %02X", ScrMode, Port_PPI_A, msx_subslot);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "FD.ST=%02X CM=%02X", FDC.status, FDC.command);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "Mapper %d [%02X]", MyMedia[MEDIA_CART1].mapperType, MyMedia[MEDIA_CART1].mapperMask);
    DSPrint(0, idx++, 7, tmpStr);
    extern u32 halt_counter;
    sprintf(tmpStr, "Halt C %d", halt_counter & 0x1FFFF);
    DSPrint(0, idx++, 7, tmpStr);
    sprintf(tmpStr, "%d Free", getMemFree());
    DSPrint(0, idx++, 7, tmpStr);

    idx = 6;
    for (u8 i = 0; i < 16; i++)
    {
        sprintf(tmpStr, "D%-2d %-8lu %04X", i, debug[i], (u16)debug[i]);
        DSPrint(15, idx++, 7, tmpStr);
    }
}

// ------------------------------------------------------------------------
// The status line shows the status of the SCC, MSX-MUSIC, disk icons, etc.
// ------------------------------------------------------------------------
void DisplayStatusLine(void)
{
    if (myGlobalConfig.debugger)
    {
        return; // If debugger, skip this
    }

    if (sram_show_status)
    {
        DSPrint(28, 0, 2, "'()"); // Show SRAM icon
        DSPrint(28, 1, 2, "GHI"); // Show SRAM icon
        if (--sram_show_status == 0)
        {
            msxSaveSRAM();
        }
    }
    else if (flash_show_status)
    {
        DSPrint(28, 0, 0, " ");
        DSPrint(29, 0, 2, "?"); // Show Flash Write icon
        DSPrint(30, 0, 0, " ");
        DSPrint(28, 1, 0, " ");
        DSPrint(29, 1, 2, "_"); // Show Flash Write icon
        DSPrint(30, 1, 0, " ");
        if (--flash_show_status == 0)
        {
            // TODO: Write flash to SD?
        }
    }
    else if (io_show_status)
    {
        if (io_show_status == 8) // Disk Write
        {
            DSPrint(28, 0, 2, "678"); // Show Disk icon
            DSPrint(28, 1, 2, "VWX"); // Show Disk icon
            io_show_status = 6;       // Show icon briefly
        }
        else if (io_show_status == 7) // Disk Read
        {
            DSPrint(28, 0, 2, "345"); // Show Disk icon
            DSPrint(28, 1, 2, "STU"); // Show Disk icon
            io_show_status = 6;       // Show icon briefly
        }

        if (--io_show_status == 0) // Done with read/write activity
        {
            fdc_persist_disk(); // See if the disk needs to be persisted.
        }
    }
    // ------------------------------------------------------------
    // If we aren't showing the disk icon or SRAM above, we clear.
    // ------------------------------------------------------------
    else
    {
        DSPrint(28, 0, 0, "   ");
        DSPrint(28, 1, 0, "   ");
    }

    // -----------------------------------------------------------------
    // If the full MSX keyboard is showing, we can show some symbols
    // to indicate if the CAPS lock is enabled, shift/ctrl/graph, etc.
    // -----------------------------------------------------------------
    if (myConfig.keyboard == OVL_FULLKBD)
    {
        // Caps Lock
        DSPrint(1, 23, 0, (msx_caps_lock ? "@" : " "));
        DSPrint(2, 23, (msx_caps_lock ? 2 : 0), (msx_caps_lock ? "@" : " "));

        if (msx_kana_lock != ((myAY.ayPortBOut & 0x80) ? 0 : 1))
        {
            msx_kana_lock = (myAY.ayPortBOut & 0x80) ? 0 : 1;
            BottomScreenKeypad();
        }

        DSPrint(1, 19, 0, (key_shift ? "A" : " "));
        DSPrint(2, 19, 0, (key_shift ? "!" : " "));

        DSPrint(1, 15, 0, (key_ctrl ? "@" : " "));
        DSPrint(2, 15, (key_ctrl ? 2 : 0), (key_ctrl ? "@" : " "));

        if (zoom_screen) // Pan and Scan on Mode 7 and Mode 0 with 80 Columns
        {
            key_graph = 0;
            DSPrint(4, 21, 0, ":;<");
            DSPrint(4, 22, 0, "Z[\\");
        }
        else
        {
            DSPrint(4, 21, 0, "   ");
            DSPrint(4, 22, 0, "   ");
        }
        DSPrint(5, 23, (key_graph ? 2 : 0), (key_graph ? "]" : " "));
    }
}

// ------------------------------------------------------------------------
// Handle mini-menu interface...
// ------------------------------------------------------------------------
u8 MiniMenu(void)
{
    u8 retVal = MENU_CHOICE_NONE;

    SoundPause();

    while ((keysCurrent() & (KEY_TOUCH | KEY_LEFT | KEY_RIGHT | KEY_A)) != 0)
        ;

    BottomScreenMiniMenu();

    while (true)
    {
        nds_key = keysCurrent();
        if (nds_key & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT | KEY_L | KEY_R))
        {
            retVal = MENU_CHOICE_NONE;
            break; // We're done...
        }

        if (nds_key & KEY_TOUCH)
        {
            touchPosition touch;
            touchRead(&touch);
            if ((touch.py >= 22) && (touch.py < 98)) // Top row of Icons
            {
                if (touch.px <= 64)
                {
                    retVal = MENU_CHOICE_LOAD_GAME1;
                }
                else if (touch.px <= 128)
                {
                    retVal = MENU_CHOICE_LOAD_GAME2;
                }
                else if (touch.px <= 192)
                {
                    retVal = MENU_CHOICE_RESET_GAME;
                }
                else if (touch.px <= 256)
                {
                    retVal = MENU_CHOICE_END_GAME;
                }

                break;
            }
            if ((touch.py >= 98) && (touch.py < 190)) // Bottom row of Icons
            {
                if (touch.px <= 64)
                {
                    retVal = MENU_CHOICE_SAVE_GAME1;
                }
                else if (touch.px <= 128)
                {
                    retVal = MENU_CHOICE_SAVE_GAME2;
                }
                else if (touch.px <= 192)
                {
                    retVal = MENU_CHOICE_GAME_OPTIONS;
                }
                else if (touch.px <= 256)
                {
                    retVal = MENU_CHOICE_DEFINE_KEYS;
                }

                break;
            }
        }
    }

    while ((keysCurrent() & (KEY_TOUCH | KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT)) != 0)
        ;

    WAITVBL;
    WAITVBL;

    BottomScreenKeypad();

    SoundUnPause();

    return retVal;
}

// ------------------------------------------------------
// Handle the MSX virtual keyboard on the bottom screen.
// ------------------------------------------------------
u8 last_special_key = 0;
u8 last_special_key_dampen = 0;
u8 last_kbd_key = 0;
u8 dampen_zoom = 0;

u8 handle_msx_keyboard_press(u16 iTx, u16 iTy) // MSX Keyboard
{
    if ((iTx > 212) && (iTy >= 102) && (iTy < 162)) // Triangular Arrow Keys... do our best
    {
        if (iTy < 120)
        {
            kbd_key = KBD_KEY_UP;
        }
        else if (iTy > 145)
        {
            kbd_key = KBD_KEY_DOWN;
        }
        else if (iTx < 234)
        {
            kbd_key = KBD_KEY_LEFT;
        }
        else
        {
            kbd_key = KBD_KEY_RIGHT;
        }
    }
    else if ((iTy >= 12) && (iTy < 42)) // Row 1 (top row with Function Keys)
    {
        if ((iTx >= 0) && (iTx < 22))
        {
            kbd_key = KBD_KEY_ESC;
        }
        else if ((iTx >= 22) && (iTx < 44))
        {
            kbd_key = KBD_KEY_HOME;
        }
        else if ((iTx >= 44) && (iTx < 73))
        {
            kbd_key = KBD_KEY_F1;
        }
        else if ((iTx >= 73) && (iTx < 102))
        {
            kbd_key = KBD_KEY_F2;
        }
        else if ((iTx >= 102) && (iTx < 131))
        {
            kbd_key = KBD_KEY_F3;
        }
        else if ((iTx >= 131) && (iTx < 160))
        {
            kbd_key = KBD_KEY_F4;
        }
        else if ((iTx >= 160) && (iTx < 190))
        {
            kbd_key = KBD_KEY_F5;
        }
        else if ((iTx >= 190) && (iTx < 212))
        {
            kbd_key = KBD_KEY_BS;
        }
        else if ((iTx >= 212) && (iTx < 235))
        {
            kbd_key = KBD_KEY_INS;
        }
        else if ((iTx >= 235) && (iTx < 255))
        {
            kbd_key = KBD_KEY_DEL;
        }
    }
    else if ((iTy >= 42) && (iTy < 72)) // Row 2 (number row)
    {
        if ((iTx >= 0) && (iTx < 15))
        {
            kbd_key = '@';
        }
        else if ((iTx >= 15) && (iTx < 31))
        {
            kbd_key = '1';
        }
        else if ((iTx >= 31) && (iTx < 45))
        {
            kbd_key = '2';
        }
        else if ((iTx >= 45) && (iTx < 61))
        {
            kbd_key = '3';
        }
        else if ((iTx >= 61) && (iTx < 75))
        {
            kbd_key = '4';
        }
        else if ((iTx >= 75) && (iTx < 91))
        {
            kbd_key = '5';
        }
        else if ((iTx >= 91) && (iTx < 106))
        {
            kbd_key = '6';
        }
        else if ((iTx >= 106) && (iTx < 121))
        {
            kbd_key = '7';
        }
        else if ((iTx >= 121) && (iTx < 135))
        {
            kbd_key = '8';
        }
        else if ((iTx >= 135) && (iTx < 151))
        {
            kbd_key = '9';
        }
        else if ((iTx >= 151) && (iTx < 165))
        {
            kbd_key = '0';
        }
        else if ((iTx >= 165) && (iTx < 181))
        {
            kbd_key = '-';
        }
        else if ((iTx >= 181) && (iTx < 195))
        {
            kbd_key = '=';
        }
        else if ((iTx >= 195) && (iTx < 210))
        {
            kbd_key = '|'; // YEN
        }
        else if ((iTx >= 210) && (iTx < 255))
        {
            kbd_key = KBD_KEY_SEL;
        }
    }
    else if ((iTy >= 72) && (iTy < 102)) // Row 3 (QWERTY row)
    {
        if ((iTx >= 0) && (iTx < 23))
        {
            kbd_key = KBD_KEY_TAB;
        }
        else if ((iTx >= 23) && (iTx < 39))
        {
            kbd_key = 'Q';
        }
        else if ((iTx >= 39) && (iTx < 54))
        {
            kbd_key = 'W';
        }
        else if ((iTx >= 54) && (iTx < 69))
        {
            kbd_key = 'E';
        }
        else if ((iTx >= 69) && (iTx < 83))
        {
            kbd_key = 'R';
        }
        else if ((iTx >= 83) && (iTx < 99))
        {
            kbd_key = 'T';
        }
        else if ((iTx >= 99) && (iTx < 113))
        {
            kbd_key = 'Y';
        }
        else if ((iTx >= 113) && (iTx < 129))
        {
            kbd_key = 'U';
        }
        else if ((iTx >= 129) && (iTx < 143))
        {
            kbd_key = 'I';
        }
        else if ((iTx >= 143) && (iTx < 158))
        {
            kbd_key = 'O';
        }
        else if ((iTx >= 158) && (iTx < 174))
        {
            kbd_key = 'P';
        }
        else if ((iTx >= 174) && (iTx < 189))
        {
            kbd_key = '[';
        }
        else if ((iTx >= 189) && (iTx < 203))
        {
            kbd_key = ']';
        }
        else if ((iTx >= 203) && (iTx < 214))
        {
            kbd_key = KBD_KEY_DEAD;
        }
        else if ((iTx >= 214) && (iTx < 255))
        {
            kbd_key = KBD_KEY_STOP;
        }
    }
    else if ((iTy >= 102) && (iTy < 132)) // Row 4 (ASDF row)
    {
        if ((iTx >= 0) && (iTx < 27))
        {
            kbd_key = KBD_KEY_CTRL;
            last_special_key = KBD_KEY_CTRL;
            last_special_key_dampen = 20;
        }
        else if ((iTx >= 27) && (iTx < 43))
        {
            kbd_key = 'A';
        }
        else if ((iTx >= 43) && (iTx < 58))
        {
            kbd_key = 'S';
        }
        else if ((iTx >= 58) && (iTx < 72))
        {
            kbd_key = 'D';
        }
        else if ((iTx >= 72) && (iTx < 87))
        {
            kbd_key = 'F';
        }
        else if ((iTx >= 87) && (iTx < 102))
        {
            kbd_key = 'G';
        }
        else if ((iTx >= 102) && (iTx < 117))
        {
            kbd_key = 'H';
        }
        else if ((iTx >= 117) && (iTx < 132))
        {
            kbd_key = 'J';
        }
        else if ((iTx >= 132) && (iTx < 147))
        {
            kbd_key = 'K';
        }
        else if ((iTx >= 147) && (iTx < 161))
        {
            kbd_key = 'L';
        }
        else if ((iTx >= 161) && (iTx < 178))
        {
            kbd_key = ';';
        }
        else if ((iTx >= 178) && (iTx < 192))
        {
            kbd_key = KBD_KEY_QUOTE;
        }
        else if ((iTx >= 192) && (iTx < 214))
        {
            kbd_key = KBD_KEY_RET;
        }
    }
    else if ((iTy >= 132) && (iTy < 162)) // Row 5 (ZXCV row)
    {
        if ((iTx >= 0) && (iTx < 33))
        {
            kbd_key = KBD_KEY_SHIFT;
            last_special_key = KBD_KEY_SHIFT;
            last_special_key_dampen = 20;
        }
        else if ((iTx >= 33) && (iTx < 49))
        {
            kbd_key = 'Z';
        }
        else if ((iTx >= 49) && (iTx < 64))
        {
            kbd_key = 'X';
        }
        else if ((iTx >= 64) && (iTx < 78))
        {
            kbd_key = 'C';
        }
        else if ((iTx >= 78) && (iTx < 94))
        {
            kbd_key = 'V';
        }
        else if ((iTx >= 94) && (iTx < 109))
        {
            kbd_key = 'B';
        }
        else if ((iTx >= 109) && (iTx < 123))
        {
            kbd_key = 'N';
        }
        else if ((iTx >= 123) && (iTx < 139))
        {
            kbd_key = 'M';
        }
        else if ((iTx >= 139) && (iTx < 154))
        {
            kbd_key = ',';
        }
        else if ((iTx >= 154) && (iTx < 169))
        {
            kbd_key = '.';
        }
        else if ((iTx >= 169) && (iTx < 184))
        {
            kbd_key = '/';
        }
        else if ((iTx >= 184) && (iTx < 214))
        {
            kbd_key = KBD_KEY_RET;
        }
    }
    else if ((iTy >= 162) && (iTy < 192)) // Row 6 (SPACE BAR and icons row)
    {
        if ((iTx >= 1) && (iTx < 30))
        {
            kbd_key = KBD_KEY_CAPS;
        }
        else if ((iTx >= 30) && (iTx < 53)) // Graph Key... also Zoom for Screen 7
        {
            if (zoom_screen == 1)
            {
                if (!dampen_zoom)
                {
                    zoom_screen = 2;
                }
                dampen_zoom = 10;
            }
            else if (zoom_screen == 2)
            {
                if (!dampen_zoom)
                {
                    zoom_screen = 1;
                }
                dampen_zoom = 10;
            }
            else
            {
                kbd_key = KBD_KEY_GRAPH;
                last_special_key = KBD_KEY_GRAPH;
                last_special_key_dampen = 20;
            }
        }
        else if ((iTx >= 53) && (iTx < 163))
        {
            kbd_key = ' ';
        }
        else if ((iTx >= 163) && (iTx < 192))
        {
            kbd_key = KBD_KEY_KANA;
        }
        else if ((iTx >= 192) && (iTx < 255))
        {
            return MENU_CHOICE_MENU;
        }
    }

    return MENU_CHOICE_NONE;
}

u8 handle_alpha_keyboard_press(u16 iTx, u16 iTy) // Generic and Simplified Alpha-Numeric Keyboard
{
    if ((iTy >= 14) && (iTy < 50)) // Row 1 (number row)
    {
        if ((iTx >= 0) && (iTx < 28))
        {
            kbd_key = '1';
        }
        else if ((iTx >= 28) && (iTx < 54))
        {
            kbd_key = '2';
        }
        else if ((iTx >= 54) && (iTx < 80))
        {
            kbd_key = '3';
        }
        else if ((iTx >= 80) && (iTx < 106))
        {
            kbd_key = '4';
        }
        else if ((iTx >= 106) && (iTx < 132))
        {
            kbd_key = '5';
        }
        else if ((iTx >= 132) && (iTx < 148))
        {
            kbd_key = '6';
        }
        else if ((iTx >= 148) && (iTx < 174))
        {
            kbd_key = '7';
        }
        else if ((iTx >= 174) && (iTx < 200))
        {
            kbd_key = '8';
        }
        else if ((iTx >= 200) && (iTx < 226))
        {
            kbd_key = '9';
        }
        else if ((iTx >= 226) && (iTx < 255))
        {
            kbd_key = '0';
        }
    }
    else if ((iTy >= 50) && (iTy < 88)) // Row 2 (QWERTY row)
    {
        if ((iTx >= 0) && (iTx < 28))
        {
            kbd_key = 'Q';
        }
        else if ((iTx >= 28) && (iTx < 54))
        {
            kbd_key = 'W';
        }
        else if ((iTx >= 54) && (iTx < 80))
        {
            kbd_key = 'E';
        }
        else if ((iTx >= 80) && (iTx < 106))
        {
            kbd_key = 'R';
        }
        else if ((iTx >= 106) && (iTx < 132))
        {
            kbd_key = 'T';
        }
        else if ((iTx >= 132) && (iTx < 148))
        {
            kbd_key = 'Y';
        }
        else if ((iTx >= 148) && (iTx < 174))
        {
            kbd_key = 'U';
        }
        else if ((iTx >= 174) && (iTx < 200))
        {
            kbd_key = 'I';
        }
        else if ((iTx >= 200) && (iTx < 226))
        {
            kbd_key = 'O';
        }
        else if ((iTx >= 226) && (iTx < 255))
        {
            kbd_key = 'P';
        }
    }
    else if ((iTy >= 88) && (iTy < 125)) // Row 3 (ASDF row)
    {
        if ((iTx >= 0) && (iTx < 28))
        {
            kbd_key = 'A';
        }
        else if ((iTx >= 28) && (iTx < 54))
        {
            kbd_key = 'S';
        }
        else if ((iTx >= 54) && (iTx < 80))
        {
            kbd_key = 'D';
        }
        else if ((iTx >= 80) && (iTx < 106))
        {
            kbd_key = 'F';
        }
        else if ((iTx >= 106) && (iTx < 132))
        {
            kbd_key = 'G';
        }
        else if ((iTx >= 132) && (iTx < 148))
        {
            kbd_key = 'H';
        }
        else if ((iTx >= 148) && (iTx < 174))
        {
            kbd_key = 'J';
        }
        else if ((iTx >= 174) && (iTx < 200))
        {
            kbd_key = 'K';
        }
        else if ((iTx >= 200) && (iTx < 226))
        {
            kbd_key = 'L';
        }
        else if ((iTx >= 226) && (iTx < 255))
        {
            kbd_key = KBD_KEY_BS;
        }
    }
    else if ((iTy >= 125) && (iTy < 160)) // Row 4 (ZXCV row)
    {
        if ((iTx >= 0) && (iTx < 28))
        {
            kbd_key = 'Z';
        }
        else if ((iTx >= 28) && (iTx < 54))
        {
            kbd_key = 'X';
        }
        else if ((iTx >= 54) && (iTx < 80))
        {
            kbd_key = 'C';
        }
        else if ((iTx >= 80) && (iTx < 106))
        {
            kbd_key = 'V';
        }
        else if ((iTx >= 106) && (iTx < 132))
        {
            kbd_key = 'B';
        }
        else if ((iTx >= 132) && (iTx < 148))
        {
            kbd_key = 'N';
        }
        else if ((iTx >= 148) && (iTx < 174))
        {
            kbd_key = 'M';
        }
        else if ((iTx >= 174) && (iTx < 200))
        {
            kbd_key = (key_shift ? ',' : '.');
        }
        else if ((iTx >= 200) && (iTx < 255))
        {
            kbd_key = KBD_KEY_RET;
        }
    }
    else if ((iTy >= 160) && (iTy < 192)) // Row 5 (SPACE BAR and icons row)
    {
        if ((iTx >= 1) && (iTx < 202))
        {
            kbd_key = ' ';
        }
        else if ((iTx >= 202) && (iTx < 255))
        {
            return MENU_CHOICE_MENU;
        }
    }

    return MENU_CHOICE_NONE;
}

u8 handle_debugger_overlay(u16 iTx, u16 iTy)
{
    if ((iTy >= 175) && (iTy < 192)) // Bottom row is where the debugger keys are...
    {
        if ((iTx >= 1) && (iTx < 125))
        {
            kbd_key = ' ';
        }
        if ((iTx >= 125) && (iTx < 158))
        {
            return MENU_CHOICE_MENU;
        }
        else if ((iTx >= 192) && (iTx < 255))
        {
            return MENU_CHOICE_MENU;
        }
    }
    else
    {
        kbd_key = 0;
        last_kbd_key = 0;
    }

    return MENU_CHOICE_NONE;
}

// ------------------------------------------------------------------------
// The main emulation loop is here... call into the Z80, VDP and PSG
// ------------------------------------------------------------------------
void IllusionDSRunEmu(void)
{
    u16 iTx, iTy;
    u16 SaveNow = 0, LoadNow = 0;
    u32 JoyStickMap;
    static u32 lastUN = 0;
    static u8 dampenClick = 0;
    u8 meta_key = 0;

    // Setup the debug buffer for DSi use
    debug_init();

    // Returns when  user has asked for a game to run...
    BottomScreenOptions();

    // Clear top screen
    BG_PALETTE[0] = RGB15(0, 0, 0);
    memset((u8 *)0x06000000, 0x00, 0x40000); // Ensure screen is clear... (LCD_A and LCD_B)

    // Get the MSX Machine Emulator ready
    msxInit();

    Z80_Interface_Reset(); // Reset the Z80 Interface module
    ResetZ80(&CPU);        // Reset the CZ80 core CPU
    BottomScreenKeypad();  // Show the game-related screen with keypad / keyboard

    // Frame-to-frame timing...
    TIMER1_CR = 0;
    TIMER1_DATA = 0;
    TIMER1_CR = TIMER_ENABLE | TIMER_DIV_1024;

    // Once/second timing...
    TIMER2_CR = 0;
    TIMER2_DATA = 0;
    TIMER2_CR = TIMER_ENABLE | TIMER_DIV_1024;
    timingFrames = 0;

    // Force the sound engine to turn on when we start emulation
    bStartSoundEngine = true;

    // -------------------------------------------------------------------
    // Stay in this loop running the MSX game until the user exits...
    // -------------------------------------------------------------------
    while (1)
    {
        // Take a tour of the Z80 counter and display the screen if necessary
        if (!LoopZ80())
        {
            // If we've been asked to start the sound engine, rock-and-roll!
            if (bStartSoundEngine)
            {
                bStartSoundEngine = false;
                SoundUnPause();
            }

            // -------------------------------------------------------------
            // Stuff to do once/second such as FPS display and Debug Data
            // -------------------------------------------------------------
            if (TIMER1_DATA >= 32728) //  1000MS (1 sec)
            {
                TIMER1_CR = 0;
                TIMER1_DATA = 0;
                TIMER1_CR = TIMER_ENABLE | TIMER_DIV_1024;
                u16 emuFps = emuActFrames;
                if (myGlobalConfig.showFPS)
                {
                    if (myGlobalConfig.showFPS != 2) // If not Full Speed... round towards 60
                    {
                        if (emuFps == 61)
                        {
                            emuFps = 60;
                        }
                        else if (emuFps == 59)
                        {
                            emuFps = 60;
                        }
                    }
                    DSPrint_fps(emuFps);
                }
                emuActFrames = 0;
            }

            emuActFrames++; // This one can go above or below 60... used for FPS calculation

            // ---------------------------------------------
            // We only support NTSC 60 frames per second...
            // ---------------------------------------------
            if (++timingFrames == 60)
            {
                TIMER2_CR = 0;
                TIMER2_DATA = 0;
                TIMER2_CR = TIMER_ENABLE | TIMER_DIV_1024;
                timingFrames = 0;
                DisplayStatusLine(); // Every half second
            }
            else if (timingFrames == 30) // Every half second
            {
                DisplayStatusLine();
            }

            // -----------------------------------------------------
            // Vertical Sync reduces tearing but costs CPU time...
            // -----------------------------------------------------
            while (dsVSyncCount == last_vsync_count)
            {
                if (myGlobalConfig.showFPS == 2)
                {
                    break; // If Full Speed, break out...
                }
            }
            last_vsync_count = dsVSyncCount;

            // And copy out XBuf[] to the DS VRAM!
            msxUpdateScreen();

            // If the Z80 Debugger is enabled, call it every frame. Expensive but we need the debug!
            if (myGlobalConfig.debugger)
            {
                ShowDebugZ80();
            }

            // Screen 7 is 512px wide and we have the ability to render it in Zoom mode
            if (ScrMode == 7)
            {
                if (!zoom_screen)
                {
                    zoom_screen = 1;
                }
                render_blended = 1; // We Alpha Blend the A/B pixels in Screen 7
            }
            else if (ScrMode == 9) // 80 Column Mode (480px)
            {
                if (!zoom_screen)
                {
                    zoom_screen = 1;
                }
            }
            else
            {
                zoom_screen = 0;
            }

            // ---------------------------------------------------------------------------------
            // Hold the key press for a brief instant... some machines take longer than others
            // (eg MSX needs to see the keypress for many tens of milliseconds)... This allows
            // us to 'hold' the keypress in memory for several frames which is good enough.
            // ---------------------------------------------------------------------------------
            if (key_debounce > 0)
            {
                key_debounce--;
            }
            else
            {
                // -----------------------------------------------------------
                // This is where we accumulate the keys pressed... up to 12!
                // -----------------------------------------------------------
                kbd_keys_pressed = 0;
                memset(kbd_keys, 0x00, sizeof(kbd_keys));
                kbd_key = 0;

                // ------------------------------------------
                // Handle any screen touch events
                // ------------------------------------------
                if (keysCurrent() & KEY_TOUCH)
                {
                    // ------------------------------------------------------------------------------------------------
                    // Just a tiny bit of touch debounce so ensure touch screen is pressed for a fraction of a second.
                    // ------------------------------------------------------------------------------------------------
                    if (++touch_debounce > 1)
                    {
                        touchPosition touch;
                        touchRead(&touch);
                        iTx = touch.px;
                        iTy = touch.py;

                        if (myGlobalConfig.debugger)
                        {
                            meta_key = handle_debugger_overlay(iTx, iTy);
                        }
                        // ------------------------------------------------------------
                        // Test the touchscreen for various full keyboard handlers...
                        // ------------------------------------------------------------
                        else
                        {
                            meta_key = handle_msx_keyboard_press(iTx, iTy);
                        }

                        if (kbd_key != 0)
                        {
                            kbd_keys[kbd_keys_pressed++] = kbd_key;
                            key_debounce = 2;
                        }

                        // If the special menu key indicates we should show the choice menu, do so here...
                        if (meta_key == MENU_CHOICE_MENU)
                        {
                            meta_key = MiniMenu();
                        }

                        // -------------------------------------------------------------------
                        // If one of the special meta keys was picked, we handle that here...
                        // -------------------------------------------------------------------
                        switch (meta_key)
                        {
                        case MENU_CHOICE_RESET_GAME:
                            SoundPause();
                            // Ask for verification
                            if (showMessage("DO YOU REALLY WANT TO", "RESET THE CURRENT GAME ?") == ID_SHM_YES)
                            {
                                memset((u8 *)0x06000000, 0x00, 0x40000); // Ensure screen is clear...
                                ResetMSX();
                            }
                            BottomScreenKeypad();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_END_GAME:
                            SoundPause();
                            //  Ask for verification
                            if (showMessage("DO YOU REALLY WANT TO", "QUIT THE CURRENT GAME ?") == ID_SHM_YES)
                            {
                                memset((u8 *)0x06000000, 0x00, 0x40000); // Reset VRAM to 0x00 to clear any potential display garbage on way out
                                return;
                            }
                            BottomScreenKeypad();
                            DisplayStatusLine();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_GAME_OPTIONS:
                            SoundPause();
                            BottomScreenOptions();
                            IllusionDSGameOptions(false);
                            BottomScreenKeypad();
                            DisplayStatusLine();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_DEFINE_KEYS:
                            SoundPause();
                            BottomScreenOptions();
                            IllusionDSChangeKeymap();
                            BottomScreenKeypad();
                            DisplayStatusLine();
                            SoundUnPause();
                            break;

                        case MENU_CHOICE_SAVE_GAME1:
                            if (!SaveNow)
                            {
                                SoundPause();
                                if (showMessage("DO YOU REALLY WANT TO", "SAVE GAME STATE ?") == ID_SHM_YES)
                                {
                                    SaveNow = 1;
                                    msxSaveState(1);
                                }
                                BottomScreenKeypad();
                                SoundUnPause();
                            }
                            break;
                            
                        case MENU_CHOICE_SAVE_GAME2:
                            if (!SaveNow)
                            {
                                SoundPause();
                                if (showMessage("DO YOU REALLY WANT TO", "SAVE GAME STATE ?") == ID_SHM_YES)
                                {
                                    SaveNow = 1;
                                    msxSaveState(2);
                                }
                                BottomScreenKeypad();
                                SoundUnPause();
                            }
                            break;


                        case MENU_CHOICE_LOAD_GAME1:
                            if (!LoadNow)
                            {
                                SoundPause();
                                if (showMessage("DO YOU REALLY WANT TO", "LOAD GAME STATE ?") == ID_SHM_YES)
                                {
                                    LoadNow = 1;
                                    msxLoadState(1);
                                }
                                BottomScreenKeypad();
                                SoundUnPause();
                            }
                            break;

                        case MENU_CHOICE_LOAD_GAME2:
                            if (!LoadNow)
                            {
                                SoundPause();
                                if (showMessage("DO YOU REALLY WANT TO", "LOAD GAME STATE ?") == ID_SHM_YES)
                                {
                                    LoadNow = 1;
                                    msxLoadState(2);
                                }
                                BottomScreenKeypad();
                                SoundUnPause();
                            }
                            break;

                        default:
                            SaveNow = 0;
                            LoadNow = 0;
                        }

                        if (++dampenClick > 0) // Make sure the key is pressed for an appreciable amount of time...
                        {
                            if ((kbd_key != 0) && (lastUN == 0))
                            {
                                mmEffect(SFX_KEYCLICK); // Play short key click for feedback...
                            }
                            lastUN = kbd_key;
                        }
                    }
                } //  SCR_TOUCH
                else
                {
                    touch_debounce = 0;
                    SaveNow = LoadNow = 0;
                    lastUN = 0;
                    dampenClick = 0;
                    last_kbd_key = 0;
                    if (dampen_zoom)
                    {
                        dampen_zoom--;
                    }
                }
            }

            // ------------------------------------------------------------------------
            //  Test DS keypresses (ABXY, L/R) and map to corresponding MSX keys
            // ------------------------------------------------------------------------
            key_shift = false;
            key_ctrl = false;
            key_kana = false;
            key_graph = false;

            JoyStickMap = 0;
            nds_key = keysCurrent(); // Get any current keys pressed on the NDS

            // ----------------------------------------------------------------------
            // If we are in Screen 7 'Zoom' mode, the L/R keys take on new meaning...
            // ----------------------------------------------------------------------
            if ((zoom_screen == 2) && (nds_key & (KEY_L | KEY_R)) && ((nds_key & KEY_Y) == 0))
            {
                if (nds_key & KEY_R)
                {
                    if (screenPanX < 256)
                    {
                        screenPanX += 4;
                    }
                }
                if (nds_key & KEY_L)
                {
                    if (screenPanX > 0)
                    {
                        screenPanX -= 4;
                    }
                }
                nds_key &= ~(KEY_L | KEY_R); // We lose the ability for normal handling of these keys
            }

            if ((nds_key & KEY_L) && (nds_key & KEY_R) && (nds_key & KEY_X))
            {
                // Screen swap top/bottom
                lcdSwap();
                WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL;
            }
            else if ((nds_key & KEY_L) && (nds_key & KEY_R) && (nds_key & KEY_Y))
            {
                // Snapshot - show the little camera icon for a second or so...
                DSPrint(28, 0, 2, "9:;");
                DSPrint(28, 1, 2, "YZ[");
                screenshot();
                debug_save();
                WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL; WAITVBL;
                DSPrint(28, 0, 6, "   ");
                DSPrint(28, 1, 6, "   ");
            }
            else if (nds_key & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START | KEY_SELECT | KEY_R | KEY_L | KEY_X | KEY_Y))
            {
                // --------------------------------------------------------------------------------------------------
                // There are 12 NDS buttons (D-Pad, XYAB, L/R and Start+Select) - we allow mapping of any of these.
                // --------------------------------------------------------------------------------------------------
                for (u8 i = 0; i < 12; i++)
                {
                    if (nds_key & NDS_keyMap[i])
                    {
                        if (keyCoresp[myConfig.keymap[i]] < 0xFFFE0000) // Normal Joystick-type key map
                        {
                            JoyStickMap |= keyCoresp[myConfig.keymap[i]];
                        }
                        else // This is a keyboard maping... handle that here... just set the appropriate kbd_key
                        {
                            if ((keyCoresp[myConfig.keymap[i]] >= META_KBD_A) && (keyCoresp[myConfig.keymap[i]] <= META_KBD_Z))
                            {
                                kbd_key = ('A' + (keyCoresp[myConfig.keymap[i]] - META_KBD_A));
                            }
                            else if ((keyCoresp[myConfig.keymap[i]] >= META_KBD_0) && (keyCoresp[myConfig.keymap[i]] <= META_KBD_9))
                            {
                                kbd_key = ('0' + (keyCoresp[myConfig.keymap[i]] - META_KBD_0));
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SPACE)
                            {
                                kbd_key = ' ';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RETURN)
                            {
                                kbd_key = KBD_KEY_RET;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_ESC)
                            {
                                kbd_key = KBD_KEY_ESC;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SHIFT)
                            {
                                key_shift = 1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_CTRL)
                            {
                                key_ctrl = 1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_KANA)
                            {
                                key_kana = 1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_GRAPH)
                            {
                                key_graph = 1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_HOME)
                            {
                                kbd_key = KBD_KEY_HOME;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_UP)
                            {
                                kbd_key = KBD_KEY_UP;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_DOWN)
                            {
                                kbd_key = KBD_KEY_DOWN;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_LEFT)
                            {
                                kbd_key = KBD_KEY_LEFT;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RIGHT)
                            {
                                kbd_key = KBD_KEY_RIGHT;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_PERIOD)
                            {
                                kbd_key = '.';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_COMMA)
                            {
                                kbd_key = ',';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_COLON)
                            {
                                kbd_key = ':';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SEMI)
                            {
                                kbd_key = ';';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_QUOTE)
                            {
                                kbd_key = KBD_KEY_QUOTE;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_SLASH)
                            {
                                kbd_key = '/';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_CARET)
                            {
                                kbd_key = '^';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_MINUS)
                            {
                                kbd_key = '-';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_LBRACKET)
                            {
                                kbd_key = '[';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_RBRACKET)
                            {
                                kbd_key = ']';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_BS)
                            {
                                kbd_key = KBD_KEY_BS;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_ATSIGN)
                            {
                                kbd_key = '@';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_YEN)
                            {
                                kbd_key = '|';
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_TAB)
                            {
                                kbd_key = KBD_KEY_TAB;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_INS)
                            {
                                kbd_key = KBD_KEY_INS;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_DEL)
                            {
                                kbd_key = KBD_KEY_DEL;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_STOP_BRK)
                            {
                                kbd_key = KBD_KEY_STOP;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F1)
                            {
                                kbd_key = KBD_KEY_F1;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F2)
                            {
                                kbd_key = KBD_KEY_F2;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F3)
                            {
                                kbd_key = KBD_KEY_F3;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F4)
                            {
                                kbd_key = KBD_KEY_F4;
                            }
                            else if (keyCoresp[myConfig.keymap[i]] == META_KBD_F5)
                            {
                                kbd_key = KBD_KEY_F5;
                            }

                            if (kbd_key != 0)
                            {
                                kbd_keys[kbd_keys_pressed++] = kbd_key;
                            }
                        }
                    }
                }
            }
            else // Nothing pressed... let slide-n-glide counters drift down
            {
                last_mapped_key = 0;
            }

            // ---------------------------------------------------------
            // Accumulate all bits above into the Joystick State var...
            // ---------------------------------------------------------
            JoyState = JoyStickMap;

            // --------------------------------------------------
            // Handle Auto-Fire if enabled in configuration...
            // --------------------------------------------------
            static u8 autoFireTimer[2] = {0, 0};
            if ((myConfig.autoFire & 0x01) && (JoyState & JST_FIRE1)) // Fire Button 1
            {
                if ((++autoFireTimer[0] & 7) > 4)
                {
                    JoyState &= ~JST_FIRE1;
                }
            }
            if ((myConfig.autoFire & 0x02) && (JoyState & JST_FIRE2)) // Fire Button 2
            {
                if ((++autoFireTimer[1] & 7) > 4)
                {
                    JoyState &= ~JST_FIRE2;
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------------------
// We can use this VRAM as semi-fast RAM if needed... right now, no actual need.
// ----------------------------------------------------------------------------------------
void useVRAM(void)
{
    vramSetBankD(VRAM_D_LCD); // Not using this for video but 128K of faster RAM always useful!  Mapped at 0x06860000 -   Used as temp swap area when saving/loading state (256K total)
    vramSetBankE(VRAM_E_LCD); // Not using this for video but 64K of faster RAM always useful!   Mapped at 0x06880000 -   ..
    vramSetBankF(VRAM_F_LCD); // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x06890000 -   ..
    vramSetBankG(VRAM_G_LCD); // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x06894000 -   ..
    vramSetBankH(VRAM_H_LCD); // Not using this for video but 32K of faster RAM always useful!   Mapped at 0x06898000 -   ..
    vramSetBankI(VRAM_I_LCD); // Not using this for video but 16K of faster RAM always useful!   Mapped at 0x068A0000 -   16K available for general use...
}

void ShowWarning(u8 type)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    if (type == 1)
    {
        decompress(warning1Tiles, bgGetGfxPtr(bg0b), LZ77Vram);
        decompress(warning1Map, (void *)bgGetMapPtr(bg0b), LZ77Vram);
        dmaCopy((void *)warning1Pal, (void *)BG_PALETTE_SUB, 256 * 2);
    }
    else
    {
        decompress(warning2Tiles, bgGetGfxPtr(bg0b), LZ77Vram);
        decompress(warning2Map, (void *)bgGetMapPtr(bg0b), LZ77Vram);
        dmaCopy((void *)warning2Pal, (void *)BG_PALETTE_SUB, 256 * 2);
    }

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);

}

void restore_lcd(void)
{
    //  Init graphic mode (bitmap mode)
    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
    videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_BG1_ACTIVE | DISPLAY_SPR_1D_LAYOUT | DISPLAY_SPR_ACTIVE);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE); // Once emulation of game starts, we steal this back for an additional 128K of VRAM at 0x6820000
    vramSetBankC(VRAM_C_SUB_BG);

    //  Stop blending effect of intro
    REG_BLDCNT = 0;
    REG_BLDCNT_SUB = 0;
    REG_BLDY = 0;
    REG_BLDY_SUB = 0;

    swiWaitForVBlank();

    // Ensure upper screen is correct...
    UpperScreenMainMenu();
}

/*********************************************************************************
 * Init DS Emulator - setup VRAM banks and background screen rendering banks
 ********************************************************************************/
void IllusionDSInit(void)
{
    restore_lcd();

    // Clear out media bays
    memset(MyMedia, 0x00, sizeof(MyMedia));
}

// ---------------------------------------------------------------------------
// Setup the bottom screen - mostly for menu, high scores, options, etc.
// ---------------------------------------------------------------------------
void BottomScreenOptions(void)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    decompress(optionsTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(optionsMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void *)optionsPal, (void *)BG_PALETTE_SUB, 256 * 2);

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
}

// ---------------------------------------------------------------------------
// Setup the bottom screen for the main menu icons (carts, disk, play button)
// ---------------------------------------------------------------------------
void BottomScreenMainMenu(void)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    decompress(mainmenuTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(mainmenuMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void *)mainmenuPal, (void *)BG_PALETTE_SUB, 256 * 2);

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
}

// -----------------------------------------------------------------------------
// Setup the bottom screen for the mini MSX menu (with all the in-game options)
// -----------------------------------------------------------------------------
void BottomScreenMiniMenu(void)
{
    swiWaitForVBlank();

    bg0b = bgInitSub(0, BgType_Text8bpp, BgSize_T_256x256, 31, 0);
    bg1b = bgInitSub(1, BgType_Text8bpp, BgSize_T_256x256, 29, 0);
    bgSetPriority(bg0b, 1);
    bgSetPriority(bg1b, 0);
    decompress(minimenuTiles, bgGetGfxPtr(bg0b), LZ77Vram);
    decompress(minimenuMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
    dmaCopy((void *)minimenuPal, (void *)BG_PALETTE_SUB, 256 * 2);

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
}

// ---------------------------------------------------------------------------
// Keyboard decompression buffers in main RAM.
// Keep these static; do not put them on the stack.
// ---------------------------------------------------------------------------
static u16 kbdMapBuf[32 * 64] __attribute__((aligned(4))); // The two maps are 2K in size

// ---------------------------------------------------------------------------
// Setup the bottom screen with the correct virtual keyboard.
// ---------------------------------------------------------------------------
void BottomScreenKeypad(void)
{
    if (myGlobalConfig.debugger) // Full Z80 Debug overrides things... put up the debugger overlay
    {
        //  Init bottom screen
        decompress(debug_ovlTiles, bgGetGfxPtr(bg0b), LZ77Vram);
        decompress(debug_ovlMap, (void *)bgGetMapPtr(bg0b), LZ77Vram);
        dmaCopy((void *)bgGetMapPtr(bg0b) + 32 * 30 * 2, (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);
        dmaCopy((void *)debug_ovlPal, (void *)BG_PALETTE_SUB, 256 * 2);
    }
    else // Must be OVL_FULLKBD
    {
        const void *tiles;
        const void *map;
        const u16 *pal;

        tiles = msx_kbdTiles;
        map = msx_kbdMap;
        pal = msx_kbdPal;

        // --------------------------------------------------------------
        // Decompress into RAM and then do the copy... this prevents the
        // graphics from flashing/garbage while the decompress happens.
        // --------------------------------------------------------------
        decompress(tiles, COMPRESS_BUFFER, LZ77);
        decompress(map, kbdMapBuf, LZ77);

        // Get uncompressed byte sizes from the LZ77 headers.
        const u8 *t = (const u8 *)tiles;
        const u8 *m = (const u8 *)map;

        u32 tileSize = t[1] | ((u32)t[2] << 8) | ((u32)t[3] << 16);
        u32 mapSize = m[1] | ((u32)m[2] << 8) | ((u32)m[3] << 16);

        DC_FlushRange(COMPRESS_BUFFER, tileSize);
        DC_FlushRange(kbdMapBuf, mapSize);

        swiWaitForVBlank(); // Draw during VBLANK to avoid artifacts

        bgHide(bg0b);
        bgHide(bg1b);

        // Copy only the actual decompressed data.
        dmaCopy(COMPRESS_BUFFER, bgGetGfxPtr(bg0b), tileSize);
        dmaCopy(kbdMapBuf, bgGetMapPtr(bg0b), mapSize);

        // Preserve the original map offset and copy size.
        dmaCopy((void *)bgGetMapPtr(bg0b) + 32 * 30 * 2,
                (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);

        dmaCopy((void *)pal, (void *)BG_PALETTE_SUB, 256 * 2);

        bgShow(bg0b);
        bgShow(bg1b);
    }

    unsigned short dmaVal = *(bgGetMapPtr(bg1b) + 24 * 32);
    dmaFillWords(dmaVal | (dmaVal << 16), (void *)bgGetMapPtr(bg1b), 32 * 24 * 2);

    DisplayStatusLine();
}

/*********************************************************************************
 * Init CPU for the current game
 ********************************************************************************/
void IllusionDSInitCPU(void)
{
    //  -----------------------------------------
    //  Init Main Memory and VDP Video Memory
    //  -----------------------------------------
    memset(RAM_Memory, 0x00, RAM_MEMORY_SIZE);
    memset(VDP_Memory, 0x00, sizeof(VDP_Memory));

    // -----------------------------------------------
    // Init bottom screen do display correct overlay
    // -----------------------------------------------
    BottomScreenKeypad();

    // -----------------------------------------------------
    //  Load the correct Bios ROM for the given machine
    // -----------------------------------------------------
    msx_restore_bios();
}

// -----------------------------------------------------------------
// This is where we handle our pan/scan up and down as well as
// some basic vSYNC timing so that we lock the DS screen updates
// and the MSX screen updates to avoid tearing as much as possible.
// -----------------------------------------------------------------
ITCM_CODE void irqVBlank(void)
{
    int ydyBG = 0x0100; // Default to no screen scale
    int shifty = 0;
    int cyBG = 0;

    // Manage time and true vSync on display output to reduce tearing...
    vusCptVBL++;
    dsVSyncCount++;
    
    // ---------------------------------------------------------------------
    // Are we in 192 scanline mode? If so, we don't need ANY screen panning.
    // ---------------------------------------------------------------------
    if (!(VDP[9] & 0x80)) // High bit set if we are in 212 mode
    {
        cyBG = 0;   // No shift
        shifty = 0; // No shift
    }
    else
    {
        cyBG = ((s16)myConfig.yOffset) << 8;

        // --------------------------------------------------------------------
        // Compress screen (yuck!). We do a little bit of DS magic here to
        // shift one of the two DS video pointers so that we end up with a
        // tiny bit of vertical blurring that helps games that have static
        // screens so that some lines repeat a pixel rather than just losing
        // it completely. This produces squatter/fatter text but generally
        // looks better than a line of missing pixels. It's not perfect.
        // --------------------------------------------------------------------
        ydyBG = ((212 / 192) << 8) | (212 % 192);
        if (myConfig.yOffset == 0)
        {
            cyBG = (1 << 8);
        }
        shifty = 20;
    }

    REG_BG2Y = cyBG + shifty;
    REG_BG3Y = cyBG;

    REG_BG2PD = ydyBG;
    REG_BG3PD = ydyBG;
}

/*********************************************************************************
 * Program entry point - check if an argument has been passed in probably from TWL++
 ********************************************************************************/
int main(int argc, char **argv)
{
    // Init sound so we get an intro jingle...
    consoleDemoInit();

    // Initialize the old FAT16 library for use with the SD card
    if (!fatInitDefault())
    {
        iprintf("Unable to initialize libfat!\n");
        return -1;
    }

    // -----------------------------------------------------------------
    // Allocate the Large DSi buffer for expanded ASCII16-X banking...
    // -----------------------------------------------------------------
    if (isDSiMode())
    {
        ROM_Memory = malloc(MAX_CART_SIZE_KB * 1024);
    }
    else // Non DSi/XL/LL is not supported for Illusions-DS
    {
        ShowWarning(1);
        
        for (;;)
        {
            WAITVBL;
        }        
    }
    
    // The RAM for our fake Turbo-R is 512K
    RAM_Memory = malloc(512 * 1024);

    // Clear out the ROM memory with all 0xFF values...
    memset(ROM_Memory, 0xFF, (MAX_CART_SIZE_KB * 1024));

    // ------------------------------------------------------
    // Both DS-Lite and DSi get a single Disk buffer of 720K
    // ------------------------------------------------------
    DISK_Memory = malloc(MAX_DISK_SIZE_KB * 1024);
    memset(DISK_Memory, 0xFF, (MAX_DISK_SIZE_KB * 1024));

    // Make sure the DISK memory is reset...
    memset(DISK_Memory, 0xFF, (MAX_DISK_SIZE_KB * 1024));

    // Make sure the SRAM is reset... This small buffer is pre-allocated.
    memset(SRAM_Memory, 0xFF, sizeof(SRAM_Memory));

    // ---------------------------------------------------------------
    // The main game action is on the top screen, keyboard on bottom
    // ---------------------------------------------------------------
    lcdMainOnTop();

    //  Init timer for frame management
    TIMER2_DATA = 0;
    TIMER2_CR = TIMER_ENABLE | TIMER_DIV_1024;

    // Install the sound driver for emulated AY/SCC sound
    SoundPause();
    dsInstallSoundEmuFIFO();

    //  Show the fade-away intro logo...
    intro_logo();

    SetYtrigger(180); // trigger 12 lines before true DS vsync

    irqSet(IRQ_VBLANK, irqVBlank);
    irqEnable(IRQ_VBLANK);

    // -------------------------------------------
    // Setup our DS VRAM - most banks go unused.
    // -------------------------------------------
    useVRAM();

    // -----------------------------------------------------------------
    // And do an initial load of configuration... We'll match it up
    // with the game that was selected later... Mostly need globals.
    // -----------------------------------------------------------------
    LoadConfigDatabase();

    // ----------------------------------------
    // Get into the best directory possible...
    // ----------------------------------------
    chdir("/roms"); // Try to start in roms area... doesn't matter if it fails

    // -----------------------------------------------------
    // Make sure we have true-ish random number generation
    // -----------------------------------------------------
    srand(time(NULL));
    
    //  ------------------------------------------------------------
    //  We run this loop forever until game exit is selected...
    //  ------------------------------------------------------------
    while (1)
    {
        IllusionDSInit();
        
        // Read in the one Illusion City ASCII16-X ROM
        // Try with and without spaces...
        if (LoadGameIntoMedia(MEDIA_CART1, "Illusion City.rom") == 0)
        {
            if (LoadGameIntoMedia(MEDIA_CART1, "IllusionCity.rom") == 0)
            {
                ShowWarning(2);
                for (;;) {WAITVBL;}
            }
        }

        while (1)
        {
            SoundPause();

            //  Choose carts and disks and options
            IllusionDSMainMenu();

            //  Run Machine
            IllusionDSInitCPU();
            IllusionDSRunEmu();
        }
    }
    return (0);
}

// -----------------------------------------------------------------------------------------------------------
// Used by the MSX handler to point to different 8K segments of memory as RAM and Carts are swapped in/out.
// This is one of the most important data structures in our system as it universally maps what to read/write.
// Please note: to gain an almost 5% speed-up, wherever these are written, we offset them by the appropriate
// amount. So MemoryMap[0] is offset by -0x0000 and MemoryMap[3] is offset by -0x6000, etc. This way we don't
// need to do any masking when we fetch bytes in the Z80 and those fetches happen a million times per second.
// -----------------------------------------------------------------------------------------------------------
u8 *MemoryMap[8] __attribute__((section(".dtcm"))) = {0, 0, 0, 0, 0, 0, 0, 0};

// ---------------------------------------
// The main Peripheral Interface IO Ports
// ---------------------------------------
u8 Port_PPI_A __attribute__((section(".dtcm"))) = 0x00;
u8 Port_PPI_B __attribute__((section(".dtcm"))) = 0x00;
u8 Port_PPI_C __attribute__((section(".dtcm"))) = 0x00;

// -------------------------------------
// Our venerable Z80 CPU structure!
// -------------------------------------
Z80 CPU __attribute__((section(".dtcm"))); // Put the entire CPU state into fast memory for speed!

// -----------------------------------------
// Some keyboard/joystick mapping vars...
// -----------------------------------------
u8 JoyMode __attribute__((section(".dtcm"))) = 0;   // Joystick Mode (1=Keypad, 0=Joystick)
u32 JoyState __attribute__((section(".dtcm"))) = 0; // Joystick State for P1 and P2


/*********************************************************************************
 * Init MSX Emulation Engine for that game
 ********************************************************************************/
void msxInit(void)
{
    // We've got some debug data we can use for development... reset these.
    memset(debug, 0x00, sizeof(debug));
    DX = DY = 0;

    // Put up the proper bottom keyboard overlay/graphic
    BottomScreenKeypad();

    // -----------------------------------------------------------------
    // Change graphic mode to initiate emulation.
    // Here we can claim back 128K of VRAM which is otherwise unused
    // but we can use it for fast memory swaps and look-up-tables.
    // -----------------------------------------------------------------
    videoSetMode(MODE_5_2D | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000); // Bank A starts at 0x06000000
    vramSetBankB(VRAM_B_MAIN_BG_0x06020000); // Bank B starts at 0x06020000

    REG_BG2CNT = BG_BMP8_256x256 | BG_BMP_BASE(0); // Uses Bank A
    REG_BG3CNT = BG_BMP8_256x256 | BG_BMP_BASE(8); // Uses Bank B

    // Alpha blending configuration
    REG_BLDCNT = BLEND_ALPHA | BLEND_SRC_BG2 | BLEND_DST_BG3;

    // EVA = 8, EVB = 8 (50/50 blend)
    REG_BLDALPHA = (8) | (8 << 8);

    REG_BG2PA = (1 << 8);
    REG_BG2PB = 0;
    REG_BG2PC = 0;
    REG_BG2PD = (1 << 8);
    REG_BG2X = 0;
    REG_BG2Y = 0;

    REG_BG3PA = (1 << 8);
    REG_BG3PB = 0;
    REG_BG3PC = 0;
    REG_BG3PD = (1 << 8);
    REG_BG3X = 0;
    REG_BG3Y = 0;

    memset((u8 *)0x06000000, 0x00, 0x40000); // Ensure screen is clear... (LCD_A and LCD_B)    
    
    // Wipe RAM area for the MSX
    msxWipeRAM();

    // Perform a standard system RESET
    ResetMSX();
}

/*********************************************************************************
 * Update the screen for the current cycle. On the DSi this will generally
 * be called right after swiWaitForVBlank() in VDP9938a.c which will help
 * reduce visual tearing and other artifacts. It's not strictly necessary
 * and that does slow down the loop a bit... but DSi can handle it.
 ********************************************************************************/
void msxUpdateScreen(void)
{
    if (!skip_render)
    {
        dmaCopyWordsAsynch(2, (u32 *)XBuf[(drawn_frame_number + 1) & 1], (u32 *)DS_LCD_VRAM_1, (256 * 212));
    }

    skip_render = 0;
}

// -------------------------------------------------------------------------
// Return the master CRC32 used for high scores and Configuration. This is
// always the Cart 1 unless that's not loaded in which case it's the Disk.
// We never use Cart 2 as the master slot (only used for cart-combos).
// -------------------------------------------------------------------------
u32 GetMasterCRC(void)
{
    if (MyMedia[MEDIA_CART1].filecrc)
    {
        return MyMedia[MEDIA_CART1].filecrc;
    }
    else if (MyMedia[MEDIA_DISK].filecrc)
    {
        return MyMedia[MEDIA_DISK].filecrc;
    }

    return 0x00000000;
}

// -------------------------------------------------------------------------
// Return the master filename needed as the base filename for .sav and .srm
// Always the Cart 1 unless that's not loaded in which case it's the Disk.
// We never use Cart 2 as the master slot (only used for cart-combos).
// -------------------------------------------------------------------------
char *GetMasterFilename(void)
{
    if (MyMedia[MEDIA_CART1].filecrc)
    {
        return MyMedia[MEDIA_CART1].filename;
    }
    else if (MyMedia[MEDIA_DISK].filecrc)
    {
        return MyMedia[MEDIA_DISK].filename;
    }

    return "No File";
}

// -------------------------------------------------------------------------
// Return the master path needed as the directory needed for .sav and .srm
// Always the Cart 1 unless that's not loaded in which case it's the Disk.
// We never use Cart 2 as the master slot (only used for cart-combos).
// -------------------------------------------------------------------------
char *GetMasterPath(void)
{
    if (MyMedia[MEDIA_CART1].filecrc)
    {
        return MyMedia[MEDIA_CART1].filepath;
    }
    else if (MyMedia[MEDIA_DISK].filecrc)
    {
        return MyMedia[MEDIA_DISK].filepath;
    }

    return "";
}

/** LoadGameIntoMedia() **************************************************************/
/** Open a rom file from file system and load it into the appropriate buffer...     **/
/** After this call, the filename, file size and CRC32 are set for this media slot. **/
/*************************************************************************************/
u32 LoadGameIntoMedia(u8 media_id, char *filename)
{
    // Save the directory in which this file was found...
    getcwd(MyMedia[media_id].filepath, MAX_PATH_NAME_LEN);

    // Save the current game filename. We uppercase it so it's easier to search the title...
    strcpy(MyMedia[media_id].filename, filename);
    for (int i = 0; i < strlen(MyMedia[media_id].filename); i++)
    {
        MyMedia[media_id].filename[i] = toupper(MyMedia[media_id].filename[i]); // Uppercase string
    }

    // Now open and read the file... grab file size and CRC32 as well...
    if (media_id == MEDIA_CART1)
    {
        MyMedia[media_id].filesize = ReadFileCarefully(MyMedia[media_id].filename, ROM_Memory, (MAX_CART_SIZE_KB * 1024), 0, &MyMedia[media_id].filecrc);
    }
    else if (media_id == MEDIA_CART2)
    {
        MyMedia[media_id].filesize = ReadFileCarefully(MyMedia[media_id].filename, ROM_Memory + ((MAX_CART_SIZE_KB * 1024) / 2), (MAX_CART_SIZE_KB * 1024) / 2, 0, &MyMedia[media_id].filecrc);
    }
    else // Must be MEDIA_DISK
    {
        strcpy(MyMedia[MEDIA_DISK].currdisk, MyMedia[media_id].filename); // This is the current disk name - since disks can be wapped.
        MyMedia[media_id].filesize = ReadFileCarefully(MyMedia[media_id].filename, DISK_Memory, (MAX_DISK_SIZE_KB * 1024), 0, &MyMedia[media_id].filecrc);
    }
    
    // --------------------------------------------------------
    // Load the Configuration for this new cart/disk layout...
    // --------------------------------------------------------
    FindConfig();

    return MyMedia[media_id].filesize;
}

// -------------------------------------------------------------------------
// For various machines, we have patched the BIOS so that we trap calls
// to various I/O routines: namely cassette access. We handle that here.
// -------------------------------------------------------------------------
void PatchZ80(register Z80 *r)
{
    // Not used at this time...
}

/** LoopZ80() *************************************************/
/** Z80 emulation calls this function periodically to run    **/
/** Z80 code for the loaded ROM. It runs code refreshing the **/
/** VDP and checking for interrupt requests.                 **/
/**************************************************************/
u32 LoopZ80(void)
{
    // Execute 1 scanline worth of CPU instructions
    extern u8 s1990_reg[];
    u32 cycles_to_process = VDP9938_CLOCKS_PER_LINE + CPU.CycleDeficit;
    
    // -------------------------------------------------------------------------------
    // And here is some fake Turbo-R magic... we are essentially giving the
    // core Z80 more cycles to work with to simulate a semi-Turbo-R experience.
    // This is NOT accurate emulation but fortunately Illusion City generally
    // doesn't use any Turbo-R specific CPU / R800 functionality so this kinda works!
    // -------------------------------------------------------------------------------
    if (s1990_reg[6] & 0x40) // DRAM mode if bit is set (faster Z80)
    {
        if (s1990_reg[6] & 0x20) // R800 mode if bit is clear
        {
            cycles_to_process += VDP9938_CLOCKS_PER_LINE + 57; // 2.25x Z80 mode
        }
        else // R800 turbo mode... best we can do is 3x and remember we don't really have an R800 processor!
        {
            cycles_to_process += (VDP9938_CLOCKS_PER_LINE * 2); // 3x Z80 mode
        }    
    }
    
    CPU.CycleDeficit = ExecZ80(cycles_to_process);
    
    turbo_timer += 16;  // Roughly 16.3 ticks per scanline... good enough

    // Run the FDC engine
    LoopFDC();

    // Run the VDP engine
    LoopVDP();

    // Refresh VDP
    Loop9938();

    // Generate an interrupt if called for...
    if (CPU.IRequest != INT_NONE)
    {
        IntZ80(&CPU, CPU.IRequest);
        CPU.NumInts++;  // Track Interrupt Requests
    }

    // Drop out unless end of screen is reached
    if (CurLine == VDP9938_END_LINE)
    {
        CPU.CycleDeficit = 0; // Start fresh at the VBlank boundary. Keeps frame timing consistent.
        return 0;
    }
    return 1;
}

// -----------------------------------------------------------------------
// A bit of memory reserved to store debug information... Reduce this
// when we are stable and don't have much need for it anymore.
// -----------------------------------------------------------------------

#define MAX_DPRINTF_STR_SIZE 128
#define MAX_DEBUG_BUF_SIZE (32 * 1024)

char DEBUG_BUFFER[MAX_DEBUG_BUF_SIZE];
u32 debug_len = 0;
extern char szName[]; // Reuse buffer which has no other in-game use

void debug_init()
{
    debug_len = 0;
    memset(DEBUG_BUFFER, 0x00, MAX_DEBUG_BUF_SIZE);
}

void debug_printf(const char *str, ...)
{
    if (debug_len >= (MAX_DEBUG_BUF_SIZE - MAX_DPRINTF_STR_SIZE))
    {
        return; // No more room!
    }

    va_list ap = {0};

    va_start(ap, str);
    vsnprintf(szName, MAX_DPRINTF_STR_SIZE, str, ap);
    va_end(ap);

    strcat(DEBUG_BUFFER + debug_len, szName);
    debug_len += strlen(szName);
}

void debug_save()
{
    if (debug_len > 0) // Only if we have debug data to write...
    {
        FILE *fp = fopen("debug.log", "w");
        if (fp)
        {
            fwrite(DEBUG_BUFFER, 1, debug_len, fp);
            fclose(fp);
        }
    }
}

// End of file
