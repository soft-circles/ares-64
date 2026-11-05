//IPL1 and IPL2 run from the PIF ROM. Without it, set up the state IPL2 leaves
//for IPL3: the CIC seeds read and the ROM locked out, the first 4KB of the
//cartridge (or the 64DD IPL ROM) in DMEM, PI domain 1 timings from its
//header, and the IPL3 entry registers.
auto PIF::bootHLE() -> void {
  initHLE();
  if(state == Error) return;

  ramWriteCommand(ramReadCommand() | 0x10);
  io.romLockout = 1;
  joyInit();
  state = WaitGetChecksum;

  n32 osinfo = ram.read<Word>(0x24);
  cpu.ipu.r[CPU::IPU::S3].u64 = osinfo.bit(19);       //ROM type: 0 = cartridge, 1 = 64DD
  cpu.ipu.r[CPU::IPU::S4].u64 = Region::PAL() ? 0 : 1; //TV type: 0 = PAL, 1 = NTSC
  cpu.ipu.r[CPU::IPU::S5].u64 = osinfo.bit(17);       //reset type: 0 = cold, 1 = NMI
  cpu.ipu.r[CPU::IPU::S6].u64 = osinfo.bit(8,15);     //CIC IPL3 seed
  cpu.ipu.r[CPU::IPU::S7].u64 = osinfo.bit(18);       //PIF version
  cpu.ipu.r[CPU::IPU::T3].u64 = 0xffff'ffff'a400'0040ull;
  cpu.ipu.r[CPU::IPU::SP].u64 = 0xffff'ffff'a400'1ff0ull;
  cpu.ipu.r[CPU::IPU::RA].u64 = 0xffff'ffff'a400'1550ull;

  cpu.scc.status.errorLevel = 0;
  cpu.scc.status.interruptMask = 0;
  cpu.scc.status.softReset = 0;
  cpu.scc.status.vectorLocation = 0;
  cpu.context.setMode();

  bool fromDD = osinfo.bit(19) && _DD();
  for(u32 address = 0; address < 0x1000; address += 4) {
    u32 data = 0;
    if(fromDD && address < dd.iplrom.size) data = dd.iplrom.read<Word>(address);
    if(!fromDD && address < cartridge.rom.size) data = cartridge.rom.read<Word>(address);
    rsp.dmem.write<Word>(address, data);
  }

  n32 config = rsp.dmem.read<Word>(0x00);
  pi.bsd1.latency = config.bit(0,7);
  pi.bsd1.pulseWidth = config.bit(8,15);
  pi.bsd1.pageSize = config.bit(16,19);
  pi.bsd1.releaseDuration = config.bit(20,21);

  //CIC-NUS-6105 IPL3s run this loop, which IPL2 leaves in IMEM
  static constexpr u32 imem6105[] = {
    0x3c0dbfc0, 0x8da807fc, 0x25ad07c0, 0x31080080,
    0x5500fffc, 0x3c0dbfc0, 0x8da80024, 0x3c0bb000,
  };
  for(u32 n : range(8)) rsp.imem.write<Word>(n * 4, imem6105[n]);

  cpu.pipeline.setPc(0xffff'ffff'a400'0040ull);
}
