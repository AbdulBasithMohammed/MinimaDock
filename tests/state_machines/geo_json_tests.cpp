#include "taskbar/geo_json.h"
#include <iostream>
#include <stdexcept>
using namespace minima;
namespace {
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool near(double a,double b){return std::abs(a-b)<1e-9;}
// Synthetic answers shaped like Nominatim's jsonv2 (made-up places and coordinates).
constexpr std::string_view search=R"([
  {"place_id":1,"lat":"30.1234","lon":"70.5678","name":"Testville","display_name":"Testville, North Region, Exampleland",
   "address":{"town":"Testville","state":"North Region","country":"Exampleland","country_code":"XL"}},
  {"place_id":2,"lat":"-12.5","lon":"-45.25","display_name":"Écoville-sur-Mer, Province, País",
   "address":{"city":"Écoville-sur-Mer","state":"Province","country":"País","country_code":"xp"}},
  {"place_id":3,"lat":"10","lon":"20","display_name":"مثال, Region",
   "address":{"village":"مثال","country_code":"xa","extra":[1,2.5e3,true,false,null,{"deep":["🌙"]}]}},
  {"place_id":4,"lat":"95","lon":"20","display_name":"Out of range"},
  {"place_id":5,"lat":"abc","lon":"20","display_name":"Not a number"}
])";
constexpr std::string_view reverse=R"({"lat":"1.5","lon":"2.5","display_name":"Hamlet, County, Country",
  "address":{"county":"County","country":"Country","country_code":"xc"}})";
}
int main() {
    try {
        // Search answers: names from address (town/city/village), escapes and raw UTF-8.
        const auto places=parse_places(search);
        expect(places&&places->size()==3,"Search: three usable places (out-of-range and non-numeric skipped)");
        const auto& p=*places;
        expect(near(p[0].latitude,30.1234)&&near(p[0].longitude,70.5678),"Coordinates from strings");
        expect(p[0].name==L"Testville"&&p[0].region==L"North Region"&&p[0].country==L"Exampleland"&&p[0].country_code=="xl","Town, region, country, lower-case code");
        expect(p[0].label()==L"Testville, North Region, Exampleland","Label");
        expect(p[1].name==L"Écoville-sur-Mer"&&p[1].country==L"País","\\u escapes decoded to UTF-16");
        expect(p[2].name==L"مثال"&&p[2].country.empty(),"Raw UTF-8 Arabic decoded");
        expect(p[2].label()==L"مثال","Label skips empty parts");
        // Reverse answer: one object, county as the name.
        const auto one=parse_places(reverse);
        expect(one&&one->size()==1&&(*one)[0].name==L"County"&&(*one)[0].label()==L"County, Country","Reverse: single object, repeated parts dropped");
        // Nothing found, and things that are not JSON at all.
        const auto none=parse_places(R"({"error":"Unable to geocode"})");expect(none&&none->empty(),"Error object: no places");
        expect(parse_places("[]")&&parse_places("[]")->empty(),"Empty array");
        expect(!parse_places("<html><body>Sign in to the Wi-Fi</body></html>"),"Captive portal HTML is not JSON");
        expect(!parse_places("")&&!parse_places("[{\"lat\":\"1\"")&&!parse_places("[1,]")&&!parse_places("{} trailing"),"Truncated or malformed JSON");
        expect(!parse_places(std::string(100,'[')+std::string(100,']')),"Nesting limit");
        // JSON details.
        const auto v=parse_json(R"({"s":"a\"b\\c\/d\n","bad":"\ud800x","n":-1.5e2})");
        expect(v&&v->get("s")->text=="a\"b\\c/d\n"&&v->get("n")->number()==-150.0,"Escapes and numbers");
        expect(v->get("bad")->text=="\xEF\xBF\xBDx","Lone surrogate becomes U+FFFD");
        expect(utf8_to_wide("\xF0\x9F\x8C\x99")==L"\U0001F319"&&utf8_to_wide("a\xC3")==L"a�"&&utf8_to_wide("\xC0\xAF")==L"��","UTF-8 decoding, truncated and overlong");
        // Query encoding.
        expect(percent_encode(L"New Town 2")==L"New%20Town%202","Spaces and digits");
        expect(percent_encode(L"Éco & co/")==L"%C3%89co%20%26%20co%2F","UTF-8 bytes and reserved characters");
        // Coordinates typed into the city field.
        const auto c=parse_coordinates(L" 30.5, -70.25 ");expect(c&&near(c->latitude,30.5)&&near(c->longitude,-70.25),"lat, lon");
        expect(parse_coordinates(L"-12 45")&&!parse_coordinates(L"91, 10")&&!parse_coordinates(L"10, 181"),"Space separated; out of range");
        expect(!parse_coordinates(L"Testville")&&!parse_coordinates(L"30.5")&&!parse_coordinates(L"1, 2, 3")&&!parse_coordinates(L"30.5,"),"Names and partial input are searched instead");
        // Distance.
        expect(std::abs(distance_km(0,0,0,1)-111.19)<0.1&&distance_km(30,70,30,70)==0,"Haversine");
        expect(distance_km(30,70,30.03,70)>3&&distance_km(30,70,30.03,70)<4,"Small move under 5 km");
        std::cout<<"Geo JSON passed: search and reverse answers, non-ASCII names, error and HTML bodies, malformed JSON, query encoding, typed coordinates, distance.\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
