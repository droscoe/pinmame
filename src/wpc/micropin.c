// license:BSD-3-Clause

/*
 * "Pentacup" by Micropin.
 * There were two different models:
 * One in 1978/79 and one in 1980/81.
 *
 *      1978  |  1980
 * CPU: 6800  |  8085
 * I/O: PIAs, |  CPU ports
 *      DMA   |  DMA
 * Snd: 566   |  ?
 */

#include "driver.h"
#include "core.h"
#include "machine/6821pia.h"
#include "cpu/i8085/i8085.h"
#include "cpu/m6800/m6800.h"
#include "sound/discrete.h"

static struct {
  int col;
  int store;
  int vol, vol_on;
  int decay;
  int restart_flg;
  mame_timer *sndTimer;
  mame_timer *resetTimer;

  double volume;
  UINT8  irq;
  UINT8  swData_irq;
  UINT8  swData[4];
  UINT8  out[16];
} locals;


/* 1978 version */

static void mp_adjust_volume(void) {
  if (locals.decay & 0x8) {
    double expdecay = 0;
    switch (locals.decay & 0x7) {                   // exp(-100 / (6.8 * 80*((decay) & 0x7 + 1)/8));
      case 0: expdecay = 0.23; break;
      case 1: expdecay = 0.47; break;
      case 2: expdecay = 0.61; break;
      case 3: expdecay = 0.69; break;
      case 4: expdecay = 0.75; break;
      case 5: expdecay = 0.78; break;
      case 6: expdecay = 0.81; break;
      case 7: expdecay = 0.83; break;
    }
    if (locals.vol_on) {
      if (locals.volume > locals.vol)
        locals.volume = ((locals.volume - locals.vol) * expdecay) + locals.vol;
      else
        locals.volume = locals.vol;
    } else {
      locals.volume *= expdecay;
    }
    timer_adjust(locals.sndTimer, TIME_IN_MSEC(100), 0, TIME_NEVER);
  } else {
    if (locals.vol_on)
      locals.volume = locals.vol;
    else
      locals.volume = 0;
  }
  mixer_set_volume(0, (int)locals.volume);
}

static WRITE_HANDLER(mp_pia0a_w) {
  if (locals.col >= 0) {
    coreGlobals.segments[15-locals.col].w = core_bcd2seg7a[data & 0x0f];
    coreGlobals.segments[31-locals.col].w = core_bcd2seg7a[data >> 4];
  }
}
static WRITE_HANDLER(mp_pia0b_w) {
  if (locals.col >= 0) {
    coreGlobals.segments[47-locals.col].w = core_bcd2seg7a[data & 0x0f];
    coreGlobals.segments[63-locals.col].w = core_bcd2seg7a[data >> 4];
  }
}
static WRITE_HANDLER(mp_pia0ca2_w) {
  if (!data && locals.col < 8) coreGlobals.lampMatrix[4] |= (1 << locals.col);
}

static READ_HANDLER(mp_pia1b_r) {
  logerror("PIA #1 B READ\n");
  return (coreGlobals.swMatrix[0] ^ 0x70);
}
static WRITE_HANDLER(mp_pia1a_w) {
  discrete_sh_reset(); // stops sound and flushes out the samples buffer
  discrete_sound_w(1 << (~data & 0x0f), 1);
  locals.vol = (int)((float)(((data ^ 0xf0) >> 4) + 1) / 16.0 * 100.0);
  mp_adjust_volume();
  logerror("PIA #1 A: %02x\n", data);
}
static WRITE_HANDLER(mp_pia1b_w) {
  locals.decay = (data & 0xf) ^ 0x7;
  mp_adjust_volume();
  logerror("PIA #1 B: %02x\n", data);
}
static WRITE_HANDLER(mp_pia1ca2_w) {
  locals.vol_on = data;
  mp_adjust_volume();
  logerror("PIA #1 CA2: %d\n", data);
}
static WRITE_HANDLER(mp_pia1cb2_w) {
// cpu_set_nmi_line(0, PULSE_LINE);
  locals.restart_flg = data;
  logerror("PIA #1 CB2: %d\n", data);
}
static void mp_pia1irq(int data) {
  logerror("PIA #1 IRQ\n");
}

