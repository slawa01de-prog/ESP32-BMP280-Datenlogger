from pathlib import Path
import tempfile, subprocess
root=Path(__file__).resolve().parents[1]
s=(root/'Esp32_LCD_Datalogger/Esp32_LCD_Datalogger.ino').read_text()
history=s.split('// GRAPH743_BEGIN\n')[1].split('// GRAPH743_END')[0]
graph=s[s.index('void drawGraphScreen() {'):s.index('void drawPressureScreen()')]
pre=r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <algorithm>
#include <cstdio>
#include <type_traits>
#include <iostream>
using std::min;using std::max;using std::isfinite;
struct String:std::string {
 using std::string::string;String(const std::string&s):std::string(s){}
 template<class T,typename std::enable_if<std::is_integral<T>::value,int>::type=0> String(T v):std::string(std::to_string(v)){}
};
int constrain(int v,int a,int b){return max(a,min(v,b));}
String shown(float v,int n){char b[32];snprintf(b,sizeof(b),"%.*f",n,v);return String(b);}
struct {int graphWindow=0,graphMode=0,language=0;}cfg;
const uint8_t graphHours[4]={1,6,12,24};
uint64_t clockMs=0;uint64_t elapsedNow(){return clockMs;}
const char*txtDEEN(const char*a,const char*b){return cfg.language?b:a;}
int u8g2_font_5x8_tf=0;
struct OLED {
 int curves=0; bool empty=false;
 void clearBuffer(){curves=0;empty=false;}
 void setDrawColor(int){}void setFont(int){}
 void drawBox(int x,int y,int w,int h){assert(x>=0&&y>=0&&x+w<=128&&y+h<=64);}
 void drawStr(int x,int y,const char*s){assert(x>=0&&x+(int)std::string(s).size()*5<=128&&y<=64);if(std::string(s)=="Keine Daten"||std::string(s)=="No data")empty=true;}
 void drawVLine(int x,int y,int h){assert(x>=0&&x<128&&y>=0&&y+h<=64);if(x>=34){++curves;assert(y>=16&&y+h<=43);}}
 void drawHLine(int x,int y,int w){assert(x>=0&&x+w<=128&&y<64);}
 void drawLine(int x,int y,int a,int b){assert(x>=34&&a<128&&y>=16&&y<=42&&b>=16&&b<=42);}
}oled;
void sendOledIfChanged(){}
'''
tests=r'''
int main(){
 HourHistory h;
 assert(h.atAge(0,0)==nullptr);h.add(0,20,1000);assert(h.atAge(0,0)->temperature==20);
 h.add(1000,21,1001);assert(h.atAge(1000,0)->temperature==21);assert(h.atAge(1000,1)==nullptr);
 h.add(60000,22,1002);assert(h.atAge(60000,1)->temperature==21);
 assert(h.atAge(120000,0)==nullptr); // actual missing minute
 for(uint64_t m=2;m<=1560;m++)h.add(m*60000,23,1003);
 assert(h.atAge(1560ULL*60000,1440));assert(h.atAge(1560ULL*60000,1441)==nullptr);
 assert(h.atAge(1562ULL*60000,0)==nullptr);
 h.clear();assert(h.atAge(1560ULL*60000,0)==nullptr);
 uint64_t later=50ULL*86400000;h.add(later,25,1010);assert(h.atAge(later,0));
 h.add(later+1000,NAN,NAN);assert(!isfinite(h.atAge(later+1000,0)->temperature));
 // Empty, short, flat, irregular, negative and full-day series; both languages.
 for(int scenario=0;scenario<6;scenario++) {
  hourHistory.clear();clockMs=scenario<2?90000:25ULL*3600000;
  if(scenario==1)hourHistory.add(clockMs,23.5,1013);
  if(scenario>=2)for(int age=0;age<=1440;age++){
   if(scenario==3&&age>20&&age<60)continue;
   float t=scenario==2?23.5f:scenario==4?-35+5*sin(age/20.0):25+5*sin(age/20.0);
   hourHistory.add(clockMs-(uint64_t)age*60000,t,1000+12*cos(age/80.0));
  }
  for(int lang=0;lang<2;lang++)for(int mode=0;mode<2;mode++)for(int w=0;w<4;w++){
   cfg.language=lang;cfg.graphMode=mode;cfg.graphWindow=w;drawGraphScreen();
   assert(oled.empty==(scenario==0));if(scenario)assert(oled.curves>0);
  }
 }
 std::cout<<"PASS: real-minute timestamps, 24h wrap, gaps, invalid samples, 50-day uptime, reset\n";
 std::cout<<"PASS: graph drawing bounds and data presence across 96 scenarios (2 channels, 4 windows, DE/EN)\n";
 std::cout<<"History RAM bytes: "<<sizeof(HourHistory)<<" (host alignment)\n";
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.cpp').write_text(pre+history+'\nHourHistory hourHistory;\n'+graph+tests)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
assert s.count('prefs.putUChar("graphwin"')==1
assert 'cfg.graphWindow > 3' in s
assert 'if (graphVisible && (btnPush.pressedEvent || btnBak.pressedEvent))' in s
assert 'Serial.setTxTimeoutMs(0)' in s and 'Serial.print' not in s
