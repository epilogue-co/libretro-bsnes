#include <sfc/sfc.hpp>

namespace SuperFamicom {

#include "memory.cpp"
#include "time.cpp"
#include "serialization.cpp"
SharpRTC sharprtc;

auto SharpRTC::synchronizeCPU() -> void {
  if(clock >= 0) scheduler.resume(cpu.thread);
}

auto SharpRTC::Enter() -> void {
  while(true) {
    scheduler.synchronize();
    sharprtc.main();
  }
}

auto SharpRTC::main() -> void {
  tickSecond();

  step(1);
  synchronizeCPU();
}

auto SharpRTC::step(uint clocks) -> void {
  clock += clocks * (uint64_t)cpu.frequency;
}

auto SharpRTC::initialize() -> void {
  second = 0;
  minute = 0;
  hour = 0;
  day = 0;
  month = 0;
  year = 0;
  weekday = 0;
}

auto SharpRTC::power() -> void {
  create(SharpRTC::Enter, 1);

  state = State::Read;
  index = -1;
}

auto SharpRTC::synchronize(uint64 timestamp) -> void {
  time_t systime = timestamp;
  tm* timeinfo = localtime(&systime);

  second = min(59, timeinfo->tm_sec);
  minute = timeinfo->tm_min;
  hour = timeinfo->tm_hour;
  day = timeinfo->tm_mday;
  month = 1 + timeinfo->tm_mon;

  //The S-RTC year is three BCD digits over an implicit 1000 base (registers 9-11), so it can
  //only represent 1000-1999. A real year >= 2000 overflows the hundreds digit to an invalid
  //BCD value (10+) that the games reject. Fold the year back in whole 28-year calendar cycles
  //into 1972-1999: this keeps the weekday and leap-year pattern identical to the real date
  //(so calculateWeekday stays consistent) while staying within the chip's range.
  uint fullyear = 1900 + timeinfo->tm_year;
  while(fullyear > 1999) fullyear -= 28;
  year = fullyear - 1000;

  weekday = timeinfo->tm_wday;
}

auto SharpRTC::read(uint addr, uint8 data) -> uint8 {
  addr &= 1;

  if(addr == 0) {
    if(state != State::Read) return 0;

    if(index < 0) {
      index++;
      return 15;
    } else if(index > 12) {
      index = -1;
      return 15;
    } else {
      return rtcRead(index++);
    }
  }

  return data;
}

auto SharpRTC::write(uint addr, uint8 data) -> void {
  addr &= 1, data &= 15;

  if(addr == 1) {
    if(data == 0x0d) {
      state = State::Read;
      index = -1;
      return;
    }

    if(data == 0x0e) {
      state = State::Command;
      return;
    }

    if(data == 0x0f) return;  //unknown behavior

    if(state == State::Command) {
      if(data == 0) {
        state = State::Write;
        index = 0;
      } else if(data == 4) {
        state = State::Ready;
        index = -1;
        //reset time
        second = 0;
        minute = 0;
        hour = 0;
        day = 0;
        month = 0;
        year = 0;
        weekday = 0;
      } else {
        //unknown behavior
        state = State::Ready;
      }
      return;
    }

    if(state == State::Write) {
      if(index >= 0 && index < 12) {
        rtcWrite(index++, data);
        if(index == 12) {
          //day of week is automatically calculated and written
          weekday = calculateWeekday(1000 + year, month, day);
        }
      }
      return;
    }
  }
}

}