static const struct pia6821_interface mp_pia[] = {{
  /*i: A/B,CA/B1,CA/B2 */ 0, 0, PIA_UNUSED_VAL(1), PIA_UNUSED_VAL(1), 0, 0,
  /*o: A/B,CA/B2       */ mp_pia0a_w, mp_pia0b_w, mp_pia0ca2_w, 0
},{
  /*i: A/B,CA/B1,CA/B2 */ 0, mp_pia1b_r, PIA_UNUSED_VAL(1), PIA_UNUSED_VAL(1), 0, 0,
  /*o: A/B,CA/B2       */ mp_pia1a_w, mp_pia1b_w, mp_pia1ca2_w, mp_pia1cb2_w,
  /*irq: A/B           */ mp_pia1irq, mp_pia1irq
}};

static WRITE_HANDLER(m400x_w) {
  switch (offset) {
  case 0:
    coreGlobals.solenoids = (coreGlobals.solenoids & 0xff00ff) | ((data ^ 0x08) << 8);
    break;
  case 5:
    coreGlobals.solenoids = (coreGlobals.solenoids & 0xffff00) | data;
    break;
  default:
    coreGlobals.lampMatrix[offset-1] = data;
  }
}

static READ_HANDLER(m400x_r) {
  UINT8 val = coreGlobals.swMatrix[offset + 1];
  if (!offset) val ^= 0x40; // invert tilt switch
  locals.col = -1;
  return val;
}

/* The chip at 510x is a PIA 6821 according to the schematics
 * but the data bus is inverted */
static WRITE_HANDLER(m510x_w) {
  pia_1_w(offset, data ^ 0xff);
}

static READ_HANDLER(m510x_r) {
  return (pia_1_r(offset) ^ 0xff);
}

static WRITE_HANDLER(m520x_w) {
  switch (offset) {
  case 0:
    coreGlobals.solenoids = (coreGlobals.solenoids & 0x00ffff) | (data << 16);
    break;
  case 2:
    locals.col = data & 0x0f;
    coreGlobals.lampMatrix[4] &= 0xff ^ (1 << data);
    break;
  case 3:
    locals.store = data & 1;
    break;
  default:
    logerror("m520%d_w: %02x\n", offset, data);
  }
}

static void snd_timer(int n) {
  mp_adjust_volume();
}

static void reset_timer(int n) {
  cpu_set_halt_line(0, CLEAR_LINE);
  locals.vol_on = 0;                  // change volume after reset because initial pia write to 5203 is not forwarded to CA2  
  mp_adjust_volume();                 // when pia port is reconfigured from input to output
}

/*-----------------------------------------------
/ Load/Save static ram
/-------------------------------------------------*/
static UINT8 *mp_CMOS;

static NVRAM_HANDLER(MICROPIN) {
  core_nvram(file, read_or_write, mp_CMOS, 0x200, 0x00);
}

static WRITE_HANDLER(mp_CMOS_w) {
  if (locals.store) {
    mp_CMOS[offset] = data;
  } else {
    logerror("no write to %04x: %03x\n", offset, data);
  }
}

static MEMORY_WRITE_START(mp_writemem)
  { 0x0000, 0x01ff, mp_CMOS_w, &mp_CMOS },
  { 0x4000, 0x4005, m400x_w },
  { 0x5000, 0x5003, pia_0_w },
  { 0x5100, 0x5103, m510x_w },
  { 0x5200, 0x5203, m520x_w },
MEMORY_END

static MEMORY_READ_START(mp_readmem)
  { 0x0000, 0x01ff, MRA_RAM },
  { 0x4000, 0x4004, m400x_r },
  { 0x5000, 0x5003, pia_0_r },
  { 0x5100, 0x5103, m510x_r },
  { 0x6000, 0x7fff, MRA_ROM },
  { 0xfc00, 0xffff, MRA_ROM },
MEMORY_END

static MACHINE_INIT(MICROPIN) {
  memset(&locals, 0, sizeof(locals));
  locals.sndTimer = timer_alloc(snd_timer);
  locals.resetTimer = timer_alloc(reset_timer);

  pia_config(0, PIA_STANDARD_ORDERING, &mp_pia[0]);
  pia_config(1, PIA_STANDARD_ORDERING, &mp_pia[1]);
  pia_reset();
}

