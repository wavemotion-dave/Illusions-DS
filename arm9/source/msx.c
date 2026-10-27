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
#include "printf.h"

// -------------------------------------------------------------
// Some MSX Mapper / Slot Handling and sound generation stuff...
// -------------------------------------------------------------
u8 bCartInPage[4]           __attribute__((section(".dtcm"))) = {0,0,0,0};
u8 bRAMInPage[4]            __attribute__((section(".dtcm"))) = {0,0,0,0};

u8 *MSXCartPtr[2][8]        __attribute__((section(".dtcm"))) = { {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0} };
u8 *MSXRamPtr[8]            __attribute__((section(".dtcm"))) = {0,0,0,0,0,0,0,0};

u8 msx_music_capable_game   __attribute__((section(".dtcm"))) = 0;      // True if game is making use of MSX-MUSIC (YM Sound)
u8 special_memory_access    __attribute__((section(".dtcm"))) = 0x00;   // This lets us know we've got something special mapped into memory
u32 msx_music_writes        __attribute__((section(".dtcm"))) = 0;      // If we cross a threshold, we decare we are MSX-MUSIC capable (and mix in the proper sound)
u8 msx_subslot              __attribute__((section(".dtcm"))) = 0x00;   // Only one slot is expanded so we only need to track one register here
u8 msx_subslot0             __attribute__((section(".dtcm"))) = 0x00;   // Only one slot is expanded so we only need to track one register here
u16 turbo_timer             __attribute__((section(".dtcm"))) = 0x0000; // Rough Turbo-R timer handling - advanced 16 ticks every scanline.
AY38910 myAY                __attribute__((section(".dtcm")));          // Declare new AY structure for basic MSX sounds
YM      myYM                __attribute__((section(".dtcm")));          // Declare new YM module (MSX-MUSIC) for MSX games that use it

// Some Turbo-R Specific handlers... very bare bones!
u8 last_f4 = 0x00;
u8 s1990_reg_idx=0;
u8 s1990_reg[16] = {0};

// --------------------------------------------------------------------------
// These aren't used very often so we don't need them in fast .dtcm memory
// --------------------------------------------------------------------------
u8 mirror_ram_bank[4]   = {3,2,1,0}; // For port readback mostly... though not all MSX2 machines report it back.

u8 Unmapped_Memory[0x2000]; // Full of 0xFF values. We point all memory segments here that are not mapped to some other device.

static uint8_t rtc_reg  = 0;      // Selected register index (0-15)
static uint8_t rtc_bank = 0;      // Active bank selected by Reg 13 (0-3)

// Complete 4-bank RAM array initialized with valid MSX2 checksums
static uint8_t rtc_ram[4][16] = {
    // Bank 0: Time & Date Tracking
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0, 0x01, 0, 0 },

    // Bank 1: Alarm Settings (Do not place System/Screen settings here)
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0, 0, 0, 0 },

    // Bank 2: MSX system initialization settings
    {
        0x0A,  // Reg 0: RTC settings valid — REQUIRED
        0x00,  // Reg 1: X adjust
        0x00,  // Reg 2: Y adjust
        0x00,  // Reg 3: SCREEN 0, normal display
        0x08,  // Reg 4: WIDTH low nibble
        0x02,  // Reg 5: WIDTH high bits (40 columns)
        0x0F,  // Reg 6: Initial text color
        0x04,  // Reg 7: Initial background color
        0x04,  // Reg 8: Initial border color
        0x00,  // Reg 9: Default transfer/keyboard options
        0x00,  // Reg 10: BEEP settings
        0x00,  // Reg 11: Startup logo colors
        0x01,  // Reg 12: Area code (US)
        0x00, 0x00, 0x00
    },

    // Bank 3: Color Palette Settings + General Validation Checksums
    { 0x04, 0x04, 0x07, 0x05, 0x02, 0x02, 0x07, 0x07,
      0x01, 0x01, 0x03, 0x03, 0x08, 0x05, 0x00, 0x00 }
};

// WRITE PORT 0xB4: Selects Register Index ONLY
__attribute__((noinline)) void write_port_RTC_index(uint8_t data)
{
    rtc_reg = data & 0x0F; // Only lower 4 bits pick the register!
}

// WRITE PORT 0xB5: Writes Data into the current Bank & Register
__attribute__((noinline))  void write_port_RTC_data(uint8_t data)
{
    uint8_t val = data & 0x0F;

    // Store data in current active bank
    rtc_ram[rtc_bank][rtc_reg] = val;

    // IF writing to Register 13 (Mode Register), update the ACTIVE BANK!
    if (rtc_reg == 13 || rtc_reg == 0x0D) {
        rtc_bank = val & 0x03; // Bits 0-1 select Banks 0-3
    }
}

// RTC Register Layout Constants for the date/time handling
#define R_1_SEC    0
#define R_10_SEC   1
#define R_1_MIN    2
#define R_10_MIN   3
#define R_1_HOUR   4
#define R_10_HOUR  5
#define R_WEEK     6
#define R_1_DAY    7
#define R_10_DAY   8
#define R_1_MON    9
#define R_10_MON   10
#define R_1_YEAR   11
#define R_10_YEAR  12

