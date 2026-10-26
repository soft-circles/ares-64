//ares-embed: runs the N64 core headless behind the Odin64 editor's N64 tab.
//
//  ares-embed [--parallel] <rom.z64> <frames-file>
//
//It reuses ares-test's EmulatorRunner (angrylion software RDP, no window, no GPU) and paces the core to the
//system's refresh rate on this thread. angrylion renders on this thread too unless --parallel is given. Every
//presented frame goes into <frames-file>, which the editor maps:
//
//  u32 magic 'O64F', u32 sequence (odd while a frame is being written), u32 width, u32 height,
//  u32 frames emulated per second x100, 44 bytes reserved, then width*height RGBA8 pixels (max 640x480).
//
//Commands arrive on stdin, one per line; EOF quits:
//  scene <path>   writes the file at path into the ROM's preview mailbox
//  pause | resume | quit
//The ROM's ISViewer log goes to stdout; diagnostics go to stderr.
//
//The preview mailbox is a 16-byte-aligned block in the ROM's RDRAM, found by its signature, so no ELF is needed:
//
//  16-byte signature "ODIN64-PREVIEW!!", u32 sequence, u32 length, u32 capacity, u32 reserved, then the payload
//
//The ROM sets the capacity; ares-embed writes the payload, then its length, then a new sequence, all through the
//CPU data cache. The payload is opaque here: what it holds is between the editor and the ROM.

#include "../test-runner/runner.hpp"

#include <nall/main.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
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

const char MailboxSignature[16] = {'O','D','I','N','6','4','-','P','R','E','V','I','E','W','!','!'};
constexpr u32 MailboxSequence = 16;
constexpr u32 MailboxLength = 20;
constexpr u32 MailboxCapacity = 24;
constexpr u32 MailboxHeader = 32;

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
  commands.push_back("quit");
}

//physical address of the mailbox, or 0. Scans on 16-byte boundaries.
auto findMailbox() -> u32 {
  u32 size = ares::Nintendo64::rdram.ram.size;
  std::vector<u8> block;
  for(u32 address = 0; address + 16 <= size; address += 16) {
    if(emulatorRunner.readRdram(address, 16, block)) return 0;
    if(memcmp(block.data(), MailboxSignature, 16) == 0) return address;
  }
  return 0;
}

auto readWord(u32 address) -> u32 {
  std::vector<u8> bytes;
  if(emulatorRunner.readRdram(address, 4, bytes)) return 0;
  return bytes[0] << 24 | bytes[1] << 16 | bytes[2] << 8 | bytes[3];
}

auto writeWord(u32 address, u32 value) -> void {
  u8 bytes[4] = {(u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value};
  emulatorRunner.writeRdram(address, bytes, 4);
}

u32 mailbox = 0;
u32 mailboxSequence = 0;

auto pushScene(const string& path) -> void {
  auto data = file::read(path);
  if(data.empty()) { print(stderr, "[ares-embed] cannot read ", path, "\n"); return; }
  if(!mailbox) mailbox = findMailbox();
  if(!mailbox) { print(stderr, "[ares-embed] no preview mailbox in RDRAM; is this a preview ROM?\n"); return; }
  u32 capacity = readWord(mailbox + MailboxCapacity);
  if(data.size() > capacity) {
    print(stderr, "[ares-embed] ", path, " is ", data.size(), " bytes; the mailbox holds ", capacity, "\n");
    return;
  }
  if(auto error = emulatorRunner.writeRdram(mailbox + MailboxHeader, data.data(), data.size())) {
    print(stderr, "[ares-embed] ", error, "\n");
    return;
  }
  writeWord(mailbox + MailboxLength, data.size());
  writeWord(mailbox + MailboxSequence, ++mailboxSequence);
}

}  //namespace

auto nall::main(Arguments arguments) -> void {
  ares::Memory::FixedAllocator::get();
  bool parallel = arguments.take("--parallel");
  string romPath = arguments.take();
  string framesPath = arguments.take();
  if(!romPath || !framesPath) { print(stderr, "usage: ares-embed [--parallel] <rom.z64> <frames-file>\n"); ::exit(2); }

  string pattern = {Path::temporary(), "ares-embed-XXXXXX"};
  if(!::mkdtemp(pattern.get())) { print(stderr, "[ares-embed] cannot create a work directory\n"); ::exit(2); }
  string workDir = {pattern, "/"};
  mia::setHomeLocation([workDir]() -> string { return workDir; });
  mia::setSaveLocation([workDir]() -> string { return {workDir, "Saves/"}; });
  ares::platform = &emulatorRunner;

  int fd = ::open(framesPath.data(), O_RDWR | O_CREAT, 0644);
  if(fd < 0 || ::ftruncate(fd, FileSize) != 0) { print(stderr, "[ares-embed] cannot open ", framesPath, "\n"); ::exit(2); }
  auto shared = (u8*)::mmap(nullptr, FileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(shared == MAP_FAILED) { print(stderr, "[ares-embed] cannot map ", framesPath, "\n"); ::exit(2); }
  auto header = (std::atomic<u32>*)shared;
  header[0] = FrameMagic;

  #if defined(ANGRYLION)
  ares::Nintendo64::angrylion.parallel = parallel;
  #endif
  emulatorRunner.homebrewMode = true;
  if(auto error = emulatorRunner.loadRom(romPath)) {
    print(stderr, "[ares-embed] ", error, "\n");
    directory::remove(workDir);
    ::exit(1);
  }
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
      if(command == "quit") {
        emulatorRunner.closeRom();
        directory::remove(workDir);
        ::exit(0);
      }
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
