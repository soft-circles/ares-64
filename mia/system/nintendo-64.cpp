struct Nintendo64 : System {
  auto name() -> string override { return "Nintendo 64"; }
  auto load(string location) -> LoadResult override;
  auto save(string location) -> bool override;
};

auto Nintendo64::load(string location) -> LoadResult {
  this->location = locate();
  pak = std::make_shared<vfs::directory>();

  for(string name : {"pif.ntsc.rom", "pif.pal.rom", "pif.sm5.rom"}) {
    auto rom = file::read(mia::locate({"Firmware/Nintendo 64/", name}));
    if(!rom.empty()) pak->append(name, rom);
  }
  return successful;
}

auto Nintendo64::save(string location) -> bool {
  return true;
}
