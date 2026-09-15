"""Run the actual firmware journal/spool/ACK functions with an in-memory radio and filesystem.
Requires g++ on a development PC; no board required. Firmware source is extracted at run time.
"""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'Esp32_LCD_Datalogger/Esp32_LCD_Datalogger.ino').read_text()
def function(name):
 m=re.search(r'(?m)^(?:void|bool|String|uint32_t) '+name+r'\([^\n]*\) \{',s)
 i=s.index('{',m.start())+1;d=1
 while d:
  if s[i]=='{':d+=1
  elif s[i]=='}':d-=1
  i+=1
 return s[m.start():i]
structs=s[s.index('struct __attribute__((packed)) PairReqPacket'):s.index('// --------------------------------------------------\n// Messwerte / Status')]
structs=structs[:structs.index('DataPacket radioPending')]
code=r'''
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <iostream>
struct String:std::string {
 using std::string::string;
 String(const std::string&s):std::string(s){}
 int indexOf(char c,int start=0)const {auto p=find(c,start);return p==npos?-1:(int)p;}
 int lastIndexOf(char c)const {auto p=rfind(c);return p==npos?-1:(int)p;}
 String substring(int a,int b)const{return substr(a,b-a);}
 String substring(int a)const{return substr(a);}
 float toFloat()const{return std::strtof(c_str(),nullptr);}
 long toInt()const{return std::strtol(c_str(),nullptr,10);}
};
#define PROTO_VERSION 3
#define PT_PAIR_REQ 1
#define PT_PAIR_ACCEPT 2
#define PT_DATA 3
#define MODE_SENSOR 0
#define WL_CONNECTED 3
'''+structs+r'''
uint32_t nowMs=1;
uint32_t millis(){return nowMs;}
struct {bool paired=true;int mode=0;uint8_t gatewayMac[6]={1,2,3,4,5,6};char nodeId[11]="B123456789";}cfg;
struct {int status(){return WL_CONNECTED;}uint8_t channel(){return 6;}void macAddress(uint8_t*p){memset(p,9,6);}}WiFi;
struct {uint8_t getUChar(const char*,int){return 6;}void putUChar(const char*,uint8_t){}}prefs;
String journal;
struct File {
 bool open=false;size_t pos=0;
 explicit operator bool()const{return open;}
 size_t size(){return journal.size();}
 void seek(size_t p){pos=p;}
 bool available(){return open && pos<journal.size();}
 String readStringUntil(char c){auto end=journal.find(c,pos);if(end==std::string::npos)end=journal.size();String line=journal.substr(pos,end-pos);pos=end<journal.size()?end+1:end;return line;}
 size_t position(){return pos;}
 void close(){open=false;}
};
struct {File open(const char*,const char*){File f;f.open=true;return f;}}LittleFS;
const char*LOCAL_LOG_FILE="local";
bool fsReady=true,radioWaiting=false;
uint32_t radioCursor=0,radioNextCursor=0,radioLastSend=0,radioConfirmed=0,lastGoodRxMs=0;
uint8_t radioTries=0,radioChannel=6;
DataPacket radioPending={};
int sent=0;
void esp_now_send(const uint8_t*,const uint8_t*,size_t){sent++;}
#define WIFI_SECOND_CHAN_NONE 0
void esp_wifi_set_channel(int,int){}
bool isSensorRole(){return true;}
bool macEqual(const uint8_t*a,const uint8_t*b){return memcmp(a,b,6)==0;}
void handlePairReq(const PairReqPacket&){}
void handlePairAccept(const PairAcceptPacket&){}
void handleData(const DataPacket&){}
uint32_t hashString(const String&s){uint32_t h=2166136261U;for(unsigned char c:s)h=(h^c)*16777619U;return h;}
'''+function('parseJournal')+'\n'+function('serviceRadioSpool')+'\n'+function('handleIncoming')+r'''
int main(){
 String row="NODEDATA,B123456789,Logger,V7.4,1,23.5,23.5,1000,1000,0,0,2,0,0,09:09:09:09:09:09,17,100,1000,1780000000000,7,60";
 char checksum[16];snprintf(checksum,sizeof(checksum),"%x",hashString(row));journal=row+","+checksum+"\n";
 serviceRadioSpool();assert(sent==1 && radioWaiting && radioCursor==0);
 assert(radioPending.sequence==17 && radioPending.session==100);
 nowMs+=300;serviceRadioSpool();assert(sent==2 && radioCursor==0);
 AckPacket ack={};memcpy(ack.magic,"BPN2",4);ack.proto=3;ack.type=5;strcpy(ack.id,cfg.nodeId);ack.session=100;ack.sequence=16;
 handleIncoming((uint8_t*)&ack,sizeof(ack),cfg.gatewayMac);assert(radioWaiting && radioCursor==0);
 ack.sequence=17;uint8_t wrong[6]={};handleIncoming((uint8_t*)&ack,sizeof(ack),wrong);assert(radioWaiting);
 handleIncoming((uint8_t*)&ack,sizeof(ack),cfg.gatewayMac);assert(!radioWaiting && radioCursor==journal.size() && radioConfirmed==1);
 serviceRadioSpool();assert(sent==2);
 DataPacket p;assert(!parseJournal(row+",bad",p));
 std::cout<<"PASS: journal parse, ACK cursor, retry, stale ACK, wrong sender, EOF, checksum\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
