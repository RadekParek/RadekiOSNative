#include "ipa/inflate.h"
#include <algorithm>
#include <array>
#include <limits>
namespace radeki::ipa {
uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = ~0u;
  for (size_t i=0; i<n; ++i) { c ^= d[i]; for (int k=0;k<8;++k) c = (c>>1) ^ (0xEDB88320u & -(c&1u)); }
  return ~c;
}
uint32_t adler32(const uint8_t* d, size_t n) {
  uint32_t a=1,b=0;
  for (size_t i=0;i<n;++i) { a=(a+d[i])%65521; b=(b+a)%65521; }
  return (b<<16)|a;
}
namespace {
struct Bits {
  const std::vector<uint8_t>& in; size_t bit=0;
  bool get(unsigned count, unsigned& out) {
    if (count>24 || bit>in.size()*8 || count>in.size()*8-bit) return false;
    out=0;
    for (unsigned k=0;k<count;++k) out |= unsigned((in[(bit+k)/8]>>((bit+k)%8))&1)<<k;
    bit+=count; return true;
  }
};
struct Huff {
  std::array<std::vector<int>,16> code;
  bool build(const std::vector<unsigned>& lengths, std::string& error) {
    std::array<unsigned,16> counts{}, next{};
    for (auto n:lengths) { if (n>15) {error="invalid Huffman code length";return false;} if(n) ++counts[n]; }
    int left=1;
    for (int n=1;n<=15;++n) {left=left*2-int(counts[n]); if(left<0){error="oversubscribed Huffman codes";return false;} }
    unsigned c=0;
    for (int n=1;n<=15;++n) { c=(c+counts[n-1])*2; next[n]=c; code[n].assign(1u<<n,-1); }
    for (size_t sym=0;sym<lengths.size();++sym) if (unsigned n=lengths[sym]) {
      unsigned canonical=next[n]++, reversed=0;
      for (unsigned k=0;k<n;++k) reversed=(reversed<<1)|((canonical>>k)&1);
      code[n][reversed]=int(sym);
    }
    return true;
  }
  bool decode(Bits& b, unsigned& sym) const {
    unsigned v=0,x;
    for (unsigned n=1;n<=15;++n) {
      if(!b.get(1,x)) return false;
      v |= x<<(n-1);
      if (code[n][v]>=0) {sym=unsigned(code[n][v]);return true;}
    }
    return false;
  }
};
constexpr int lengthBase[]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
constexpr int lengthExtra[]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
constexpr int distBase[]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
constexpr int distExtra[]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
}
bool inflateRaw(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, size_t limit, std::string& error) {
  out.clear(); error.clear();
  if (in.size()>std::numeric_limits<size_t>::max()/8) {error="deflate input too large";return false;}
  Bits b{in}; unsigned final=0, type=0, n=0;
  do {
    if(!b.get(1,final)||!b.get(2,type)) {error="truncated deflate block";return false;}
    if (type==0) {
      b.bit=(b.bit+7)&~size_t(7);
      unsigned len=0, check=0;
      if(!b.get(16,len)||!b.get(16,check)) {error="truncated stored block";return false;}
      if ((len^check)!=65535) {error="invalid stored block length";return false;}
      if (len>limit-out.size() || len>(in.size()*8-b.bit)/8) {error="stored block exceeds limit or input";return false;}
      for (unsigned k=0;k<len;++k) {b.get(8,n);out.push_back(uint8_t(n));}
      continue;
    }
    if (type==3) {error="reserved deflate block type";return false;}
    std::vector<unsigned> ll, dd;
    if (type==1) {
      ll.resize(288);dd.assign(32,5);
      for(unsigned i=0;i<288;++i) ll[i]=i<144?8:i<256?9:i<280?7:8;
    } else {
      unsigned hlit,hdist,hclen;
      if(!b.get(5,hlit)||!b.get(5,hdist)||!b.get(4,hclen)) {error="truncated dynamic header";return false;}
      hlit+=257;hdist+=1;hclen+=4;
      if (hlit>286 || hdist>32) {error="invalid dynamic alphabet size";return false;}
      static constexpr unsigned order[]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
      std::vector<unsigned> cl(19);
      for(unsigned i=0;i<hclen;++i) if(!b.get(3,cl[order[i]])) {error="truncated code lengths";return false;}
      Huff alphabet;
      if(!alphabet.build(cl,error)) return false;
      std::vector<unsigned> all;
      while(all.size()<hlit+hdist) {
        unsigned v,rep=1,value=0;
        if(!alphabet.decode(b,v)) {error="invalid dynamic code length";return false;}
        if(v<=15) value=v;
        else if(v==16) { if(all.empty()||!b.get(2,n)){error="invalid repeat code";return false;} value=all.back();rep=n+3; }
        else if(v==17) {if(!b.get(3,n)){error="truncated repeat";return false;}rep=n+3;}
        else if(v==18) {if(!b.get(7,n)){error="truncated repeat";return false;}rep=n+11;}
        else {error="invalid repeat symbol";return false;}
        if(rep>hlit+hdist-all.size()) {error="code lengths exceed alphabet";return false;}
        all.insert(all.end(),rep,value);
      }
      ll.assign(all.begin(),all.begin()+hlit);dd.assign(all.begin()+hlit,all.end());
    }
    if(ll.size()<=256 || ll[256]==0) {error="missing end-of-block code";return false;}
    Huff lit,dist;
    if(!lit.build(ll,error)||!dist.build(dd,error))return false;
    for(;;) {
      unsigned v;
      if(!lit.decode(b,v)){error="invalid or truncated literal/length";return false;}
      if(v<256) {if(out.size()>=limit){error="output limit exceeded";return false;}out.push_back(uint8_t(v));continue;}
      if(v==256)break;
      if(v>285){error="invalid length symbol";return false;}
      unsigned extra=0,d;
      if(!b.get(lengthExtra[v-257],extra)||!dist.decode(b,d)||d>=30){error="invalid length or distance symbol";return false;}
      size_t len=lengthBase[v-257]+extra;
      if(!b.get(distExtra[d],extra)){error="truncated distance";return false;}
      size_t back=distBase[d]+extra;
      if(!back||back>out.size()||len>limit-out.size()){error="distance outside output or output limit exceeded";return false;}
      for(size_t k=0;k<len;++k)out.push_back(out[out.size()-back]);
    }
  } while(!final);
  if ((b.bit+7)/8 != in.size()) {error="trailing deflate data";return false;}
  return true;
}
bool zlibInflate(const std::vector<uint8_t>& in,std::vector<uint8_t>& out,size_t limit,std::string& error) {
  if(in.size()<6){error="truncated zlib stream";return false;}
  if((in[0]&15)!=8 || (in[0]>>4)>7 || ((unsigned(in[0])<<8)|in[1])%31 || (in[1]&32)) {error="invalid zlib header";return false;}
  std::vector<uint8_t> raw(in.begin()+2,in.end()-4);
  if(!inflateRaw(raw,out,limit,error))return false;
  uint32_t expected=(uint32_t(in[in.size()-4])<<24)|(uint32_t(in[in.size()-3])<<16)|(uint32_t(in[in.size()-2])<<8)|in.back();
  if(adler32(out.data(),out.size())!=expected){error="zlib Adler-32 mismatch";out.clear();return false;}
  return true;
}
std::vector<uint8_t> zlibDeflateStored(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> out{0x78,0x01};
  size_t pos=0;
  do {
    size_t n=std::min<size_t>(65535,in.size()-pos);
    out.push_back(pos+n==in.size()?1:0);
    out.push_back(uint8_t(n));out.push_back(uint8_t(n>>8));
    out.push_back(uint8_t(~n));out.push_back(uint8_t(~n>>8));
    out.insert(out.end(),in.begin()+pos,in.begin()+pos+n);pos+=n;
  }while(pos<in.size());
  uint32_t a=adler32(in.data(),in.size());
  for(int i=3;i>=0;--i)out.push_back(uint8_t(a>>(i*8)));
  return out;
}
}