static MACHINE_RESET(MICROPIN) {
  // Initialize pia1 outputs at reset to emulate pullup of undriven input signals to generate reset tone
  pia_write(1, 0, 0xff);    // set vol/snd ports to outputs
  pia_write(1, 2, 0x0f);    // set decay ports to outputs      
  pia_write(1, 1, 0x3c);    // set vol_on 
  pia_write(1, 3, 0x3c);    // set reset
  pia_write(1, 0, 0xff);    // set vol/snd ports
  pia_write(1, 2, 0x0f);    // set decay ports       
  mp_adjust_volume();
  discrete_sound_w(0, 1);

  // Hold CPU in reset for 1.1s as specified in schematic
  cpu_set_halt_line(0, ASSERT_LINE);
  timer_adjust(locals.resetTimer, TIME_IN_MSEC(1100), 0, TIME_NEVER);
}

static MACHINE_STOP(MICROPIN) {
  timer_remove(locals.sndTimer);
  timer_remove(locals.resetTimer);
}

static SWITCH_UPDATE(MICROPIN) {
  if (inports) {
    CORE_SETKEYSW(inports[CORE_COREINPORT]>>8, 0xff, 0);
    CORE_SETKEYSW(inports[CORE_COREINPORT],    0xd9, 1);
  }
}

static INTERRUPT_GEN(mp_vblank) {
  core_updateSw(core_getSol(12));
}
static INTERRUPT_GEN(mp_irq) {
  cpu_set_irq_line(0, M6800_IRQ_LINE, PULSE_LINE);
}
static void mp_nmi(int data) {
  // probably a watchdog timer (will pulse NMI which causes a reset)
}

static DISCRETE_SOUND_START(mp_discInt)
  DISCRETE_INPUT(NODE_01,0x0001,0xffff,0)
  DISCRETE_INPUT(NODE_02,0x0002,0xffff,0)
  DISCRETE_INPUT(NODE_03,0x0004,0xffff,0)
  DISCRETE_INPUT(NODE_04,0x0008,0xffff,0)
  DISCRETE_INPUT(NODE_05,0x0010,0xffff,0)
  DISCRETE_INPUT(NODE_06,0x0020,0xffff,0)
  DISCRETE_INPUT(NODE_07,0x0040,0xffff,0)
  DISCRETE_INPUT(NODE_08,0x0080,0xffff,0)
  DISCRETE_INPUT(NODE_09,0x0100,0xffff,0)
  DISCRETE_INPUT(NODE_10,0x0200,0xffff,0)
  DISCRETE_INPUT(NODE_11,0x0400,0xffff,0)
  DISCRETE_INPUT(NODE_12,0x0800,0xffff,0)
  DISCRETE_INPUT(NODE_13,0x1000,0xffff,0)
  DISCRETE_INPUT(NODE_14,0x2000,0xffff,0)
  DISCRETE_INPUT(NODE_15,0x4000,0xffff,0)
  DISCRETE_INPUT(NODE_16,0x8000,0xffff,0)

  DISCRETE_SQUAREWFIX(NODE_21,NODE_01,387, 50000,50,0,0) // G4 note
  DISCRETE_SQUAREWFIX(NODE_22,NODE_02,435, 50000,50,0,0) // A4 note
  DISCRETE_SQUAREWFIX(NODE_23,NODE_03,488, 50000,50,0,0) // B4 note
  DISCRETE_SQUAREWFIX(NODE_24,NODE_04,517, 50000,50,0,0) // C5 note
  DISCRETE_SQUAREWFIX(NODE_25,NODE_05,581, 50000,50,0,0) // D5 note
  DISCRETE_SQUAREWFIX(NODE_26,NODE_06,652, 50000,50,0,0) // E5 note
  DISCRETE_SQUAREWFIX(NODE_27,NODE_07,691, 50000,50,0,0) // F5 note
  DISCRETE_SQUAREWFIX(NODE_28,NODE_08,775, 50000,50,0,0) // G5 note
  DISCRETE_SQUAREWFIX(NODE_29,NODE_09,870, 50000,50,0,0) // A5 note
  DISCRETE_SQUAREWFIX(NODE_30,NODE_10,977, 50000,50,0,0) // B5 note
  DISCRETE_SQUAREWFIX(NODE_31,NODE_11,1035,50000,50,0,0) // C6 note
  DISCRETE_SQUAREWFIX(NODE_32,NODE_12,1161,50000,50,0,0) // D6 note
  DISCRETE_SQUAREWFIX(NODE_33,NODE_13,1304,50000,50,0,0) // E6 note
  DISCRETE_SQUAREWFIX(NODE_34,NODE_14,1381,50000,50,0,0) // F6 note
  DISCRETE_SQUAREWFIX(NODE_35,NODE_15,1550,50000,50,0,0) // G6 note
  DISCRETE_SQUAREWFIX(NODE_36,NODE_16,1740,50000,50,0,0) // A6 note

  DISCRETE_ADDER4(NODE_41,1,NODE_21,NODE_22,NODE_23,NODE_24)
  DISCRETE_ADDER4(NODE_42,1,NODE_25,NODE_26,NODE_27,NODE_28)
  DISCRETE_ADDER4(NODE_43,1,NODE_29,NODE_30,NODE_31,NODE_32)
  DISCRETE_ADDER4(NODE_44,1,NODE_33,NODE_34,NODE_35,NODE_36)
  DISCRETE_ADDER4(NODE_50,1,NODE_41,NODE_42,NODE_43,NODE_44)

  DISCRETE_OUTPUT(NODE_50, 70)
