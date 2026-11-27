// PC CPU simulator: the packs' files from a folder (--root / KINOKO_FILESYSTEM_ROOT),
// extracted byte for byte from the game's packs (tools/cpu_sim/sweep.py --extract).
#include <mkw/asset_pack.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
namespace mkw::pack {
bool read(const char* path,std::vector<uint8_t>& out){
    const char* root=std::getenv("KINOKO_FILESYSTEM_ROOT");
    std::string full=std::string(root?root:".")+"/"+path;
    std::FILE* f=std::fopen(full.c_str(),"rb");if(!f)return false;
    std::fseek(f,0,SEEK_END);const long n=std::ftell(f);std::fseek(f,0,SEEK_SET);
    out.resize(size_t(n>0?n:0));const bool ok=std::fread(out.data(),1,out.size(),f)==out.size();std::fclose(f);
    return ok;
}
}
