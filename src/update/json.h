#pragma once
#include <charconv>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ttplayer::update::detail {
// Bounded JSON reader: only the release fields selected by the caller have
// meaning. Unknown fields are parsed and ignored, never interpreted as code.
struct Json {
    enum Kind { null, boolean, number, string, array, object } kind{null};
    std::string text;
    std::vector<Json> items;
    std::map<std::string,Json,std::less<>> fields;
    const Json& operator[](std::string_view key) const {
        static const Json empty;
        const auto it=fields.find(key); return it==fields.end() ? empty : it->second;
    }
    std::string String() const { return kind==string ? text : std::string{}; }
    bool True() const { return kind==boolean && text=="true"; }
    uint64_t Integer() const {
        uint64_t n{};
        if(kind!=number) return 0;
        const auto r=std::from_chars(text.data(),text.data()+text.size(),n);
        if(r.ec!=std::errc{} || r.ptr!=text.data()+text.size()) throw std::runtime_error("Invalid JSON integer");
        return n;
    }
};
class JsonReader {
    std::string_view input;
    size_t at{},nodes{};
    [[noreturn]] void Fail() const { throw std::runtime_error("Invalid release JSON"); }
    void Space() { while(at<input.size() && (input[at]==' ' || input[at]=='\r' || input[at]=='\n' || input[at]=='\t')) ++at; }
    bool Eat(char c) { Space(); if(at<input.size() && input[at]==c) { ++at;return true; } return false; }
    unsigned Hex() {
        unsigned n=0;
        for(int i=0;i<4;++i) {
            if(at==input.size()) Fail();
            const char c=input[at++];
            unsigned d=c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : 16;
            if(d==16) Fail(); n=n*16+d;
        }
        return n;
    }
    static void Utf8(std::string& s,unsigned c) {
        if(c<0x80) s+=char(c);
        else if(c<0x800) { s+=char(0xc0|(c>>6));s+=char(0x80|(c&63)); }
        else if(c<0x10000) { s+=char(0xe0|(c>>12));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63)); }
        else { s+=char(0xf0|(c>>18));s+=char(0x80|((c>>12)&63));s+=char(0x80|((c>>6)&63));s+=char(0x80|(c&63)); }
    }
    std::string String() {
        if(!Eat('"')) Fail(); std::string out;
        while(at<input.size()) {
            const unsigned char c=input[at++];
            if(c=='"') return out;
            if(c<32) Fail();
            if(c!='\\') { out+=char(c);continue; }
            if(at==input.size()) Fail();
            switch(input[at++]) {
            case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u': {
                unsigned cp=Hex();
                if(cp>=0xd800 && cp<=0xdbff) {
                    if(at+2>input.size() || input.substr(at,2)!="\\u") Fail();at+=2;
                    const unsigned low=Hex();if(low<0xdc00 || low>0xdfff) Fail();
                    cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
                } else if(cp>=0xdc00 && cp<=0xdfff) Fail();
                Utf8(out,cp);break;
            }
            default:Fail();
            }
        }
        Fail();
    }
    Json Value(unsigned depth) {
        if(depth>32 || ++nodes>100000) Fail();
        Json out;Space();if(at==input.size()) Fail();
        if(input[at]=='"') {out.kind=Json::string;out.text=String();return out;}
        if(Eat('{')) {
            out.kind=Json::object;if(Eat('}')) return out;
            do { auto key=String();if(!Eat(':')) Fail();
                if(!out.fields.emplace(std::move(key),Value(depth+1)).second) Fail();
            } while(Eat(','));
            if(!Eat('}')) Fail();return out;
        }
        if(Eat('[')) {
            out.kind=Json::array;if(Eat(']')) return out;
            do {out.items.push_back(Value(depth+1));} while(Eat(','));
            if(!Eat(']')) Fail();return out;
        }
        for(const auto word:{std::string_view("null"),std::string_view("true"),std::string_view("false")})
            if(input.substr(at,word.size())==word) {at+=word.size();out.kind=word=="null" ? Json::null : Json::boolean;out.text=word;return out;}
        const auto start=at;if(input[at]=='-') ++at;
        auto digits=[&] {const auto begin=at;while(at<input.size() && input[at]>='0' && input[at]<='9') ++at;if(at==begin) Fail();};
        if(at<input.size() && input[at]=='0') ++at;else digits();
        if(at<input.size() && input[at]=='.') {++at;digits();}
        if(at<input.size() && (input[at]=='e' || input[at]=='E')) {++at;if(at<input.size() && (input[at]=='+' || input[at]=='-')) ++at;digits();}
        out.kind=Json::number;out.text=input.substr(start,at-start);return out;
    }
public:
    explicit JsonReader(std::string_view s):input(s) {if(s.size()>2*1024*1024) Fail();}
    Json Read() {auto out=Value(0);Space();if(at!=input.size()) Fail();return out;}
};
}