/**
 * Splits a decimal value into individual 4-bit BCD nibbles
 * and stores them sequentially into the RTC RAM array.
 */
void store_nibbles(uint8_t rtc_sub_ram[], int index_1, int index_10, int val)
{
    rtc_sub_ram[index_1]  = (uint8_t)(val % 10);        // Lower 4-bits (Units)
    rtc_sub_ram[index_10] = (uint8_t)((val / 10) % 10); // Upper 4-bits (Tens)
}

// ---------------------------------------------------------------------------------------
// Grab system time/date and populate RTC registers... unlikely any program really cares.
// ---------------------------------------------------------------------------------------
void populate_msx_rtc()
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // 1. Time Components
    store_nibbles(rtc_ram[0], R_1_SEC, R_10_SEC, t->tm_sec);
    store_nibbles(rtc_ram[0], R_1_MIN, R_10_MIN, t->tm_min);
    store_nibbles(rtc_ram[0], R_1_HOUR, R_10_HOUR, t->tm_hour);

    // 2. Day of the Week (0 = Sunday, 6 = Saturday)
    // Matches the native C struct tm standard perfectly for MSX2.
    rtc_ram[0][R_WEEK] = (uint8_t)t->tm_wday;

    // 3. Calendar Components (struct tm uses 0-11 for months; RP5C01 expects 1-12)
    store_nibbles(rtc_ram[0], R_1_DAY, R_10_DAY, t->tm_mday);
    store_nibbles(rtc_ram[0], R_1_MON, R_10_MON, t->tm_mon + 1);

    // 4. Year Tracking (2-digit, 00-99)
    int year_2_digits = t->tm_year % 100;
    store_nibbles(rtc_ram[0], R_1_YEAR, R_10_YEAR, year_2_digits);
}

// READ PORT 0xB5: Reads Data from current Bank & Register
__attribute__((noinline)) uint8_t read_port_RTC_data(void)
{
    populate_msx_rtc(); // Get current system time/date and populate RTC

    uint8_t val = rtc_ram[rtc_bank][rtc_reg] & 0x0F;

    // Reg 13 (Mode Register): Force BUSY flag (Bit 1) to 0
    if (rtc_reg == 13)
    {
        val &= ~0x02; // Bit 1 = 0 (NOT BUSY)
    }

    return val;
}