DISCRETE_SOUND_END

MACHINE_DRIVER_START(pentacup)
  MDRV_IMPORT_FROM(PinMAME)
  MDRV_CORE_INIT_RESET_STOP(MICROPIN,MICROPIN,MICROPIN)
  MDRV_CPU_ADD_TAG("mcpu", M6800, 1000000)
  MDRV_CPU_MEMORY(mp_readmem, mp_writemem)
  MDRV_CPU_PERIODIC_INT(mp_irq, 500) // noted as 11.5 us in schematics, but 500 Hz is more like the real thing. :)
  MDRV_CPU_VBLANK_INT(mp_vblank, 1)
  MDRV_TIMER_ADD(mp_nmi, 2)
  MDRV_SWITCH_UPDATE(MICROPIN)
  MDRV_NVRAM_HANDLER(MICROPIN)
  MDRV_SOUND_ADD(DISCRETE, mp_discInt)
MACHINE_DRIVER_END

INPUT_PORTS_START(pentacup)
  CORE_PORTS
  SIM_PORTS(1)
  PORT_START /* 0 */
    COREPORT_BITDEF(0x0010, IPT_START1,    IP_KEY_DEFAULT)
    COREPORT_BIT   (0x0001, "Tilt #1",     KEYCODE_COMMA)
    COREPORT_BIT   (0x0008, "Tilt #2",     KEYCODE_STOP)
    COREPORT_BIT   (0x0040, "Tilt #3",     KEYCODE_SLASH)
    COREPORT_BIT   (0x0080, "Tilt #4",     KEYCODE_DEL)
    COREPORT_BITDEF(0x1000, IPT_COIN1,     IP_KEY_DEFAULT)
    COREPORT_BIT   (0x2000, "Volume up",   KEYCODE_7)
    COREPORT_BIT   (0x4000, "Volume down", KEYCODE_8)
    COREPORT_BIT   (0x8000, "Alarm/Tilt",  KEYCODE_9)
INPUT_PORTS_END

core_tLCDLayout mp_disp[] = {
  {0, 0,33, 2, CORE_SEG7}, {0, 6,35, 1, CORE_SEG7}, {0,10,17, 3, CORE_SEG7}, {0,16, 1, 3, CORE_SEG7}, {0,24, 4, 6, CORE_SEG7}, {0,38,10, 6, CORE_SEG7},
  {3, 8,57, 7, CORE_SEG7}, {3,24,20, 6, CORE_SEG7}, {3,38,26, 6, CORE_SEG7},
  {6, 8,50, 7, CORE_SEG7}, {6,24,36, 6, CORE_SEG7}, {6,38,42, 6, CORE_SEG7}, {0}
};
static core_tGameData pentacupGameData = {0,mp_disp,{FLIP_SWNO(6,3),0,-3}};
static void init_pentacup(void) {
  core_gameData = &pentacupGameData;
}

