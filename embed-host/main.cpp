//ares-embed (SPIKE): runs the N64 core headless for the Odin64 editor's viewport.
//
//  ares-embed <rom.z64> <frames-file>
//
//It reuses ares-test's EmulatorRunner (angrylion software RDP, no window, no GPU) and paces the core to the
//system's refresh rate on this thread. Every presented frame goes into <frames-file>, which the editor maps:
//
//  u32 magic 'O64F', u32 sequence (odd while a frame is being written), u32 width, u32 height,
//  u32 frames emulated per second x100, 44 bytes reserved, then width*height RGBA8 pixels (max 640x480).
//
//Commands arrive on stdin, one per line:
//  scene <path>   writes the .o64s at path into the ROM's preview mailbox (found by its signature in RDRAM)
//  pause | resume | quit
//The ROM's ISViewer log goes to stdout; diagnostics go to stderr.

#include "../test-runner/runner.hpp"

#include <nall/main.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

using namespace nall;

namespace {

constexpr u32 FrameMagic = 0x4f363446;  //"O64F"
constexpr u32 HeaderSize = 64;
constexpr u32 MaxWidth = 640, MaxHeight = 480;
constexpr u32 FileSize = HeaderSize + MaxWidth * MaxHeight * 4;

//The ROM's Preview_Mailbox starts with these 16 bytes, so no ELF is needed to find it.
const char MailboxSignature[16] = {'O','D','I','N','6','4','-','P','R','E','V','I','E','W','!','!'};
constexpr u32 MailboxHeader = 32;   //signature, sequence, length, 8 reserved bytes
constexpr u32 MailboxSize = 16 * 1024;

std::mutex commandMutex;
std::deque<std::string> commands;

auto readCommands() -> void {
  std::string line;
  char c;
  while(::read(0, &c, 1) == 1) {
    if(c != '\n') { line += c; continue; }
    std::lock_guard<std::mutex> lock(commandMutex);
    commands.push_back(line);
    line.clear();
  }
  std::lock_guard<std::mutex> lock(commandMutex);
  commands.push_back("quit");  //the editor closed our stdin or exited
}

//KSEG0 address of the mailbox, or 0. Scans RDRAM on 16-byte boundaries (the struct is #align(16)).
auto findMailbox() -> u32 {
  auto& ram = ares::Nintendo64::rdram.ram;
  for(u32 address = 0; address + 16 <= ram.size; address += 16) {
    bool match = true;
    for(u32 i = 0; i < 16 && match; i += 4) {
      u32 word = ram.read<ares::Nintendo64::Word>(address + i, ares::Nintendo64::RBusDevice::UNKNOWN);
      u32 want = (u8)MailboxSignature[i] << 24 | (u8)MailboxSignature[i+1] << 16 | (u8)MailboxSignature[i+2] << 8 | (u8)MailboxSignature[i+3];
      match = word == want;
    }
    if(match) return 0x8000'0000 | address;
  }
  return 0;
}

//Writes like the CPU would through KSEG0, so a line the ROM has cached sees the bytes (the same path as ares-64's
//GDB server uses for its M packets).
auto writeGuest(u32 address, const u8* data, u32 size) -> void {
  for(u32 i = 0; i < size; i++) {
    u64 vaddr = (u64)(s32)(address + i);
    ares::Nintendo64::cpu.dcache.writeDebug<ares::Nintendo64::Byte>(vaddr, (address + i) & 0x1fff'ffff, data[i]);
  }
}

auto writeWord(u32 address, u32 value) -> void {
  u8 bytes[4] = {(u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value};
  writeGuest(address, bytes, 4);
}

u32 mailbox = 0;
u32 mailboxSequence = 0;

auto pushScene(const string& path) -> void {
  auto data = file::read(path);
  if(data.empty()) { print(stderr, "[ares-embed] cannot read ", path, "\n"); return; }
  if(data.size() > MailboxSize) { print(stderr, "[ares-embed] ", path, " is ", data.size(), " bytes; the mailbox holds ", MailboxSize, "\n"); return; }
  if(!mailbox) mailbox = findMailbox();
  if(!mailbox) { print(stderr, "[ares-embed] no preview mailbox in RDRAM; is this a preview ROM?\n"); return; }
  auto start = std::chrono::steady_clock::now();
  writeGuest(mailbox + MailboxHeader, data.data(), data.size());
  writeWord(mailbox + 20, data.size());
  writeWord(mailbox + 16, ++mailboxSequence);
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  print(stderr, "[ares-embed] pushed ", data.size(), " bytes as ", mailboxSequence, " to 0x", hex(mailbox, 8), " in ", us, "us\n");
}

}  //namespace

auto nall::main(Arguments arguments) -> void {
  ares::Memory::FixedAllocator::get();
  string romPath = arguments.take();
  string framesPath = arguments.take();
  if(!romPath || !framesPath) { print(stderr, "usage: ares-embed <rom.z64> <frames-file>\n"); ::exit(2); }

  string workDir = {Path::temporary(), "ares-embed/"};
  directory::create(workDir);
  mia::setHomeLocation([workDir]() -> string { return workDir; });
  mia::setSaveLocation([workDir]() -> string { return {workDir, "Saves/"}; });
  ares::platform = &emulatorRunner;

  int fd = ::open(framesPath.data(), O_RDWR | O_CREAT, 0644);
  if(fd < 0 || ::ftruncate(fd, FileSize) != 0) { print(stderr, "[ares-embed] cannot open ", framesPath, "\n"); ::exit(2); }
  auto shared = (u8*)::mmap(nullptr, FileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(shared == MAP_FAILED) { print(stderr, "[ares-embed] cannot map ", framesPath, "\n"); ::exit(2); }
  auto header = (std::atomic<u32>*)shared;
  header[0] = FrameMagic;

  emulatorRunner.homebrewMode = true;
  if(auto error = emulatorRunner.loadRom(romPath)) { print(stderr, "[ares-embed] ", error, "\n"); ::exit(1); }
  std::thread(readCommands).detach();

  using clock = std::chrono::steady_clock;
  auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / emulatorRunner.refreshRate()));
  auto next = clock::now();
  auto windowStart = clock::now();
  u32 windowFrames = 0, sequence = 0;
  bool paused = false;
  EmulatorRunner::ScreenshotResult frame;

  while(true) {
    std::deque<std::string> pending;
    { std::lock_guard<std::mutex> lock(commandMutex); pending.swap(commands); }
    for(auto& command : pending) {
      if(command == "quit") { emulatorRunner.closeRom(); ::exit(0); }
      else if(command == "pause") paused = true;
      else if(command == "resume") { paused = false; next = clock::now(); }
      else if(command.rfind("scene ", 0) == 0) pushScene(command.substr(6).c_str());
      else print(stderr, "[ares-embed] unknown command: ", command.c_str(), "\n");
    }
    if(paused) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

    emulatorRunner.runFrames(1);
    if(emulatorRunner.screenshot(frame, false) == "" && frame.width <= MaxWidth && frame.height <= MaxHeight) {
      header[1].store(++sequence);  //odd: writing
      header[2] = frame.width;
      header[3] = frame.height;
      memcpy(shared + HeaderSize, frame.rgba.data(), frame.rgba.size());
      header[1].store(++sequence);  //even: complete
    }

    windowFrames++;
    auto now = clock::now();
    if(now - windowStart >= std::chrono::seconds(1)) {
      double fps = windowFrames / std::chrono::duration<double>(now - windowStart).count();
      header[4] = (u32)(fps * 100);
      windowStart = now;
      windowFrames = 0;
    }
    next += period;
    if(next > now) std::this_thread::sleep_until(next);
    else next = now;  //behind: run flat out rather than bursting to catch up
  }
}