// ------------------------------------------------------------------------------------------
// Keyboard / Joystick reading is generally done every frame so at most 60 times per second.
// It's fine to keep this out of ITCM fast memory as it's not a hot spot for emulation.
// ------------------------------------------------------------------------------------------
u8 readport_keyboard(void)
{
      // ----------------------------------------------------------
      // Keyboard Port (Japanese Layout)
      //  Row   Bit_7 Bit_6 Bit_5 Bit_4 Bit_3 Bit_2 Bit_1 Bit_0
      //   0     "7"   "6"   "5"   "4"   "3"   "2"   "1"   "0"
      //   1     ":"   ";"   "["   YEN   "="   "-"   "9"   "8"
      //   2     "B"   "A"  DEAD   /     "."   ","   "]"  QUOTE
      //   3     "J"   "I"   "H"   "G"   "F"   "E"   "D"   "C"
      //   4     "R"   "Q"   "P"   "O"   "N"   "M"   "L"   "K"
      //   5     "Z"   "Y"   "X"   "W"   "V"   "U"   "T"   "S"
      //   6     F3    F2    F1   KANA   CAPS GRAPH CTRL  SHIFT
      //   7     RET   SEL   BS   STOP   TAB   ESC   F5    F4
      //   8    RIGHT DOWN   UP   LEFT   DEL   INS  HOME  SPACE
      // ----------------------------------------------------------

      // For the full keyboard overlay... this is a bit of a hack for SHIFT and CTRL
      if (last_special_key != 0)
      {
          if ((last_special_key_dampen > 0) && (last_special_key_dampen != 20))
          {
              if (--last_special_key_dampen == 0)
              {
                  last_special_key = 0;
              }
          }

          if (last_special_key == KBD_KEY_SHIFT)
          {
            key_shift = 1;
          }
          else if (last_special_key == KBD_KEY_CTRL)
          {
            key_ctrl = 1;
          }
          else if (last_special_key == KBD_KEY_KANA)
          {
            key_kana = 1;
          }
          else if (last_special_key == KBD_KEY_GRAPH)
          {
            key_graph = 1;
          }

          if ((kbd_key != 0) && (kbd_key != KBD_KEY_SHIFT) && (kbd_key != KBD_KEY_CTRL) && (kbd_key != KBD_KEY_KANA) && (kbd_key != KBD_KEY_GRAPH))
          {
              if (last_special_key_dampen == 20) last_special_key_dampen = 19;    // Start the SHIFT/CONTROL countdown... this should be enough time for it to register
          }
      }

      u8 key1 = 0x00;   // Accumulate keys here...

      // -------------------------------------------------
      // Check every key that might have been pressed...
      // -------------------------------------------------
      for (u8 i=0; i< (kbd_keys_pressed ? kbd_keys_pressed:1); i++) // Always one pass at least for joysticks...
      {
          kbd_key = kbd_keys[i];

          if ((Port_PPI_C & 0x0F) == 0)      // Row 0
          {
              if (kbd_key)
              {
                  if (kbd_key == '0')           key1  |= 0x01;
                  if (kbd_key == '1')           key1  |= 0x02;
                  if (kbd_key == '2')           key1  |= 0x04;
                  if (kbd_key == '3')           key1  |= 0x08;
                  if (kbd_key == '4')           key1  |= 0x10;
                  if (kbd_key == '5')           key1  |= 0x20;
                  if (kbd_key == '6')           key1  |= 0x40;
                  if (kbd_key == '7')           key1  |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 1)  // Row 1
          {
              if (kbd_key)
              {
                  if (kbd_key == '8')           key1 |= 0x01;
                  if (kbd_key == '9')           key1 |= 0x02;
                  if (kbd_key == '-')           key1 |= 0x04;
                  if (kbd_key == '=')           key1 |= 0x08;
                  if (kbd_key == '|')           key1 |= 0x10; // YEN
                  if (kbd_key == '@')           key1 |= 0x20;
                  if (kbd_key == '[')           key1 |= 0x40;
                  if (kbd_key == ';')           key1 |= 0x80;
                  if (kbd_key == ':')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 2)  // Row 2
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_QUOTE) key1 |= 0x01;
                  if (kbd_key == ']')           key1 |= 0x02;
                  if (kbd_key == ',')           key1 |= 0x04;
                  if (kbd_key == '.')           key1 |= 0x08;
                  if (kbd_key == '/')           key1 |= 0x10;
                  if (kbd_key == KBD_KEY_DEAD)  key1 |= 0x20;
                  if (kbd_key == 'A')           key1 |= 0x40;
                  if (kbd_key == 'B')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 3)  // Row 3
          {
              if (kbd_key)
              {
                  if (kbd_key == 'C')           key1 |= 0x01;
                  if (kbd_key == 'D')           key1 |= 0x02;
                  if (kbd_key == 'E')           key1 |= 0x04;
                  if (kbd_key == 'F')           key1 |= 0x08;
                  if (kbd_key == 'G')           key1 |= 0x10;
                  if (kbd_key == 'H')           key1 |= 0x20;
                  if (kbd_key == 'I')           key1 |= 0x40;
                  if (kbd_key == 'J')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 4)  // Row 4
          {
              if (kbd_key)
              {
                  if (kbd_key == 'K')           key1 |= 0x01;
                  if (kbd_key == 'L')           key1 |= 0x02;
                  if (kbd_key == 'M')           key1 |= 0x04;
                  if (kbd_key == 'N')           key1 |= 0x08;
                  if (kbd_key == 'O')           key1 |= 0x10;
                  if (kbd_key == 'P')           key1 |= 0x20;
                  if (kbd_key == 'Q')           key1 |= 0x40;
                  if (kbd_key == 'R')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 5)  // Row 5
          {
              if (kbd_key)
              {
                  if (kbd_key == 'S')           key1 |= 0x01;
                  if (kbd_key == 'T')           key1 |= 0x02;
                  if (kbd_key == 'U')           key1 |= 0x04;
                  if (kbd_key == 'V')           key1 |= 0x08;
                  if (kbd_key == 'W')           key1 |= 0x10;
                  if (kbd_key == 'X')           key1 |= 0x20;
                  if (kbd_key == 'Y')           key1 |= 0x40;
                  if (kbd_key == 'Z')           key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 6) // Row 6
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_SHIFT) key1 |= 0x01;
                  if (kbd_key == KBD_KEY_CTRL)  key1 |= 0x02;
                  if (kbd_key == KBD_KEY_GRAPH) key1 |= 0x04;
                  if (kbd_key == KBD_KEY_CAPS)  key1 |= 0x08;
                  if (kbd_key == KBD_KEY_KANA)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_F1)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_F2)    key1 |= 0x40;
                  if (kbd_key == KBD_KEY_F3)    key1 |= 0x80;
              }
              if (key_shift)  key1 |= 0x01;  // SHIFT
              if (key_ctrl)   key1 |= 0x02;  // CTRL
              if (key_graph)  key1 |= 0x04;  // GRAPH
              if (key_kana)   key1 |= 0x10;  // KANA
          }
          else if ((Port_PPI_C & 0x0F) == 7) // Row 7
          {
              if (kbd_key)
              {
                  if (kbd_key == KBD_KEY_F4)    key1 |= 0x01;
                  if (kbd_key == KBD_KEY_F5)    key1 |= 0x02;
                  if (kbd_key == KBD_KEY_ESC)   key1 |= 0x04;
                  if (kbd_key == KBD_KEY_TAB)   key1 |= 0x08;
                  if (kbd_key == KBD_KEY_STOP)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_BS)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_SEL)   key1 |= 0x40;
                  if (kbd_key == KBD_KEY_RET)   key1 |= 0x80;
              }
          }
          else if ((Port_PPI_C & 0x0F) == 8) // Row 8
          {
              if (kbd_key)
              {
                  if (kbd_key == ' ')           key1 |= 0x01;
                  if (kbd_key == KBD_KEY_HOME)  key1 |= 0x02;
                  if (kbd_key == KBD_KEY_INS)   key1 |= 0x04;
                  if (kbd_key == KBD_KEY_DEL)   key1 |= 0x08;
                  if (kbd_key == KBD_KEY_LEFT)  key1 |= 0x10;
                  if (kbd_key == KBD_KEY_UP)    key1 |= 0x20;
                  if (kbd_key == KBD_KEY_DOWN)  key1 |= 0x40;
                  if (kbd_key == KBD_KEY_RIGHT) key1 |= 0x80;
              }
          }
      }
      return ~key1;
}