ROM_START(pentacup)
  NORMALREGION(0x10000, REGION_CPU1)
  ROM_LOAD("ic2.bin", 0x6400, 0x0400, CRC(fa468a0f) SHA1(e9c8028bcd5b87d24f4588516536767a869c38ff))
  ROM_LOAD("ic3.bin", 0x6800, 0x0400, CRC(7bfdaec8) SHA1(f2037c0e2d4acf0477351ecafc9f0826e9d64d76))
  ROM_LOAD("ic4.bin", 0x6c00, 0x0400, CRC(5e0fcb1f) SHA1(e529539c6eb1e174a799ad6abfce9e31870ff8af))
  ROM_LOAD("ic5.bin", 0x7000, 0x0400, CRC(a26c6e0b) SHA1(21c4c306fbc2da52887e309b1c83a1ea69501c1f))
  ROM_LOAD("ic6.bin", 0x7400, 0x0400, CRC(4715ac34) SHA1(b6d8c20c487db8d7275e36f5793666cc591a6691))
  ROM_LOAD("ic7.bin", 0x7800, 0x0400, CRC(c58d13c0) SHA1(014958bc69ff326392a5a7782703af0980e6e170))
  ROM_LOAD("ic8.bin", 0x7c00, 0x0400, CRC(9f67bc65) SHA1(504008d4c7c23a14fdf247c9e6fc00e95d907d7b))
  ROM_RELOAD(0xfc00, 0x0400)
ROM_END
CORE_GAMEDEFNV(pentacup,"Pentacup (rev. 1)",1978,"Micropin",pentacup,0)


/* 1980 Version */

static INTERRUPT_GEN(mp2_vblank) {
  int i;
  for (i = 0; i < 32; i++) {
    UINT8 mem = memory_region(REGION_CPU1)[0x23c0 + i];
    coreGlobals.segments[62 - i * 2].w = core_bcd2seg7[mem >> 4];
    coreGlobals.segments[63 - i * 2].w = core_bcd2seg7[mem & 0x0f];
  }
  coreGlobals.lampMatrix[16] = (memory_region(REGION_CPU1)[0x23d6] & 0xf0) | (memory_region(REGION_CPU1)[0x23de] >> 4);
  coreGlobals.lampMatrix[17] = (memory_region(REGION_CPU1)[0x23cc] & 0xf0) | (memory_region(REGION_CPU1)[0x23de] & 0x0f);

  core_updateSw(TRUE);
}

static INTERRUPT_GEN(mp2_irq) {
  cpu_set_irq_line(0, I8085_RST55_LINE, locals.irq ? ASSERT_LINE : CLEAR_LINE);
  cpu_set_irq_line(0, I8085_RST65_LINE, !locals.irq ? ASSERT_LINE : CLEAR_LINE);
  locals.irq = !locals.irq;

  if (~locals.swData_irq & coreGlobals.swMatrix[3] & 0x01) { // 25 pts, inverted
    memory_region(REGION_CPU1)[0x2193] &= 0xfe;
  }
  locals.swData_irq = coreGlobals.swMatrix[3];
  if (coreGlobals.swMatrix[4] & 0x20) { // end of ball
    memory_region(REGION_CPU1)[0x21a0] = 1;
  }
  if (coreGlobals.swMatrix[6] & 0x08) { // bonus collect & end of ball
    memory_region(REGION_CPU1)[0x21ae] = 1;
  }
}

static SWITCH_UPDATE(MICROPIN2) {
  int i, sw, offset;

  if (inports) {
    CORE_SETKEYSW(inports[CORE_COREINPORT], 0x01, 0);
    CORE_SETKEYSW(inports[CORE_COREINPORT], 0xf0, 9);
  }
  cpu_set_nmi_line(0, (coreGlobals.swMatrix[0] & 1) ? ASSERT_LINE : CLEAR_LINE);

  for (sw = 0; sw < 4; sw++) {
    for (i = 0; i < 8; i++) {
      offset = 0x2193 + sw * 8 + i;
      if (offset < 0x2194 || (offset > 0x219f && offset < 0x21a4)) continue;
      if (~locals.swData[sw] & coreGlobals.swMatrix[3 + sw] & (1 << i)) {
        memory_region(REGION_CPU1)[offset] |= 1;
      } else {
        memory_region(REGION_CPU1)[offset] &= 0xfe;
      }
    }
    locals.swData[sw] = coreGlobals.swMatrix[3 + sw];
  }
}

static WRITE_HANDLER(mxxxx_w) {
  logerror("%04x: Write to %04x: %02x\n", activecpu_get_pc(), offset, data);
}

static MEMORY_WRITE_START(mp2_writemem)
  { 0x0000, 0x1fff, mxxxx_w },
  { 0x2000, 0x23ff, MWA_RAM, &generic_nvram, &generic_nvram_size },
