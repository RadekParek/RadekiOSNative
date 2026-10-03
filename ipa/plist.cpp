#include "ipa/plist.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace radeki::ipa {
const Plist* Plist::find(const std::string& key) const {auto it=dict.find(key);return it==dict.end()?nullptr:&it->second;}
const Plist* Plist::path(const std::string& dotted) const {
  const Plist* cur=this;size_t pos=0;
  while(cur && pos<dotted.size()){size_t end=dotted.find('.',pos);cur=cur->find(dotted.substr(pos,end==std::string::npos?end:end-pos));if(end==std::string::npos)break;pos=end+1;}
  return cur;
}
std::string Plist::stringOr(const std::string& fallback) const {return type==Type::String?text:fallback;}
int64_t Plist::intOr(int64_t fallback) const {return type==Type::Integer?integer:fallback;}
std::vector<std::string> Plist::stringsOr() const {std::vector<std::string> v;if(type==Type::Array)for(const auto& x:array)if(x.type==Type::String)v.push_back(x.text);return v;}
namespace {
void fail(const char* s){throw std::runtime_error(s);}
uint64_t number(const std::vector<uint8_t>& b,size_t pos,size_t n){
  if(n>8||pos>b.size()||n>b.size()-pos)fail("binary plist field outside input");
  uint64_t v=0;for(size_t i=0;i<n;++i)v=(v<<8)|b[pos+i];return v;
}
size_t bounded(uint64_t n,size_t limit){if(n>limit)fail("binary plist count exceeds limit");return size_t(n);}
void utf8(std::string& s,uint32_t c){
  if(c>0x10ffff || (c>=0xd800&&c<=0xdfff))fail("invalid UTF-16 scalar");
  if(c<128)s.push_back(char(c));
  else if(c<2048){s.push_back(char(0xc0|(c>>6)));s.push_back(char(0x80|(c&63)));}
  else if(c<65536){s.push_back(char(0xe0|(c>>12)));s.push_back(char(0x80|((c>>6)&63)));s.push_back(char(0x80|(c&63)));}
  else {s.push_back(char(0xf0|(c>>18)));s.push_back(char(0x80|((c>>12)&63)));s.push_back(char(0x80|((c>>6)&63)));s.push_back(char(0x80|(c&63)));}
}
struct Binary {
  const std::vector<uint8_t>& b;
  std::vector<uint64_t> offsets;
  size_t trailer,refSize;
  std::vector<bool> active;
  uint64_t integer(size_t& pos){
    if(pos>=trailer)fail("truncated integer object");
    uint8_t m=b[pos++];if((m>>4)!=1 || (m&15)>4)fail("invalid extended plist length");
    size_t n=1u<<(m&15);
    uint64_t v=n==16?number(b,pos+8,8):number(b,pos,n);
    pos+=n;return v;
  }
  size_t length(uint8_t m,size_t& pos){return (m&15)==15?bounded(integer(pos),b.size()):size_t(m&15);}
  Plist item(size_t i,int depth=0){
    if(i>=offsets.size()||depth>64||active[i])fail("invalid or recursive plist object reference");
    active[i]=true;
    size_t pos=bounded(offsets[i],trailer);
    if(pos>=trailer)fail("object offset outside data");
    uint8_t m=b[pos++],kind=m>>4;
    Plist p;
    if(kind==0){if(m==0x08){p.type=Plist::Type::Bool;p.boolean=false;}else if(m==0x09){p.type=Plist::Type::Bool;p.boolean=true;}else if(m!=0&&m!=0x0f)fail("unsupported plist simple value");}
    else if(kind==1){size_t n=1u<<(m&15);if(n>16)fail("oversized plist integer");uint64_t v=n==16?number(b,pos+8,8):number(b,pos,n);if(n<8 && (v&(uint64_t(1)<<(n*8-1))))v|=(~uint64_t(0)<<(n*8));p.type=Plist::Type::Integer;p.integer=int64_t(v);}
    else if(kind==2||kind==3){
      size_t n=1u<<(m&15);if((kind==2 && n!=4 && n!=8)||(kind==3 && m!=0x33))fail("unsupported plist real/date");
      uint64_t v=number(b,pos,n);double d=0;
      if(n==4){uint32_t bits=uint32_t(v);float f;std::memcpy(&f,&bits,4);d=f;}
      else std::memcpy(&d,&v,8);
      if(!std::isfinite(d))fail("non-finite plist real");
      p.type=kind==2?Plist::Type::Real:Plist::Type::Date;p.real=d;
    } else if(kind==4||kind==5||kind==6){
      size_t n=length(m,pos);size_t unit=kind==6?2:1;
      if(n>(trailer-pos)/unit || n>8*1024*1024)fail("plist data/string outside input or oversized");
      if(kind==4){p.type=Plist::Type::Data;p.data.assign(b.begin()+pos,b.begin()+pos+n);}
      else {p.type=Plist::Type::String;
        if(kind==5){p.text.assign(reinterpret_cast<const char*>(b.data()+pos),n);}
        else for(size_t k=0;k<n;++k){uint32_t ch=number(b,pos+k*2,2);
          if(ch>=0xd800 && ch<=0xdbff){if(++k>=n)fail("truncated UTF-16 surrogate");uint32_t low=number(b,pos+k*2,2);if(low<0xdc00||low>0xdfff)fail("invalid UTF-16 surrogate");ch=0x10000+((ch-0xd800)<<10)+(low-0xdc00);}
          utf8(p.text,ch);
        }
      }
    } else if(kind==8){size_t n=(m&15)+1;p.type=Plist::Type::Uid;p.integer=int64_t(number(b,pos,n));}
    else if(kind==10||kind==11||kind==12||kind==13){
      size_t n=length(m,pos);size_t factor=kind==13?2:1;
      if(n>100000 || n>(trailer-pos)/refSize/factor)fail("plist references outside input or oversized");
      p.type=kind==13?Plist::Type::Dict:Plist::Type::Array;
      for(size_t k=0;k<n;++k){size_t v=bounded(number(b,pos+(k+(kind==13?n:0))*refSize,refSize),offsets.size());
        if(kind!=13)p.array.push_back(item(v,depth+1));
        else {size_t key=bounded(number(b,pos+k*refSize,refSize),offsets.size());Plist name=item(key,depth+1);if(name.type!=Plist::Type::String)fail("non-string plist dictionary key");
          p.dict[name.text]=item(v,depth+1);}
      }
    } else fail("unsupported binary plist object type");
    active[i]=false;return p;
  }
};
struct Xml {
  std::string s;size_t pos=0;
  void ws(){while(pos<s.size() && (s[pos]==' '||s[pos]=='\n'||s[pos]=='\t'||s[pos]=='\r'))++pos;}
  bool take(const std::string& x){if(s.compare(pos,x.size(),x)==0){pos+=x.size();return true;}return false;}
  void misc(){
    if(pos==0 && s.compare(0,3,"\xEF\xBB\xBF")==0)pos=3;
    for(;;){ws();
      if(take("<?")){size_t e=s.find("?>",pos);if(e==std::string::npos)fail("unterminated XML declaration");pos=e+2;}
      else if(take("<!--")){size_t e=s.find("-->",pos);if(e==std::string::npos)fail("unterminated XML comment");pos=e+3;}
      else if(take("<!DOCTYPE")){
        int brackets=0;
        while(pos<s.size()){char c=s[pos++];if(c=='[')++brackets;else if(c==']'&&brackets>0)--brackets;else if(c=='>'&&brackets==0)break;}
      }
      else break;
    }
  }
  std::string entityText(const std::string& raw){
    std::string result;
    for(size_t i=0;i<raw.size();++i){if(raw[i]!='&'){result+=raw[i];continue;}
      size_t e=raw.find(';',i);if(e==std::string::npos)fail("unterminated XML entity");std::string x=raw.substr(i+1,e-i-1);i=e;
      if(x=="amp")result+='&';else if(x=="lt")result+='<';else if(x=="gt")result+='>';
      else if(x=="quot")result+='"';else if(x=="apos")result+='\'';
      else if(x.size()>1 && x[0]=='#'){
        int base=x[1]=='x'||x[1]=='X'?16:10;size_t begin=base==16?2:1;
        if(begin>=x.size())fail("empty numeric XML entity");
        uint32_t v=0;
        for(size_t j=begin;j<x.size();++j){char c=x[j];int digit=c>='0'&&c<='9'?c-'0':base==16&&c>='a'&&c<='f'?c-'a'+10:base==16&&c>='A'&&c<='F'?c-'A'+10:-1;
          if(digit<0||digit>=base||v>(0x10ffff-unsigned(digit))/unsigned(base))fail("invalid numeric XML entity");
          v=v*base+unsigned(digit);}
        utf8(result,v);
      }else fail("unknown XML entity");
    }
    return result;
  }
  std::string content(const std::string& tag){
    std::string value;
    for(;;){
      size_t e=s.find('<',pos);if(e==std::string::npos)fail("unterminated XML element");
      value+=entityText(s.substr(pos,e-pos));pos=e;
      if(take("<![CDATA[")){size_t ce=s.find("]]>",pos);if(ce==std::string::npos)fail("unterminated CDATA");value+=s.substr(pos,ce-pos);pos=ce+3;}
      else break;
    }
    if(!take("</"+tag))fail("mismatched XML closing tag");
    ws();if(!take(">"))fail("mismatched XML closing tag");
    return value;
  }
  static std::vector<uint8_t> base64(const std::string& s){std::vector<uint8_t> out;int v=0,bits=-8;bool padded=false;
    for(unsigned char c:s){if(c==' '||c=='\r'||c=='\n'||c=='\t')continue;if(c=='='){padded=true;continue;}
      if(padded)fail("data after base64 padding");
      int digit=c>='A'&&c<='Z'?c-'A':c>='a'&&c<='z'?c-'a'+26:c>='0'&&c<='9'?c-'0'+52:c=='+'?62:c=='/'?63:-1;
      if(digit<0)fail("invalid base64 data");
      v=(v<<6)|digit;bits+=6;if(bits>=0){out.push_back(uint8_t(v>>bits));bits-=8;v&=(1<<(-bits))-1;}}
    if(bits>=-6 && (v!=0 || (bits!=-8 && !padded)))fail("invalid base64 tail");
    return out;
  }
  static std::string trimWs(const std::string& in){
    size_t a=0,b=in.size();
    while(a<b && (in[a]==' '||in[a]=='\n'||in[a]=='\t'||in[a]=='\r'))++a;
    while(b>a && (in[b-1]==' '||in[b-1]=='\n'||in[b-1]=='\t'||in[b-1]=='\r'))--b;
    return in.substr(a,b-a);
  }
  Plist node(int depth=0){if(depth>64)fail("XML plist nesting limit");misc();if(!take("<"))fail("expected XML plist element");
    size_t start=pos;while(pos<s.size() && ((s[pos]>='a'&&s[pos]<='z')||(s[pos]>='A'&&s[pos]<='Z')))++pos;
    std::string tag=s.substr(start,pos-start);ws();bool empty=take("/>");if(!empty && !take(">"))fail("invalid XML plist tag");
    Plist p;
    if(tag=="true"||tag=="false"){if(!empty && !content(tag).empty())fail("boolean with content");p.type=Plist::Type::Bool;p.boolean=tag=="true";}
    else if(tag=="dict"||tag=="array"){
      p.type=tag=="dict"?Plist::Type::Dict:Plist::Type::Array;
      if(!empty)for(;;){misc();if(take("</"+tag)){ws();if(!take(">"))fail("invalid closing tag");break;}if(pos>=s.size())fail("unterminated XML plist container");
        if(tag=="array")p.array.push_back(node(depth+1));
        else {Plist key=node(depth+1);if(key.type!=Plist::Type::String)fail("plist dictionary key must be a string");Plist value=node(depth+1);
          p.dict[key.text]=std::move(value);}
      }
    }else if(tag=="key"||tag=="string"||tag=="integer"||tag=="real"||tag=="data"||tag=="date"){
      std::string v=empty?"":content(tag);
      if(tag=="key"||tag=="string"){p.type=Plist::Type::String;p.text=v;}
      else if(tag=="date"){p.type=Plist::Type::Date;p.text=v;}
      else if(tag=="data"){p.type=Plist::Type::Data;p.data=base64(v);}
      else {v=trimWs(v);size_t consumed=0;try{if(tag=="integer"){p.type=Plist::Type::Integer;p.integer=std::stoll(v,&consumed,0);}else{p.type=Plist::Type::Real;p.real=std::stod(v,&consumed);if(!std::isfinite(p.real))fail("non-finite plist real");}}catch(const std::invalid_argument&){fail("invalid plist number");}catch(const std::out_of_range&){fail("plist number out of range");}if(consumed!=v.size()||v.empty())fail("invalid plist number");}
    }else fail("unknown XML plist element");return p;
  }
};
}
bool parsePlist(const std::vector<uint8_t>& bytes,Plist& out,std::string& error){
  out={};error.clear();
  try{
    if(bytes.size()>=8 && !std::memcmp(bytes.data(),"bplist00",8)){
      if(bytes.size()<40)fail("truncated binary plist trailer");
      size_t trailer=bytes.size()-32;unsigned offsetSize=bytes[trailer+6],refSize=bytes[trailer+7];
      if(!offsetSize||offsetSize>8||!refSize||refSize>8)fail("invalid binary plist field widths");
      size_t count=bounded(number(bytes,trailer+8,8),100000),top=bounded(number(bytes,trailer+16,8),count);
      size_t table=bounded(number(bytes,trailer+24,8),trailer);
      if(!count||top>=count||count>(trailer-table)/offsetSize)fail("invalid binary plist offset table");
      Binary b{bytes,{},trailer,refSize,{}};for(size_t i=0;i<count;++i)b.offsets.push_back(number(bytes,table+i*offsetSize,offsetSize));
      b.active.resize(count);out=b.item(top);
    }else{
      Xml x{std::string(bytes.begin(),bytes.end())};x.misc();
      if(!x.take("<plist"))fail("missing XML plist wrapper");
      size_t e=x.s.find('>',x.pos);if(e==std::string::npos||e-x.pos>256)fail("invalid plist wrapper");x.pos=e+1;
      out=x.node();x.misc();if(!x.take("</plist>"))fail("missing closing plist wrapper");x.misc();if(x.pos!=x.s.size())fail("trailing XML plist data");
    }
    return true;
  }catch(const std::exception& e){error=e.what();out={};return false;}
}
}