// --------------------------------------------------------------------
// MSX IO Port Read - The MSX has a lot of I/O mapped peripherals
// including Joystick, PSG, SCC, RTC, keyboard, etc.
// --------------------------------------------------------------------
ITCM_CODE unsigned char cpu_readport_msx(register u8 Port)
{
  //98h~9Bh   Access to the VDP I/O ports.
  if      (Port == 0x98) return RdData9938();               // VDP Data
  else if (Port == 0x99) return RdCtrl9938();               // VDP Control (Status)
  else if (Port == 0xB5) {return read_port_RTC_data();}     // RTC Data
  else if (Port == 0xA2)  // PSG Read... might be joypad data
  {
      // -------------------------------------------
      // Only port 1 is used for the first Joystick
      // -------------------------------------------
      if (myAY.ayRegIndex == 14)
      {
          u8 joy1 = 0x00;

          // -------------------------------------------------------------
          // Only port 1... not port 2. AY register 15 (PortB) bit 6 is
          // set to 0 for the port 1 joystick and that's the only one
          // this emulator will respond to...
          // -------------------------------------------------------------
          if ((myAY.ayPortBOut & 0x40) == 0)
          {
              if (JoyState & JST_UP)    joy1 |= 0x01;
              if (JoyState & JST_DOWN)  joy1 |= 0x02;
              if (JoyState & JST_LEFT)  joy1 |= 0x04;
              if (JoyState & JST_RIGHT) joy1 |= 0x08;

              if (JoyState & JST_FIRE1) joy1 |= 0x10;
              if (JoyState & JST_FIRE2) joy1 |= 0x20;
          }

          myAY.ayPortAIn = ~joy1;
      }
      else if (myAY.ayRegIndex == 15)
      {
          // When reading PORTB of the PSG, just echo back the last value written (the MSX BIOS needs this as it will preserve the KANA LED bit)
          myAY.ayPortBIn = myAY.ayPortBOut;
      }
      return ay38910DataR(&myAY);
  }
  else if (Port == 0xA8)  // Feedback on Slot mapping
  {
      return Port_PPI_A;
  }
  else if (Port == 0xA9)  // Keyboard read
  {
      return readport_keyboard();
  }
  else if (Port == 0xAA)  // Port C feedback
  {
      return Port_PPI_C;
  }
  else if (Port == 0xA7) // Turbo R Pause switch / R800 LED port
  {
    return 0x00;  // Pause switch released
  }      
  else if (Port == 0xF4) // Turbo R Reset Status
  {
    return last_f4;  // Pause switch released
  }      
  else if (Port >= 0xFC) // Mirror of RAM select. Not all MSX2 machine return this but we do.
  {
      return mirror_ram_bank[Port - 0xFC];
  }
  else if (Port == 0xE4)
  {
      return s1990_reg_idx;
  }
  else if (Port == 0xE5)
  {
      u8 val = s1990_reg[s1990_reg_idx];
      return val;
  }
  else if (Port == 0xE6)
  {
      return turbo_timer & 0xFF;
  }
  else if (Port == 0xE7)
  {
      return (turbo_timer>>8) & 0xFF;
  }
  else if (Port == 0xE9) // MIDI - not used...
  {
      return 0x05;
  }
  else if (Port == 0xF3)
  {
      return ((VDP[0] >> 1) & 0x07) |
       (VDP[1] & 0x18) |
       (VDP[8] & 0x20) |
       ((VDP[25] & 0x18) << 3);
  }
  else // Unknown port read
  {
      //debug[DX++ & 0xF] = Port;
  }

  // No such port
  return(NORAM);
}