MEMORY_END

static MEMORY_READ_START(mp2_readmem)
  { 0x0000, 0x1fff, MRA_ROM },
  { 0x2000, 0x23ff, MRA_RAM },
MEMORY_END

static WRITE_HANDLER(mp2_out) {
  coreGlobals.lampMatrix[offset] = ~data;
  if (locals.out[offset] != data) logerror("out %x: %02x\n", offset, data);
  locals.out[offset] = data;
#ifdef MAME_DEBUG
  if (offset == 0x0d) {
    coreGlobals.segments[64].w = core_bcd2seg7[data ^ 0xff];
  }
  if (offset == 0x0e) {
    coreGlobals.segments[65].w = core_bcd2seg7[data ^ 0xff];
  }
  if (offset == 0x0f) {
    coreGlobals.segments[66].w = core_bcd2seg7[data >> 4];
    coreGlobals.segments[67].w = core_bcd2seg7[data & 0x0f];
  }
#endif
}

static READ_HANDLER(mp2_sw) {
  if (coreGlobals.swMatrix[1 + offset]) {
    if (coreGlobals.swMatrix[1 + offset] == 1)
      return 0;
    return coreGlobals.swMatrix[1 + offset];
  }
  return 1;
}

static READ_HANDLER(mp2_dip) {
  switch(offset + 2) {
    case 2: // 1..7 cents, unknown switch
      return ~(core_getDip(4) | (coreGlobals.swMatrix[9] & 1));
    case 3: // 10..70 cents, unknown diagnostic switch
      return ~(core_getDip(2) | (coreGlobals.swMatrix[10] & 1));
    case 4: // 8+9 cents, 80+90 cents, coin, show previous, NVRAM protect, unknown switch
      return ~((core_getDip(5) & 0x03) | (core_getDip(3) & 0x0c) | (coreGlobals.swMatrix[9] & 0xf0));
    default: // dips, 1+2 set balls per game
      return ~core_getDip(0);
  }
}

static PORT_WRITE_START(mp2_writeport)
  { 0x00, 0x0f, mp2_out },
PORT_END

static PORT_READ_START(mp2_readport)
  { 0x00, 0x01, mp2_sw },
  { 0x02, 0x05, mp2_dip },
PORT_END

MACHINE_DRIVER_START(pentacp2)
  MDRV_IMPORT_FROM(PinMAME)
  MDRV_CPU_ADD_TAG("mcpu", 8085A, 2500000)
  MDRV_CPU_MEMORY(mp2_readmem, mp2_writemem)
  MDRV_CPU_PORTS(mp2_readport, mp2_writeport)
  MDRV_CPU_PERIODIC_INT(mp2_irq, 120)
  MDRV_CPU_VBLANK_INT(mp2_vblank, 1)
  MDRV_SWITCH_UPDATE(MICROPIN2)
  MDRV_DIPS(48)
  MDRV_NVRAM_HANDLER(generic_1fill)
  MDRV_DIAGNOSTIC_LEDH(1)
MACHINE_DRIVER_END

#define COREPORT_BITINV(mask, name, key) \
  PORT_BITX(mask,IP_ACTIVE_LOW,IPT_BUTTON1,name,key,IP_JOY_NONE)

