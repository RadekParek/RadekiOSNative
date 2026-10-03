#include "ipa/bundle.h"
#include "ipa/plist.h"
#include "ipa/png.h"
#include "mach_o/macho.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
namespace radeki::ipa {
namespace fs=std::filesystem;
namespace {
std::vector<uint8_t> read(const fs::path& path,size_t max){
  std::error_code ec;uintmax_t size=fs::file_size(path,ec);
  if(ec||size>max)return {};
  std::ifstream f(path,std::ios::binary);if(!f)return {};
  return {std::istreambuf_iterator<char>(f),{}};
}
std::string lower(std::string s){
  std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});
  return s;
}
std::string val(const Plist& p,const std::string& name){
  auto* v=p.find(name);if(!v)return "";
  if(v->type==Plist::Type::String)return v->text;
  if(v->type==Plist::Type::Integer)return std::to_string(v->integer);
  return "";
}
void named(const Plist* p,std::vector<std::string>& dest){
  if(!p)return;
  if(p->type==Plist::Type::String)dest.push_back(p->text);
  else if(p->type==Plist::Type::Array){auto a=p->stringsOr();dest.insert(dest.end(),a.begin(),a.end());}
}
void primary(const Plist* p,std::vector<std::string>& dest){
  if(!p)return;
  p=p->find("CFBundlePrimaryIcon");if(!p)return;
  named(p->find("CFBundleIconFiles"),dest);named(p->find("CFBundleIconName"),dest);
}
fs::path findChildCaseInsensitive(const fs::path& root,const std::string& wanted){
  fs::path direct=root/wanted;
  std::error_code ec;
  if(fs::is_regular_file(direct,ec))return direct;
  std::string w=lower(wanted);
  for(const auto& entry:fs::directory_iterator(root,ec)){
    if(entry.is_regular_file(ec) && lower(entry.path().filename().string())==w)return entry.path();
  }
  return direct;
}
struct Candidate {RgbaImage image;std::string file;};
}
bool inspectBundle(const std::string& appPath,const std::string& iconOutPath,BundleInfo& out,std::string& error){
  out={};error.clear();
  try {
    fs::path root=fs::weakly_canonical(fs::path(appPath).lexically_normal());
    if(root.filename().empty())root=root.parent_path();
    fs::path plistPath=findChildCaseInsensitive(root,"Info.plist");
    if(!fs::is_directory(root) || (lower(root.extension().string())!=".app" && !fs::is_regular_file(plistPath))){
      error="bundle path is not an .app directory";return false;
    }
    auto bytes=read(plistPath,16*1024*1024);
    Plist info;
    if(bytes.empty()||!parsePlist(bytes,info,error)||info.type!=Plist::Type::Dict){if(error.empty())error="missing or invalid Info.plist";return false;}
    out.name=val(info,"CFBundleDisplayName");if(out.name.empty())out.name=val(info,"CFBundleName");if(out.name.empty())out.name=root.stem().string();
    out.bundleIdentifier=val(info,"CFBundleIdentifier");out.executable=val(info,"CFBundleExecutable");
    if(out.executable.empty())out.executable=out.name;
    if(out.bundleIdentifier.empty() && !out.name.empty())out.bundleIdentifier="bundle."+out.name;
    out.version=val(info,"CFBundleVersion");out.shortVersion=val(info,"CFBundleShortVersionString");
    if(out.version.empty())out.version=out.shortVersion.empty()?"0":out.shortVersion;
    out.minimumOSVersion=val(info,"MinimumOSVersion");out.platform=val(info,"DTPlatformName");
    if(out.executable.empty() || fs::path(out.executable).filename()!=out.executable){error="bundle has no safe CFBundleExecutable";return false;}
    fs::path execCandidate=findChildCaseInsensitive(root,out.executable);
    fs::path executable=fs::weakly_canonical(execCandidate);
    if(executable.parent_path()!=root || !fs::is_regular_file(executable)){error="bundle executable missing or outside app";return false;}
    out.executablePath=executable.string();out.executableSize=fs::file_size(executable);
    auto macho=read(executable,512*1024*1024);
    if(macho.empty()){out.warnings.push_back("executable could not be read for architecture detection");}
    else {
      try {auto slices=macho::listSlices(macho);for(const auto& slice:slices)out.architectures.emplace_back(macho::archName(slice.arch));}
      catch(const std::exception& e){out.warnings.push_back(std::string("architecture detection: ")+e.what());}
    }
    std::vector<std::vector<std::string>> groups(4);
    primary(info.find("CFBundleIcons~ipad"),groups[0]);
    primary(info.find("CFBundleIcons"),groups[1]);
    named(info.find("CFBundleIconFiles"),groups[2]);
    named(info.find("CFBundleIconFile"),groups[3]);
    auto accept=[](const std::vector<uint8_t>& data,bool requireCgbi)->std::optional<RgbaImage>{
      PngInfo meta;std::string why;RgbaImage image;
      if(!pngInfo(data,meta,why)|| (requireCgbi && !meta.cgbi) || !decodePng(data,image,why))return std::nullopt;
      return image;
    };
    std::optional<Candidate> best;
    auto offer=[&](const fs::path& file,bool cgbi){
      auto data=read(file,32*1024*1024);
      if(data.empty())return;
      auto image=accept(data,cgbi);if(!image)return;
      if(!best || (image->width==image->height && best->image.width!=best->image.height) ||
        ((image->width==image->height)==(best->image.width==best->image.height) &&
         uint64_t(image->width)*image->height>uint64_t(best->image.width)*best->image.height))
        best=Candidate{std::move(*image),file.string()};
    };
    std::set<std::string> seen;
    auto safeIcon = [&](const fs::path& path) {
      std::error_code ec;
      return fs::is_regular_file(path,ec) && fs::weakly_canonical(path,ec).parent_path()==root;
    };
    for(const auto& group:groups){
      for(const auto& n:group){
        if(n.empty()||fs::path(n).filename()!=n)continue;
        std::vector<std::string> variants={n};
        const auto ext=lower(fs::path(n).extension().string());
        const std::string stem=ext==".png"?fs::path(n).stem().string():ext.empty()?n:"";
        if(ext.empty())variants.push_back(n+".png");
        if(!stem.empty()){
          for(const char* suffix:{"@2x.png","@3x.png","~ipad.png","@2x~ipad.png",
              "60x60@2x.png","60x60@3x.png","76x76~ipad.png","76x76@2x~ipad.png","83.5x83.5@2x~ipad.png"})
            variants.push_back(stem+suffix);
        }
        for(const auto& v:variants)if(seen.insert(v).second && safeIcon(root/v))offer(root/v,false);
      }
      // Declared groups are ordered; compare all renditions within the winning group.
      if(best)break;
    }
    if(!best){
      std::vector<fs::path> loose;
      for(auto& entry:fs::directory_iterator(root))if(entry.is_regular_file()){
        std::string filename=lower(entry.path().filename().string());
        if((filename.rfind("icon",0)==0||filename.rfind("appicon",0)==0) &&
           filename.size()>=4 && filename.substr(filename.size()-4)==".png" && safeIcon(entry.path()))
          loose.push_back(entry.path());
      }
      std::sort(loose.begin(),loose.end());for(const auto& p:loose)offer(p,false);
    }
    if(!best && fs::is_regular_file(root/"Assets.car")){
      auto asset=read(root/"Assets.car",128*1024*1024);
      static const uint8_t signature[]={137,80,78,71,13,10,26,10};
      for(size_t i=0;i+8<=asset.size();++i){
        if(!std::equal(std::begin(signature),std::end(signature),asset.begin()+i))continue;
        size_t pos=i+8;
        while(pos+12<=asset.size()){
          uint32_t n=(uint32_t(asset[pos])<<24)|(uint32_t(asset[pos+1])<<16)|(uint32_t(asset[pos+2])<<8)|asset[pos+3];
          if(n>asset.size()-pos-12)break;
          bool end=std::equal(asset.begin()+pos+4,asset.begin()+pos+8,"IEND");pos+=n+12;
          if(end){std::vector<uint8_t> png(asset.begin()+i,asset.begin()+pos);auto image=accept(png,true);
            if(image && (!best || (image->width==image->height && best->image.width!=best->image.height) ||
              ((image->width==image->height)==(best->image.width==best->image.height) && uint64_t(image->width)*image->height>uint64_t(best->image.width)*best->image.height)))
              best=Candidate{std::move(*image),(root/"Assets.car").string()};
            break;
          }
        }
      }
    }
    if(best){
      auto png=encodePng(best->image);if(png.empty()){error="cannot encode bundle icon";return false;}
      fs::path path=iconOutPath;if(path.has_parent_path())fs::create_directories(path.parent_path());
      std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(png.data()),png.size());
      if(!f){error="cannot write extracted icon";return false;}out.iconPath=path.string();
    }else out.warnings.push_back("no icon found in imported bundle");
    return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}