//--------------------------------------------------------------------------------------------------
// MSX2 Machine Configuration: Slot 3 is expanded. Slot 2 contains RAM. Slot 1 is main Cartridge.
//--------------------------------------------------------------------------------------------------
// Memory          Slot 0       Slot 1      Slot 2      Slot 3-0   Slot 3-1    Slot 3-2    Slot 3-3
// C000h~FFFFh      ---         Cart1       Cart2       ---        ---         ---         16K RAM
// 8000h~BFFFh      ---         Cart1       Cart2       ---        ---         ---         16K RAM
// 4000h~7FFFh    Main-ROM      Cart1       Cart2       ---        Disk-ROM    MSX-Music   16K RAM
// 0000h~3FFFh    Main-ROM      Cart1       Cart2       Ext-ROM    ---         ---         16K RAM
//--------------------------------------------------------------------------------------------------
void msx_slot_map_turboR(unsigned char Value)
{
    special_memory_access &= ~SPEC_MEM_SUBSLOT_ACTIVE;  // Until proven otherwise below...
    special_memory_access &= ~SPEC_MEM_SUBSLOT0_ACTIVE; // Until proven otherwise below...
    special_memory_access &= ~SPEC_MEM_DISK_CONTROLLER;

    switch ((Value>>0) & 0x03)  // Page 0 [0x0000~0x3FFF]
    {
        case 0x00:  // Slot 0:  Maps to Main BIOS ROM
            if (((msx_subslot0 & 0x03) >> 0) == 0) // Subslot 0 has main ROM
            {
                bCartInPage[0] = 0;
                bRAMInPage[0] = 0;
                MemoryMap[0] = TURBO_BIOS;
                MemoryMap[1] = TURBO_BIOS;
            }
            else
            {
                bCartInPage[0] = 0;
                bRAMInPage[0] = 0;
                MemoryMap[0] = Unmapped_Memory - 0x0000;
                MemoryMap[1] = Unmapped_Memory - 0x2000;
            }            
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[0] = 1;
            bRAMInPage[0] = 0;
            MemoryMap[0] = (u8 *)(MSXCartPtr[MEDIA_CART1][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[MEDIA_CART1][1]) - 0x2000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[0] = 2;
            bRAMInPage[0] = 0;
            MemoryMap[0] = (u8 *)(MSXCartPtr[MEDIA_CART2][0]) - 0x0000;
            MemoryMap[1] = (u8 *)(MSXCartPtr[MEDIA_CART2][1]) - 0x2000;
            break;
        case 0x03:  // Slot 3:  This is an expanded slot... has Extended BIOS and Disk Controller ROMs
            bCartInPage[0] = 0;
            bRAMInPage[0] = 0;
            if (((msx_subslot & 0x03) >> 0) == 1) // Subslot 1 has Extended BIOS
            {
                MemoryMap[0] = (u8 *)TURBO_EXT;
                MemoryMap[1] = (u8 *)TURBO_EXT;
            }
            else if (((msx_subslot & 0x03) >> 0) == 0) // Subslot 0 has RAM Mapper
            {
                bCartInPage[0] = 0;
                bRAMInPage[0] = 1;
                MemoryMap[0] = (u8 *)(MSXRamPtr[0]) - 0x0000;
                MemoryMap[1] = (u8 *)(MSXRamPtr[1]) - 0x2000;
            }
            else // Other subslots have nothing in this page
            {
                MemoryMap[0] = Unmapped_Memory - 0x0000;
                MemoryMap[1] = Unmapped_Memory - 0x2000;
            }
            break;
    }

    switch ((Value>>2) & 0x03)  // Page 1 [0x4000~0x7FFF]
    {
        case 0x00:  // Slot 0:  Maps to Main BIOS ROM        
            if (((msx_subslot0 & 0x0C) >> 2) == 0) // Subslot 0 has main ROM
            {
                bCartInPage[1] = 0;
                bRAMInPage[1] = 0;
                MemoryMap[2] = TURBO_BIOS;
                MemoryMap[3] = TURBO_BIOS;
            }
            else
            {
                bCartInPage[1] = 0;
                bRAMInPage[1] = 0;
                MemoryMap[2] = Unmapped_Memory - 0x0000;
                MemoryMap[3] = Unmapped_Memory - 0x2000;
            }            
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[1] = 1;
            bRAMInPage[1] = 0;
            MemoryMap[2] = (u8 *)(MSXCartPtr[MEDIA_CART1][2]) - 0x4000;
            MemoryMap[3] = (u8 *)(MSXCartPtr[MEDIA_CART1][3]) - 0x6000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[1] = 2;
            bRAMInPage[1] = 0;
            MemoryMap[2] = (u8 *)(MSXCartPtr[MEDIA_CART2][2]) - 0x4000;
            MemoryMap[3] = (u8 *)(MSXCartPtr[MEDIA_CART2][3]) - 0x6000;
            break;
        case 0x03:  // Slot 3:  Expanded slot has the Disk Controller in subslot 1
            bCartInPage[1] = 0;
            bRAMInPage[1] = 0;
            if (((msx_subslot & 0x0C) >> 2) == 0) // Subslot 0 has RAM Mapper
            {
                bCartInPage[1] = 0;
                bRAMInPage[1] = 1;
                MemoryMap[2] = (u8 *)(MSXRamPtr[2]) - 0x4000;
                MemoryMap[3] = (u8 *)(MSXRamPtr[3]) - 0x6000;
            }
            else if (((msx_subslot & 0x0C) >> 2) == 2) // Subslot 2 has Disk Driver
            {
                special_memory_access |= SPEC_MEM_DISK_CONTROLLER;
                bCartInPage[1] = 0;
                bRAMInPage[1] = 0;
                MemoryMap[2] = (u8 *)TURBO_DISK+0x0000 - 0x4000;
                MemoryMap[3] = (u8 *)TURBO_DISK+0x2000 - 0x6000;
            }
            else // Other subslots have nothing in this page
            {
                MemoryMap[2] = Unmapped_Memory - 0x4000;
                MemoryMap[3] = Unmapped_Memory - 0x6000;
            }
            break;
    }

    switch ((Value>>4) & 0x03)  // Page 2 [0x8000~0xBFFF]
    {
        case 0x00:  // Slot 0:  Maps to nothing... 0xFF
            bCartInPage[2] = 0;
            bRAMInPage[2] = 0;
            MemoryMap[4] = Unmapped_Memory - 0x8000;
            MemoryMap[5] = Unmapped_Memory - 0xA000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[2] = 1;
            bRAMInPage[2] = 0;
            MemoryMap[4] = (u8 *)(MSXCartPtr[MEDIA_CART1][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[MEDIA_CART1][5]) - 0xA000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[2] = 2;
            bRAMInPage[2] = 0;
            MemoryMap[4] = (u8 *)(MSXCartPtr[MEDIA_CART2][4]) - 0x8000;
            MemoryMap[5] = (u8 *)(MSXCartPtr[MEDIA_CART2][5]) - 0xA000;
            break;
        case 0x03:  // Slot 3:  Maps to nothing... 0xFF. Expanded slot but nothing lives here.
            if (((msx_subslot & 0x30) >> 4) == 0) // Subslot 0 has RAM Mapper
            {
                bCartInPage[2] = 0;
                bRAMInPage[2] = 1;
                MemoryMap[4] = (u8 *)(MSXRamPtr[4]) - 0x8000;
                MemoryMap[5] = (u8 *)(MSXRamPtr[5]) - 0xA000;
            }
            else
            {
                bCartInPage[2] = 0;
                bRAMInPage[2] = 0;
                MemoryMap[4] = Unmapped_Memory - 0x8000;
                MemoryMap[5] = Unmapped_Memory - 0xA000;
            }
            break;
    }

    switch ((Value>>6) & 0x03)  // Page 3 [0xC000~0xFFFF]
    {
        case 0x00:  // Slot 0:  Maps to nothing... 0xFF
            special_memory_access |= SPEC_MEM_SUBSLOT0_ACTIVE; // Opens up 0xFFFF
            bCartInPage[3] = 0;
            bRAMInPage[3] = 0;
            MemoryMap[6] = Unmapped_Memory - 0xC000;
            MemoryMap[7] = Unmapped_Memory - 0xE000;
            break;
        case 0x01:  // Slot 1:  Maps to Game Cart
            bCartInPage[3] = 1;
            bRAMInPage[3] = 0;
            MemoryMap[6] = (u8 *)(MSXCartPtr[MEDIA_CART1][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[MEDIA_CART1][7]) - 0xE000;
            break;
        case 0x02:  // Slot 2:  Secondary Cart Slot
            bCartInPage[3] = 2;
            bRAMInPage[3] = 0;
            MemoryMap[6] = (u8 *)(MSXCartPtr[MEDIA_CART2][6]) - 0xC000;
            MemoryMap[7] = (u8 *)(MSXCartPtr[MEDIA_CART2][7]) - 0xE000;
            break;
        case 0x03:  // Slot 3:  Maps to nothing... 0xFF. Expanded slot but nothing lives here.
            special_memory_access |= SPEC_MEM_SUBSLOT_ACTIVE; // Opens up 0xFFFF
            if (((msx_subslot & 0xC0) >> 6) == 0) // Subslot 0 has RAM Mapper
            {
                bCartInPage[3] = 0;
                bRAMInPage[3] = 1;
                MemoryMap[6] = (u8 *)(MSXRamPtr[6]) - 0xC000;
                MemoryMap[7] = (u8 *)(MSXRamPtr[7]) - 0xE000;
            }
            else
            {
                bCartInPage[3] = 0;
                bRAMInPage[3] = 0;
                MemoryMap[6] = Unmapped_Memory - 0xC000;
                MemoryMap[7] = Unmapped_Memory - 0xE000;
            }
            break;
    }
}

// -----------------------------------------------------------------------------------------------
// MSX IO Port Write - VDP and AY Sound Chip, Disk I/O, MSX2 Expanded Memory plus Slot Mapper $A8
// -----------------------------------------------------------------------------------------------
ITCM_CODE void cpu_writeport_msx(register u8 Port,register unsigned char Value)
{
    static u8 msx_music_register = 0;

    if      (Port == 0x98) {WrData9938(Value);}                 // VDP Data
    else if (Port == 0x99) {WrCtrl9938(Value);}                 // VDP Control
    else if (Port == 0x9A) {write_port_palette(Value);}         // VDP Palette
    else if (Port == 0x9B) {IndirectRegWrite9938(Value);}       // Indirect Register Area
    else if (Port == 0xA0) {ay38910IndexW(Value&0xF, &myAY);}   // PSG Area
    else if (Port == 0xA1) {ay38910DataW(Value, &myAY);}        // PSG Area
    else if (Port == 0xB4) {write_port_RTC_index(Value);}       // RTC Area (index register)
    else if (Port == 0xB5) {write_port_RTC_data(Value);}        // RTC Area (data register)
    else if (Port == 0xF4) {last_f4 = Value;}
    else if (Port == 0xE4) // S1990 register index
    {
        s1990_reg_idx = Value & 0xF;
    }
    else if (Port == 0xE5) // S1990 register access
    {
        s1990_reg[s1990_reg_idx] = Value;
    }
    else if (Port == 0xE6) // Reset the Turbo-R timer
    {
        turbo_timer = 0;
    }
    else if (Port == 0xA8) // Slot system for MSX
    {
        msx_slot_map_turboR(Value);
        Port_PPI_A = Value;
    }
    else if (Port == 0xA9)  // PPI - Register B
    {
        Port_PPI_B = Value;
    }
    else if (Port == 0xAA)  // PPI - Register C
    {
        Port_PPI_C = Value;
        msx_caps_lock = ((Port_PPI_C & 0x40) ? 0:1);
    }
    else if (Port == 0xAB)  // PPI - Register C Fast Modify
    {
        // Set or clear the proper bit in PORTC
        u8 bit =  (Value & 0x0E) >> 1;
        if (Value & 1) Port_PPI_C |= (1 << bit);
        else Port_PPI_C &= ~(1 << bit);

        msx_caps_lock = ((Port_PPI_C & 0x40) ? 0:1);
    }
    else if (Port >= 0xFC && Port <= 0xFF) // Expanded Memory...
    {
        u8 page = Port-0xFC;
        u8 bank = Value & 0x1F; // 512K is 16K in 32 banks

        mirror_ram_bank[page] = bank; // For read-back

        MSXRamPtr[(page*2)+0] = RAM_Memory + (0x4000 * bank);
        MSXRamPtr[(page*2)+1] = RAM_Memory + (0x4000 * bank) + 0x2000;

        cpu_writeport_msx(0xA8, Port_PPI_A); // Enable the new map...
    }
    else if (Port == 0x7C)
    {
        msx_music_register = Value;
    }
    else if (Port == 0x7D)
    {
        if (++msx_music_writes == 10) // Arbitrary... if we hit it at least 10 times, we turn on MSX MUSIC output
        {
            msx_music_capable_game = 1;
            bClickRemoveNeeded = 1;
        }

        YMWrite(Value, msx_music_register, &myYM);    // address = resolved register 0x00-0x38, not a Z80 address
    }
    else // Unhandled port write...
    {
        //debug[DX++ & 0xF] = Port;
    }
}


/*********************************************************************************
 * We wipe main RAM with 0x00 values (helps with compression of save states even
 * if not truly accurate to real hardware) and SRAM is wiped to 0xFF values.
 ********************************************************************************/
void msxWipeRAM(void)
{
    memset(RAM_Memory, 0x00, RAM_MEMORY_SIZE);
}

// -------------------------------------------------------------------------
// Setup the initial MSX memory layout based on the size of the ROM loaded.
// -------------------------------------------------------------------------
void MSX_InitialMemoryLayout(void)
{
    // -----------------------------------------------------
    // Fill the unused memory buffer. Anything unmapped in
    // our memory mapping will point here and read 0xFF.
    // -----------------------------------------------------
    for (int i=0; i<0x2000; i++)
    {
        Unmapped_Memory[i] = 0xFF;
    }

    // -------------------------------------
    // Make sure the MSX ports are clear
    // -------------------------------------
    Port_PPI_A = 0x00;
    Port_PPI_B = 0x00;
    Port_PPI_C = 0x00;

    // ----------------------------------
    // Some helper variables for the MSX
    // ----------------------------------
    msx_music_writes = 0;
    special_memory_access = 0x00;
    msx_subslot = 0x00;
    msx_subslot0 = 0x00;

    // -----------------------------------------
    // Setup RAM/ROM pointers back to defaults
    // -----------------------------------------
    memset(bRAMInPage,  0, sizeof(bRAMInPage));   // Default to no RAM paged in until told so
    memset(bCartInPage, 0, sizeof(bCartInPage));  // Default to no ROM paged in until told so

    for (u8 i=0; i<8; i++)
    {
        MSXCartPtr[MEDIA_CART1][i] = Unmapped_Memory;          // Cart1 has nothing in it by default - will fill it in below
        MSXCartPtr[MEDIA_CART2][i] = Unmapped_Memory;          // Cart2 has nothing in it by default - will fill it in below
    }

    // ---------------------------------------------
    // RAM maps backwards... for historical reasons.
    // ---------------------------------------------
    MSXRamPtr[0] = RAM_Memory + 0xC000;
    MSXRamPtr[1] = RAM_Memory + 0xE000;
    MSXRamPtr[2] = RAM_Memory + 0x8000;
    MSXRamPtr[3] = RAM_Memory + 0xA000;
    MSXRamPtr[4] = RAM_Memory + 0x4000;
    MSXRamPtr[5] = RAM_Memory + 0x6000;
    MSXRamPtr[6] = RAM_Memory + 0x0000;
    MSXRamPtr[7] = RAM_Memory + 0x2000;

    // ---------------------------------------------
    // Restore the MSX BIOS and point to it
    // ---------------------------------------------
    msx_restore_bios();
    
    // -----------------------------------------------------------------
    // Illusion City ASCII16-X MegaROM is mapped into Slot 1 (Cart 1)
    // -----------------------------------------------------------------
    u8 *CART_Memory = ROM_Memory;

    MSXCartPtr[MEDIA_CART1][0] = (u8*)CART_Memory+0x4000;       // Segment 0 default
    MSXCartPtr[MEDIA_CART1][1] = (u8*)CART_Memory+0x6000;       // Segment 0 default
    MSXCartPtr[MEDIA_CART1][2] = (u8*)CART_Memory+0x0000;       // Segment 0 default
    MSXCartPtr[MEDIA_CART1][3] = (u8*)CART_Memory+0x2000;       // Segment 0 default
    MSXCartPtr[MEDIA_CART1][4] = (u8*)CART_Memory+0x4000;       // Segment 0 default
    MSXCartPtr[MEDIA_CART1][5] = (u8*)CART_Memory+0x6000;       // Segment 0 default
    MSXCartPtr[MEDIA_CART1][6] = (u8*)CART_Memory+0x0000;       // Segment 0 default
    MSXCartPtr[MEDIA_CART1][7] = (u8*)CART_Memory+0x2000;       // Segment 0 default

    MyMedia[MEDIA_CART1].mapperType = ASC16X;
    MyMedia[MEDIA_CART1].mapperMask = 0xFF;
    MyMedia[MEDIA_CART1].blockSize = 0x4000;    

    // -----------------------------------------------------------------
    // SCC music and FM-PAC are "carts" mapped into Slot 2 (Cart 2)...
    // -----------------------------------------------------------------
    MyMedia[MEDIA_CART2].mapperMask = 0;
    MyMedia[MEDIA_CART2].blockSize = 0x2000;
    MyMedia[MEDIA_CART2].mapperType = FMPAC_SRAM;

    MSXCartPtr[MEDIA_CART2][0] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][1] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][2] = (u8*)TURBO_FMPAC + 0x0000;      // 4000-5FFF -> MSX-MUSIC / FM-PAC BIOS
    MSXCartPtr[MEDIA_CART2][3] = (u8*)TURBO_FMPAC + 0x2000;      // 6000-7FFF -> MSX-MUSIC / FM-PAC BIOS
    MSXCartPtr[MEDIA_CART2][4] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][5] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][6] = (u8*)Unmapped_Memory;           // Segment Unmapped
    MSXCartPtr[MEDIA_CART2][7] = (u8*)Unmapped_Memory;           // Segment Unmapped

    msxLoadSRAM();  // FM-PAC with MSX-MUSIC also has 8K of SRAM
}

