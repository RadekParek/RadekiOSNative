#include "ipa/png.h"
#include "ipa/inflate.h"
#include <algorithm>
#include <cstring>
#include <limits>
namespace radeki::ipa {
namespace {
constexpr uint8_t signature[]={137,80,78,71,13,10,26,10};
uint32_t be(const uint8_t* p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
void put(std::vector<uint8_t>& v,uint32_t x){for(int i=3;i>=0;--i)v.push_back(uint8_t(x>>(i*8)));}
void chunk(std::vector<uint8_t>& v,const char* tag,const std::vector<uint8_t>& bytes){
  put(v,uint32_t(bytes.size()));size_t begin=v.size();v.insert(v.end(),tag,tag+4);
  v.insert(v.end(),bytes.begin(),bytes.end());put(v,crc32(v.data()+begin,4+bytes.size()));
}
int channels(unsigned type){switch(type){case 0:return 1;case 2:return 3;case 3:return 1;case 4:return 2;case 6:return 4;default:return 0;}}
struct Parsed {PngInfo info;std::vector<uint8_t> idat,plte,trns;};
bool parse(const std::vector<uint8_t>& bytes,Parsed& p,std::string& error){
  if(bytes.size()<8 || std::memcmp(bytes.data(),signature,8)){error="invalid PNG signature";return false;}
  bool head=false,end=false,data=false;
  for(size_t pos=8;pos<bytes.size();) {
    if(bytes.size()-pos<12){error="truncated PNG chunk";return false;}
    uint32_t n=be(bytes.data()+pos);pos+=4;
    if(size_t(n)>bytes.size()-pos-8){error="PNG chunk outside input";return false;}
    const uint8_t* tag=bytes.data()+pos;const uint8_t* content=tag+4;
    if(be(content+n)!=crc32(tag,n+4)){error="PNG CRC mismatch";return false;}
    if(!std::memcmp(tag,"CgBI",4)) {if(head || data){error="CgBI after image header";return false;}p.info.cgbi=true;}
    else if(!std::memcmp(tag,"IHDR",4)) {
      if(head||n!=13||data){error="invalid IHDR";return false;}
      head=true;p.info.width=be(content);p.info.height=be(content+4);
      p.info.bitDepth=content[8];p.info.colorType=content[9];
      if(!p.info.width||!p.info.height||(p.info.bitDepth!=8&&(p.info.bitDepth!=16||p.info.colorType==3))||
         !channels(p.info.colorType)||content[10]||content[11]||content[12]) {
        error="unsupported PNG format (8/16-bit, non-interlaced only)";return false;
      }
    } else if(!std::memcmp(tag,"PLTE",4)) {
      if(!head||data||n==0||n>768||n%3!=0){error="invalid PLTE chunk";return false;}
      p.plte.assign(content,content+n);
    } else if(!std::memcmp(tag,"tRNS",4)) {
      if(!head||data||n>256){error="invalid tRNS chunk";return false;}
      p.trns.assign(content,content+n);
    } else if(!std::memcmp(tag,"IDAT",4)) {
      if(!head||end||n>32*1024*1024 || p.idat.size()>32*1024*1024-n){error="invalid or oversized PNG IDAT";return false;}
      data=true;p.idat.insert(p.idat.end(),content,content+n);
    } else if(!std::memcmp(tag,"IEND",4)) {
      if(!head||!data||n){error="invalid PNG end";return false;}
      end=true;
    } else if ((tag[0]&32)==0) {error="unsupported critical PNG chunk";return false;}
    pos+=size_t(n)+8;
    if(end)break;
  }
  if(!end){error="missing PNG IEND";return false;}
  if(p.info.colorType==3 && p.plte.empty()){error="missing PLTE chunk for indexed PNG";return false;}
  return true;
}
}
bool pngInfo(const std::vector<uint8_t>& bytes,PngInfo& info,std::string& error){Parsed p;if(!parse(bytes,p,error))return false;info=p.info;return true;}
bool decodePng(const std::vector<uint8_t>& bytes,RgbaImage& img,std::string& error){
  Parsed p;if(!parse(bytes,p,error))return false;
  size_t w=p.info.width,h=p.info.height,ch=channels(p.info.colorType),sampleBytes=p.info.bitDepth==16?2:1,bpp=ch*sampleBytes;
  if(w>8192||h>8192||w>(std::numeric_limits<size_t>::max()-1)/bpp || h>64*1024*1024/(1+w*bpp)) {error="PNG dimensions exceed limit";return false;}
  size_t stride=w*bpp,limit=h*(stride+1);
  std::vector<uint8_t> raw;
  bool ok=p.info.cgbi?inflateRaw(p.idat,raw,limit,error):zlibInflate(p.idat,raw,limit,error);
  if(!ok && p.info.cgbi){
    if(zlibInflate(p.idat,raw,limit,error))ok=true;
    else {
      for(size_t trim=1;trim<=8&&trim<p.idat.size();++trim){
        std::vector<uint8_t> sub(p.idat.begin(),p.idat.end()-trim);
        if(inflateRaw(sub,raw,limit,error)&&raw.size()==limit){ok=true;break;}
      }
    }
  }
  if(!ok)return false;
  if(raw.size()!=limit){error="PNG pixel data length mismatch";return false;}
  img.width=p.info.width;img.height=p.info.height;img.rgba.resize(w*h*4);
  std::vector<uint8_t> prev(stride,0),cur(stride);
  for(size_t y=0;y<h;++y){
    const uint8_t* row=raw.data()+y*(stride+1);unsigned filter=row[0];
    if(filter>4){error="invalid PNG filter";return false;}
    for(size_t x=0;x<stride;++x){
      uint8_t a=x>=bpp?cur[x-bpp]:0,b=prev[x],c=x>=bpp?prev[x-bpp]:0;
      int predictor=0;
      if(filter==1)predictor=a;
      if(filter==2)predictor=b;
      if(filter==3)predictor=(int(a)+b)/2;
      if(filter==4){int pa=std::abs(int(b)-c),pb=std::abs(int(a)-c),pc=std::abs(int(a)+b-2*int(c));predictor=pa<=pb&&pa<=pc?a:pb<=pc?b:c;}
      cur[x]=uint8_t(row[x+1]+predictor);
    }
    for(size_t x=0;x<w;++x){
      const uint8_t* src=cur.data()+x*bpp;uint8_t* dst=img.rgba.data()+(y*w+x)*4;
      auto s=[&](size_t idx){return src[idx*sampleBytes];};
      switch(p.info.colorType){
        case 0:dst[0]=dst[1]=dst[2]=s(0);dst[3]=255;break;
        case 2:dst[0]=s(0);dst[1]=s(1);dst[2]=s(2);dst[3]=255;break;
        case 3:{
          size_t idx=src[0];
          if(idx*3+2<p.plte.size()){dst[0]=p.plte[idx*3];dst[1]=p.plte[idx*3+1];dst[2]=p.plte[idx*3+2];}
          else dst[0]=dst[1]=dst[2]=0;
          dst[3]=idx<p.trns.size()?p.trns[idx]:255;
          break;
        }
        case 4:dst[0]=dst[1]=dst[2]=s(0);dst[3]=s(1);break;
        case 6:dst[0]=s(0);dst[1]=s(1);dst[2]=s(2);dst[3]=s(3);break;
      }
      if(p.info.cgbi && p.info.colorType!=3){std::swap(dst[0],dst[2]);if(dst[3]<255)for(int k=0;k<3;++k)
        dst[k]=dst[3]?uint8_t(std::min(255,(int(dst[k])*255+dst[3]/2)/dst[3])):0;}
    }
    prev.swap(cur);
  }
  return true;
}
std::vector<uint8_t> encodePng(const RgbaImage& img){
  if(!img.width||!img.height||img.width>8192||img.height>8192||img.rgba.size()!=size_t(img.width)*img.height*4)return {};
  std::vector<uint8_t> out(signature,signature+8),header,raw;
  put(header,img.width);put(header,img.height);header.insert(header.end(),{8,6,0,0,0});chunk(out,"IHDR",header);
  for(uint32_t y=0;y<img.height;++y){raw.push_back(0);raw.insert(raw.end(),img.rgba.begin()+size_t(y)*img.width*4,img.rgba.begin()+size_t(y+1)*img.width*4);}
  chunk(out,"IDAT",zlibDeflateStored(raw));chunk(out,"IEND",{});return out;
}
}
