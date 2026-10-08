#include "ttplayer/update/update.h"
#include <windows.h>
#include <wincrypt.h>
#include <fstream>
#include <stdexcept>

namespace ttplayer::update {
// Keep hashing separate from ZIP installation: the player also hashes plugins,
// but only the standalone updater should link its private zlib inflater.
std::string Sha256(const std::filesystem::path& file) {
    HCRYPTPROV provider{};HCRYPTHASH hash{};
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)) throw std::runtime_error("SHA-256 provider unavailable");
    struct Cleanup{HCRYPTPROV p;HCRYPTHASH& h;~Cleanup(){if(h) CryptDestroyHash(h);CryptReleaseContext(p,0);}}cleanup{provider,hash};
    if(!CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)) throw std::runtime_error("Cannot initialize SHA-256");
    std::ifstream in(file,std::ios::binary);if(!in) throw std::runtime_error("Cannot read checksum input");
    unsigned char bytes[65536];
    while(in) {in.read(reinterpret_cast<char*>(bytes),sizeof(bytes));const auto count=in.gcount();
        if(count && !CryptHashData(hash,bytes,static_cast<DWORD>(count),0)) throw std::runtime_error("SHA-256 failed");}
    if(!in.eof()) throw std::runtime_error("Checksum read failed");
    unsigned char result[32]{};DWORD size=sizeof(result);
    if(!CryptGetHashParam(hash,HP_HASHVAL,result,&size,0) || size!=sizeof(result)) throw std::runtime_error("SHA-256 failed");
    constexpr char hex[]="0123456789abcdef";std::string out;
    for(auto c:result){out+=hex[c>>4];out+=hex[c&15];}return out;
}
}
