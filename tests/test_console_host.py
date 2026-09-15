"""Compile the real nonblocking console against a stalled/short-writing transport."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp)
    source=(root/'Esp32_LCD_Datalogger/Esp32_LCD_Datalogger.ino').read_text()
    console=source[source.index('class LoggerConsole : public Print {'):source.index('LoggerConsole loggerConsole;')]
    (d/'LoggerConsole.h').write_text('#include <Arduino.h>\n'+console)
    (d/'Arduino.h').write_text('''#pragma once
#include <cstddef>
#include <cstdint>
class Print { public: virtual size_t write(uint8_t)=0; virtual size_t write(const uint8_t*,size_t)=0; };
''')
    (d/'test.cpp').write_text(r'''
#include "LoggerConsole.h"
#include <cassert>
#include <string>
#include <iostream>
struct Host {
 int room=0; size_t limit=256; int calls=0; std::string received;
 int availableForWrite(){return room;}
 size_t write(const uint8_t*p,size_t n){++calls;assert(n<=256 && n<=(size_t)room);if(n>limit)n=limit;received.append((const char*)p,n);return n;}
};
void put(LoggerConsole&c,const std::string&s){c.write((const uint8_t*)s.data(),s.size());}
void drain(LoggerConsole&c,Host&h){for(int i=0;i<20000 && c.freeBytes()!=8192;i++)c.pump(h);assert(c.freeBytes()==8192);}
int main(){
 LoggerConsole c; Host h;
 put(c,"partial");h.room=256;c.pump(h);assert(h.calls==0);
 put(c," line\n");h.room=0;
 for(int i=0;i<100000;i++)c.pump(h);assert(h.calls==0);
 h.room=256;h.limit=0;c.pump(h);assert(c.freeBytes()==8192-13);
 h.limit=3;drain(c,h);assert(h.received=="partial line\n");
 // Sustained traffic, wrapping, short writes and reconnect retain exact order.
 h.received.clear();std::string expected;
 for(int i=0;i<6000;i++){
  std::string row="NODEDATA,"+std::to_string(i)+",23.4,checksum\n";
  if(c.freeBytes()<4096)drain(c,h);
  put(c,row);expected+=row;c.pump(h);
 }
 drain(c,h);assert(h.received==expected);assert(c.droppedLines()==0);
 // Offline overflow drops whole diagnostic lines, never a partial prefix.
 h.received.clear();h.room=0;
 for(int i=0;i<2000;i++)put(c,"OK\n");
 put(c,std::string(1025,'X')+"\n");assert(c.droppedLines()==1);
 for(int i=0;i<2000;i++)put(c,"OK\n");
 assert(c.droppedLines()>1);h.room=256;drain(c,h);
 assert(h.received.size()%3==0);
 for(size_t i=0;i<h.received.size();i+=3)assert(h.received.substr(i,3)=="OK\n");
 put(c,"RECOVERED\n");h.received.clear();drain(c,h);assert(h.received=="RECOVERED\n");
 std::cout<<"PASS: stalled host, bounded writes, short writes, wrap, whole-line overflow, reconnect\n";
}
''')
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(d),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True,timeout=10)
source=(root/'Esp32_LCD_Datalogger/Esp32_LCD_Datalogger.ino').read_text()
assert 'Serial.print' not in source and 'Serial.flush(' not in source
assert 'Serial.setTxTimeoutMs(0)' in source
assert 'if (loggerConsole.freeBytes() < 4096) return;' in source
setup=source[source.index('void setup() {'):source.index('void loop() {')]
assert 'printHelp();' not in setup
assert setup.index('updateDisplay();') < setup.index('printStatus();')
print('PASS: firmware TX routing, zero HWCDC timeout, archive backpressure, dashboard before boot status')
