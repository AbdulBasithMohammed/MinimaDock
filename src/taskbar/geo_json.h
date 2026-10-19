#pragma once
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Reading Nominatim (OpenStreetMap) search and reverse answers, plus the small
// text helpers around them. Header-only and free of Windows calls so it is
// unit-tested (tests/state_machines/geo_json_tests.cpp).
namespace minima {
// A JSON value: just enough for Nominatim's answers. Strings are UTF-8.
struct JsonValue {
    enum class Kind {Null,Bool,Number,String,Array,Object} kind=Kind::Null;
    std::string text;              // String contents, or a Number's literal.
    bool boolean=false;
    std::vector<std::string> keys; // Object member names (parallel to values).
    std::vector<JsonValue> values; // Array items or Object member values.
    const JsonValue* get(std::string_view key) const {
        if(kind!=Kind::Object)return nullptr;
        for(std::size_t i=0;i<keys.size();++i)if(keys[i]==key)return &values[i];
        return nullptr;
    }
    // Numbers, and numbers sent as strings (Nominatim sends "lat":"12.34").
    std::optional<double> number() const {
        if(kind!=Kind::Number&&kind!=Kind::String)return std::nullopt;
        double value=0;const auto* end=text.data()+text.size();
        const auto [at,error]=std::from_chars(text.data(),end,value);
        if(error!=std::errc{}||at!=end||!std::isfinite(value))return std::nullopt;
        return value;
    }
};
namespace json_detail {
inline void append_utf8(std::string& out,char32_t c) {
    if(c<0x80)out+=static_cast<char>(c);
    else if(c<0x800){out+=static_cast<char>(0xC0|(c>>6));out+=static_cast<char>(0x80|(c&0x3F));}
    else if(c<0x10000){out+=static_cast<char>(0xE0|(c>>12));out+=static_cast<char>(0x80|((c>>6)&0x3F));out+=static_cast<char>(0x80|(c&0x3F));}
    else {out+=static_cast<char>(0xF0|(c>>18));out+=static_cast<char>(0x80|((c>>12)&0x3F));out+=static_cast<char>(0x80|((c>>6)&0x3F));out+=static_cast<char>(0x80|(c&0x3F));}
}
struct Parser {
    std::string_view s;std::size_t i=0;
    void space(){while(i<s.size()&&(s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'))++i;}
    bool literal(std::string_view word){if(s.substr(i,word.size())!=word)return false;i+=word.size();return true;}
    int hex4() {
        if(i+4>s.size())return -1;int value=0;
        for(int k=0;k<4;++k){const char c=s[i++];value<<=4;
            if(c>='0'&&c<='9')value|=c-'0';else if(c>='a'&&c<='f')value|=c-'a'+10;else if(c>='A'&&c<='F')value|=c-'A'+10;else return -1;}
        return value;
    }
    bool string(std::string& out) {
        if(i>=s.size()||s[i]!='"')return false;++i;
        while(i<s.size()) {
            const char c=s[i++];
            if(c=='"')return true;
            if(static_cast<unsigned char>(c)<0x20)return false;
            if(c!='\\'){out+=c;continue;}
            if(i>=s.size())return false;
            switch(s[i++]) {
            case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u': {
                const int high=hex4();if(high<0)return false;char32_t code=static_cast<char32_t>(high);
                if(high>=0xD800&&high<=0xDBFF) { // A surrogate pair spells one character.
                    const auto back=i;int low=-1;
                    if(literal("\\u"))low=hex4();
                    if(low>=0xDC00&&low<=0xDFFF)code=0x10000+((static_cast<char32_t>(high)-0xD800)<<10)+(static_cast<char32_t>(low)-0xDC00);
                    else {i=back;code=0xFFFD;}
                } else if(high>=0xDC00&&high<=0xDFFF)code=0xFFFD;
                append_utf8(out,code);break;
            }
            default:return false;
            }
        }
        return false;
    }
    bool value(JsonValue& out,int depth) {
        if(depth>32)return false; // Nominatim nests 3 deep; refuse anything absurd.
        space();if(i>=s.size())return false;
        const char c=s[i];
        if(c=='{') {
            ++i;out.kind=JsonValue::Kind::Object;space();
            if(i<s.size()&&s[i]=='}'){++i;return true;}
            for(;;) {
                space();std::string key;if(!string(key))return false;
                space();if(i>=s.size()||s[i]!=':')return false;++i;
                JsonValue member;if(!value(member,depth+1))return false;
                out.keys.push_back(std::move(key));out.values.push_back(std::move(member));
                space();if(i>=s.size())return false;
                if(s[i]==','){++i;continue;}
                if(s[i]=='}'){++i;return true;}
                return false;
            }
        }
        if(c=='[') {
            ++i;out.kind=JsonValue::Kind::Array;space();
            if(i<s.size()&&s[i]==']'){++i;return true;}
            for(;;) {
                JsonValue item;if(!value(item,depth+1))return false;out.values.push_back(std::move(item));
                space();if(i>=s.size())return false;
                if(s[i]==','){++i;continue;}
                if(s[i]==']'){++i;return true;}
                return false;
            }
        }
        if(c=='"'){out.kind=JsonValue::Kind::String;return string(out.text);}
        if(literal("true")){out.kind=JsonValue::Kind::Bool;out.boolean=true;return true;}
        if(literal("false")){out.kind=JsonValue::Kind::Bool;return true;}
        if(literal("null"))return true;
        const auto start=i;
        while(i<s.size()&&(s[i]=='-'||s[i]=='+'||s[i]=='.'||s[i]=='e'||s[i]=='E'||(s[i]>='0'&&s[i]<='9')))++i;
        if(i==start)return false;
        out.kind=JsonValue::Kind::Number;out.text=std::string(s.substr(start,i-start));return out.number().has_value();
    }
};
}
// The whole text must be one JSON value (an HTML error page is not).
inline std::optional<JsonValue> parse_json(std::string_view text) {
    if(text.substr(0,3)=="\xEF\xBB\xBF")text.remove_prefix(3);
    json_detail::Parser parser{text};JsonValue value;
    if(!parser.value(value,0))return std::nullopt;
    parser.space();if(parser.i!=text.size())return std::nullopt;
    return value;
}
// UTF-8 to UTF-16 by hand; malformed bytes become U+FFFD.
inline std::wstring utf8_to_wide(std::string_view text) {
    std::wstring out;out.reserve(text.size());
    for(std::size_t i=0;i<text.size();) {
        const auto b=static_cast<unsigned char>(text[i]);
        int extra=b<0x80?0:(b>>5)==0x6?1:(b>>4)==0xE?2:(b>>3)==0x1E?3:-1;
        char32_t code=extra==0?b:extra==1?(b&0x1F):extra==2?(b&0x0F):(b&0x07);
        bool good=extra>=0&&i+static_cast<std::size_t>(extra)<text.size();
        for(int k=1;good&&k<=extra;++k) {
            const auto next=static_cast<unsigned char>(text[i+static_cast<std::size_t>(k)]);
            if((next&0xC0)!=0x80)good=false;else code=(code<<6)|(next&0x3F);
        }
        // Reject overlong forms, surrogates and values past U+10FFFF.
        if(good&&((extra==1&&code<0x80)||(extra==2&&code<0x800)||(extra==3&&code<0x10000)||code>0x10FFFF||(code>=0xD800&&code<=0xDFFF)))good=false;
        if(!good){out+=static_cast<wchar_t>(0xFFFD);++i;continue;}
        i+=static_cast<std::size_t>(extra)+1;
        if(code>=0x10000){code-=0x10000;out+=static_cast<wchar_t>(0xD800+(code>>10));out+=static_cast<wchar_t>(0xDC00+(code&0x3FF));}
        else out+=static_cast<wchar_t>(code);
    }
    return out;
}
inline std::string wide_to_utf8(std::wstring_view text) {
    std::string out;out.reserve(text.size());
    for(std::size_t i=0;i<text.size();++i) {
        char32_t code=static_cast<char32_t>(text[i]);
        if(code>=0xD800&&code<=0xDBFF&&i+1<text.size()&&text[i+1]>=0xDC00&&text[i+1]<=0xDFFF){code=0x10000+((code-0xD800)<<10)+(static_cast<char32_t>(text[i+1])-0xDC00);++i;}
        else if(code>=0xD800&&code<=0xDFFF)code=0xFFFD;
        json_detail::append_utf8(out,code);
    }
    return out;
}
// A search query for a URL: UTF-8, everything but unreserved characters escaped.
inline std::wstring percent_encode(std::wstring_view text) {
    constexpr char digits[]="0123456789ABCDEF";std::wstring out;
    for(const char c:wide_to_utf8(text)) {
        const auto b=static_cast<unsigned char>(c);
        if((b>='A'&&b<='Z')||(b>='a'&&b<='z')||(b>='0'&&b<='9')||b=='-'||b=='.'||b=='_'||b=='~')out+=static_cast<wchar_t>(b);
        else {out+=L'%';out+=static_cast<wchar_t>(digits[b>>4]);out+=static_cast<wchar_t>(digits[b&15]);}
    }
    return out;
}
struct Coordinates {double latitude=0,longitude=0;};
inline bool valid_coordinates(double latitude,double longitude) {
    return std::isfinite(latitude)&&std::isfinite(longitude)&&std::abs(latitude)<=90&&std::abs(longitude)<=180;
}
// "12.34, -56.78" or "12.34 -56.78" typed into the city field: used as-is,
// with no network call. Anything else is a place name to search.
inline std::optional<Coordinates> parse_coordinates(std::wstring_view text) {
    std::string ascii;
    for(const wchar_t c:text){if(c>127)return std::nullopt;ascii+=static_cast<char>(c==','?' ':c);}
    double values[2]{};std::size_t i=0;
    for(double& value:values) {
        while(i<ascii.size()&&ascii[i]==' ')++i;
        const auto [end,error]=std::from_chars(ascii.data()+i,ascii.data()+ascii.size(),value);
        if(error!=std::errc{})return std::nullopt;
        i=static_cast<std::size_t>(end-ascii.data());
    }
    while(i<ascii.size()&&ascii[i]==' ')++i;
    if(i!=ascii.size()||!valid_coordinates(values[0],values[1]))return std::nullopt;
    return Coordinates{values[0],values[1]};
}
// Great-circle distance, used to skip the reverse lookup for small moves.
inline double distance_km(double lat1,double lon1,double lat2,double lon2) {
    constexpr double radians=3.14159265358979323846/180,earth=6371.0;
    const double dlat=(lat2-lat1)*radians,dlon=(lon2-lon1)*radians;
    const double a=std::sin(dlat/2)*std::sin(dlat/2)+std::cos(lat1*radians)*std::cos(lat2*radians)*std::sin(dlon/2)*std::sin(dlon/2);
    return 2*earth*std::asin(std::sqrt(std::min(1.0,a)));
}
struct Place {
    double latitude=0,longitude=0;
    std::wstring name,region,country; // "Town", "State", "Country" (any may be empty).
    std::string country_code;         // Lower-case ISO 3166-1, "" when unknown.
    // "Town, State, Country" without empty or repeated parts.
    std::wstring label() const {
        std::wstring out;
        for(const auto* part:{&name,&region,&country}) {
            if(part->empty()||(part!=&name&&(*part==name||(part==&country&&*part==region))))continue;
            if(!out.empty())out+=L", ";
            out+=*part;
        }
        return out;
    }
};
// Nominatim's jsonv2 answer: an array (search), one object (reverse) or
// {"error":...} (nothing there). nullopt when the body isn't JSON at all, as
// with a captive portal's HTML page.
inline std::optional<std::vector<Place>> parse_places(std::string_view body) {
    const auto root=parse_json(body);if(!root)return std::nullopt;
    std::vector<Place> places;
    const auto read=[&](const JsonValue& item) {
        if(item.kind!=JsonValue::Kind::Object||item.get("error"))return;
        const auto* lat=item.get("lat");const auto* lon=item.get("lon");
        const auto latitude=lat?lat->number():std::nullopt,longitude=lon?lon->number():std::nullopt;
        if(!latitude||!longitude||!valid_coordinates(*latitude,*longitude))return;
        Place place;place.latitude=*latitude;place.longitude=*longitude;
        const auto text=[](const JsonValue* v)->std::string{return v&&v->kind==JsonValue::Kind::String?v->text:std::string();};
        if(const auto* address=item.get("address")) {
            for(const char* key:{"city","town","village","municipality","county"})if(place.name.empty())place.name=utf8_to_wide(text(address->get(key)));
            place.region=utf8_to_wide(text(address->get("state")));
            place.country=utf8_to_wide(text(address->get("country")));
            for(const char c:text(address->get("country_code")))if(place.country_code.size()<2)place.country_code+=static_cast<char>(c>='A'&&c<='Z'?c+32:c);
            if(place.country_code.size()!=2)place.country_code.clear();
        }
        if(place.name.empty())place.name=utf8_to_wide(text(item.get("name")));
        if(place.name.empty()) {
            const auto display=text(item.get("display_name"));
            place.name=utf8_to_wide(display.substr(0,display.find(',')));
        }
        places.push_back(std::move(place));
    };
    if(root->kind==JsonValue::Kind::Array)for(const auto& item:root->values)read(item);
    else if(root->kind==JsonValue::Kind::Object)read(*root);
    else return std::nullopt;
    return places;
}
}