INPUT_PORTS_START(pentacp2)
  CORE_PORTS
  SIM_PORTS(1)
  PORT_START /* 0 */
    COREPORT_BIT   (0x0001, "Reset",         KEYCODE_HOME)
    COREPORT_BIT   (0x0010, "Coin",          KEYCODE_5)
    COREPORT_BITINV(0x0020, "Keep NVRAM",    KEYCODE_0)
    COREPORT_BITINV(0x0040, "Show previous", KEYCODE_9)
    COREPORT_BIT   (0x0080, "Tilt",          KEYCODE_DEL)
  PORT_START /* 1 */
    COREPORT_DIPNAME( 0x0001, 0x0000, "S1")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0001, "1" )
    COREPORT_DIPNAME( 0x0002, 0x0000, "S2")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0002, "1" )
    COREPORT_DIPNAME( 0x0004, 0x0000, "S3")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0004, "1" )
    COREPORT_DIPNAME( 0x0008, 0x0000, "S4")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0008, "1" )
    COREPORT_DIPNAME( 0x0010, 0x0000, "S5")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0010, "1" )
    COREPORT_DIPNAME( 0x0020, 0x0000, "S6")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0020, "1" )
    COREPORT_DIPNAME( 0x0040, 0x0000, "S7")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0040, "1" )
    COREPORT_DIPNAME( 0x0080, 0x0000, "S8")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0080, "1" )
  PORT_START /* 2 */
    COREPORT_DIPNAME( 0xffff, 0x0004, "10 cents x")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0002, "1" )
      COREPORT_DIPSET(0x0004, "2" )
      COREPORT_DIPSET(0x0008, "3" )
      COREPORT_DIPSET(0x0010, "4" )
      COREPORT_DIPSET(0x0020, "5" )
      COREPORT_DIPSET(0x0040, "6" )
      COREPORT_DIPSET(0x0080, "7" )
      COREPORT_DIPSET(0x0400, "8" )
      COREPORT_DIPSET(0x0800, "9" )
  PORT_START /* 3 */
    COREPORT_DIPNAME( 0xffff, 0x0020, " 1 cent  x")
      COREPORT_DIPSET(0x0000, "0" )
      COREPORT_DIPSET(0x0002, "1" )
      COREPORT_DIPSET(0x0004, "2" )
      COREPORT_DIPSET(0x0008, "3" )
      COREPORT_DIPSET(0x0010, "4" )
      COREPORT_DIPSET(0x0020, "5" )
      COREPORT_DIPSET(0x0040, "6" )
      COREPORT_DIPSET(0x0080, "7" )
      COREPORT_DIPSET(0x0100, "8" )
      COREPORT_DIPSET(0x0200, "9" )
INPUT_PORTS_END

core_tLCDLayout mp2_disp[] = {
  { 0, 6,58,3,CORE_SEG7}, { 0,13,61,3,CORE_SEG7}, { 0,27,12,3,CORE_SEG7}, { 0,34,15,3,CORE_SEG7},
  { 3, 0, 0,2,CORE_SEG7}, { 3, 7,10,2,CORE_SEG7}, { 3,14, 9,1,CORE_SEG7},
  { 3,19,46,3,CORE_SEG7}, { 3,26,49,3,CORE_SEG7}, { 3,35,32,3,CORE_SEG7}, { 3,42,35,3,CORE_SEG7},
  { 6,19,52,3,CORE_SEG7}, { 6,26,55,3,CORE_SEG7}, { 6,35,26,3,CORE_SEG7}, { 6,42,29,3,CORE_SEG7},
  { 9,16, 4,1,CORE_SEG7}, { 9,19, 5,3,CORE_SEG7}, { 9,35,20,3,CORE_SEG7}, { 9,42,23,3,CORE_SEG7},
#ifdef MAME_DEBUG
  {12,14,39,7,CORE_SEG7}, {12,28,19,1,CORE_SEG7}, {12,30, 8,1,CORE_SEG7},
  {12, 0,64,1,CORE_SEG7}, {12, 4,65,1,CORE_SEG7}, {12, 8,66,2,CORE_SEG7},
#endif
  {0}
};
static core_tGameData pentacup2GameData = {0,mp2_disp,{FLIP_SWNO(73,65),0,10}};
static void init_pentacp2(void) {
  core_gameData = &pentacup2GameData;
}

ROM_START(pentacp2)
  NORMALREGION(0x10000, REGION_CPU1)
  ROM_LOAD("micro_1.bin", 0x0000, 0x0800, CRC(62d04111) SHA1(f0ce705c06a43a81293d8610394ce7c4143148e9))
  ROM_LOAD("micro_2.bin", 0x0800, 0x0800, CRC(832e4223) SHA1(1409b0c7de35012b9d0eba9bb73b52aecc93c0f2))
  ROM_LOAD("micro_3.bin", 0x1000, 0x0800, CRC(9d5d04d1) SHA1(1af32c418b73ee457f06ee9a8362cfec75e61f30))
  ROM_LOAD("micro_4.bin", 0x1800, 0x0800, CRC(358ffd6a) SHA1(f5299e39d991bf882f827a62a1d9bb18e46dbcfc))
ROM_END
CORE_GAMEDEFNV(pentacp2,"Pentacup (rev. 2)",1980,"Micropin",pentacp2,GAME_NOT_WORKING)