// ---------------------------------------------------------------
// Restore the MSX BIOS into the memory buffer and point to it...
// ---------------------------------------------------------------
void msx_restore_bios(void)
{
    // Slot 0 - BIOS is mapped in to start...
    MemoryMap[0] = TURBO_BIOS + 0x0000 - 0x0000;
    MemoryMap[1] = TURBO_BIOS + 0x2000 - 0x2000;
    MemoryMap[2] = TURBO_BIOS + 0x4000 - 0x4000;
    MemoryMap[3] = TURBO_BIOS + 0x6000 - 0x6000;

    MemoryMap[4] = Unmapped_Memory - 0x8000;
    MemoryMap[5] = Unmapped_Memory - 0xA000;
    MemoryMap[6] = Unmapped_Memory - 0xC000;
    MemoryMap[7] = Unmapped_Memory - 0xE000;
}


// ---------------------------------------------------------
// The MSX reset has some special memory mapping depending
// on if we have loaded a .DSK or a .ROM file.
// ---------------------------------------------------------
void msx_reset(void)
{
    last_f4 = 0x00;

    MSX_InitialMemoryLayout();
}

// -------------------------------------------------------------------
// For the few games that have SRAM built in. We back the SRAM onto
// a file with the same base filename but the extension is .SRM
// -------------------------------------------------------------------
extern char szName[];
void msxSaveSRAM(void)
{
    // Return to the original path
    chdir(GetMasterPath());

    // Make sure the 'sav' directory exists
    EnsureSaveDirectory();

    // Init filename = romname and SRM (SRAM) in place of ROM
    sprintf(szName,"sav/%s", GetMasterFilename());

    int len = strlen(szName);
    szName[len-3] = 's';
    szName[len-2] = 'r';
    szName[len-1] = 'm';
    szName[len-0] = 0;

    FILE *handle = fopen(szName, "wb+");
    if (handle != NULL)
    {
        // SRAM is up to 32K in size...
        fwrite(SRAM_Memory, sizeof(SRAM_Memory), 1, handle);
        fclose(handle);
    }
}

void msxLoadSRAM(void)
{
    // Return to the original path
    chdir(GetMasterPath());

    // Make sure the 'sav' directory exists
    EnsureSaveDirectory();

    // Init filename = romname and SRM (SRAM) in place of ROM
    sprintf(szName,"sav/%s", GetMasterFilename());

    int len = strlen(szName);
    szName[len-3] = 's';
    szName[len-2] = 'r';
    szName[len-1] = 'm';
    szName[len-0] = 0;

    // Check if the file exists... if so read it...
    if (ReadFileCarefully(szName, SRAM_Memory, sizeof(SRAM_Memory), 0, NULL) == 0)
    {
        memset(SRAM_Memory, 0xFF, sizeof(SRAM_Memory)); // Not found, so init the SRAM area
    }
}

// Ensure 'sav' directory exists. If not: create it.
void EnsureSaveDirectory(void)
{
    DIR* dir = opendir("sav");
    if (dir) closedir(dir);    // Directory exists... close it out and move on.
    else mkdir("sav", 0777);   // Otherwise create the directory...
}

// End of file
