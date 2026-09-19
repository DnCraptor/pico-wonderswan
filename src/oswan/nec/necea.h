
static UINT32 EA;
static UINT16 EO;
static UINT16 E16;

/* EA_xxx run for every memory-operand instruction; keep them in SRAM too
   (the opcode handlers already are, but pico2 left these in XIP flash). */
#define EAFN(name) static unsigned __not_in_flash_func(name)(void)

EAFN(EA_000) { EO=I.regs.w[BW]+I.regs.w[IX]; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_001) { EO=I.regs.w[BW]+I.regs.w[IY]; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_002) { EO=I.regs.w[BP]+I.regs.w[IX]; EA=DefaultBase(SS)+EO; return EA; }
EAFN(EA_003) { EO=I.regs.w[BP]+I.regs.w[IY]; EA=DefaultBase(SS)+EO; return EA; }
EAFN(EA_004) { EO=I.regs.w[IX]; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_005) { EO=I.regs.w[IY]; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_006) { EO=FETCH; EO+=FETCH<<8; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_007) { EO=I.regs.w[BW]; EA=DefaultBase(DS)+EO; return EA; }

EAFN(EA_100) { EO=(I.regs.w[BW]+I.regs.w[IX]+(INT8)FETCH); EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_101) { EO=(I.regs.w[BW]+I.regs.w[IY]+(INT8)FETCH); EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_102) { EO=(I.regs.w[BP]+I.regs.w[IX]+(INT8)FETCH); EA=DefaultBase(SS)+EO; return EA; }
EAFN(EA_103) { EO=(I.regs.w[BP]+I.regs.w[IY]+(INT8)FETCH); EA=DefaultBase(SS)+EO; return EA; }
EAFN(EA_104) { EO=(I.regs.w[IX]+(INT8)FETCH); EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_105) { EO=(I.regs.w[IY]+(INT8)FETCH); EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_106) { EO=(I.regs.w[BP]+(INT8)FETCH); EA=DefaultBase(SS)+EO; return EA; }
EAFN(EA_107) { EO=(I.regs.w[BW]+(INT8)FETCH); EA=DefaultBase(DS)+EO; return EA; }

EAFN(EA_200) { E16=FETCH; E16+=FETCH<<8; EO=I.regs.w[BW]+I.regs.w[IX]+(INT16)E16; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_201) { E16=FETCH; E16+=FETCH<<8; EO=I.regs.w[BW]+I.regs.w[IY]+(INT16)E16; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_202) { E16=FETCH; E16+=FETCH<<8; EO=I.regs.w[BP]+I.regs.w[IX]+(INT16)E16; EA=DefaultBase(SS)+EO; return EA; }
EAFN(EA_203) { E16=FETCH; E16+=FETCH<<8; EO=I.regs.w[BP]+I.regs.w[IY]+(INT16)E16; EA=DefaultBase(SS)+EO; return EA; }
EAFN(EA_204) { E16=FETCH; E16+=FETCH<<8; EO=I.regs.w[IX]+(INT16)E16; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_205) { E16=FETCH; E16+=FETCH<<8; EO=I.regs.w[IY]+(INT16)E16; EA=DefaultBase(DS)+EO; return EA; }
EAFN(EA_206) { E16=FETCH; E16+=FETCH<<8; EO=I.regs.w[BP]+(INT16)E16; EA=DefaultBase(SS)+EO; return EA; }
EAFN(EA_207) { E16=FETCH; E16+=FETCH<<8; EO=I.regs.w[BW]+(INT16)E16; EA=DefaultBase(DS)+EO; return EA; }

static unsigned (*GetEA[192])(void)={
	EA_000, EA_001, EA_002, EA_003, EA_004, EA_005, EA_006, EA_007,
	EA_000, EA_001, EA_002, EA_003, EA_004, EA_005, EA_006, EA_007,
	EA_000, EA_001, EA_002, EA_003, EA_004, EA_005, EA_006, EA_007,
	EA_000, EA_001, EA_002, EA_003, EA_004, EA_005, EA_006, EA_007,
	EA_000, EA_001, EA_002, EA_003, EA_004, EA_005, EA_006, EA_007,
	EA_000, EA_001, EA_002, EA_003, EA_004, EA_005, EA_006, EA_007,
	EA_000, EA_001, EA_002, EA_003, EA_004, EA_005, EA_006, EA_007,
	EA_000, EA_001, EA_002, EA_003, EA_004, EA_005, EA_006, EA_007,

	EA_100, EA_101, EA_102, EA_103, EA_104, EA_105, EA_106, EA_107,
	EA_100, EA_101, EA_102, EA_103, EA_104, EA_105, EA_106, EA_107,
	EA_100, EA_101, EA_102, EA_103, EA_104, EA_105, EA_106, EA_107,
	EA_100, EA_101, EA_102, EA_103, EA_104, EA_105, EA_106, EA_107,
	EA_100, EA_101, EA_102, EA_103, EA_104, EA_105, EA_106, EA_107,
	EA_100, EA_101, EA_102, EA_103, EA_104, EA_105, EA_106, EA_107,
	EA_100, EA_101, EA_102, EA_103, EA_104, EA_105, EA_106, EA_107,
	EA_100, EA_101, EA_102, EA_103, EA_104, EA_105, EA_106, EA_107,

	EA_200, EA_201, EA_202, EA_203, EA_204, EA_205, EA_206, EA_207,
	EA_200, EA_201, EA_202, EA_203, EA_204, EA_205, EA_206, EA_207,
	EA_200, EA_201, EA_202, EA_203, EA_204, EA_205, EA_206, EA_207,
	EA_200, EA_201, EA_202, EA_203, EA_204, EA_205, EA_206, EA_207,
	EA_200, EA_201, EA_202, EA_203, EA_204, EA_205, EA_206, EA_207,
	EA_200, EA_201, EA_202, EA_203, EA_204, EA_205, EA_206, EA_207,
	EA_200, EA_201, EA_202, EA_203, EA_204, EA_205, EA_206, EA_207,
	EA_200, EA_201, EA_202, EA_203, EA_204, EA_205, EA_206, EA_207
};
