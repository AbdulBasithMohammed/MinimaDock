#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace minima::launcher {
inline std::wstring lower(std::wstring_view value) {
    std::wstring result(value);
    for(auto& c:result)c=static_cast<wchar_t>(std::towlower(c));
    return result;
}
inline bool separator(wchar_t c){return c==L' '||c==L'-'||c==L'_'||c==L'.'||c==L'\\'||c==L'/'||c==L'&'||c==L'('||c==L')'||c==0x2014;}
// Damerau-Levenshtein distance, for one-typo tolerance.
inline int edit_distance(std::wstring_view a,std::wstring_view b) {
    std::vector<std::vector<int>> d(a.size()+1,std::vector<int>(b.size()+1));
    for(size_t i=0;i<=a.size();++i)d[i][0]=static_cast<int>(i);
    for(size_t j=0;j<=b.size();++j)d[0][j]=static_cast<int>(j);
    for(size_t i=1;i<=a.size();++i)for(size_t j=1;j<=b.size();++j) {
        d[i][j]=std::min({d[i-1][j]+1,d[i][j-1]+1,d[i-1][j-1]+(a[i-1]==b[j-1]?0:1)});
        if(i>1&&j>1&&a[i-1]==b[j-2]&&a[i-2]==b[j-1])d[i][j]=std::min(d[i][j],d[i-2][j-2]+1);
    }
    return d[a.size()][b.size()];
}
// How well `text` matches `query` (case-insensitive); negative = no match.
// Prefix 100 > a word's start 80 > initials 75 ("vsc") > letters in order 50
// > one typo in a word's start 40 (queries of 4+ letters).
inline double match(std::wstring_view text,std::wstring_view query) {
    const auto t=lower(text);auto q=lower(query);
    while(!q.empty()&&q.back()==L' ')q.pop_back();
    while(!q.empty()&&q.front()==L' ')q.erase(q.begin());
    if(q.empty())return -1;
    if(t.rfind(q,0)==0)return 100-std::min<double>(20,static_cast<double>(t.size()-q.size()))*.1;
    std::vector<std::wstring> words;std::wstring word;
    for(const auto c:t){if(separator(c)){if(!word.empty())words.push_back(word);word.clear();}else word+=c;}
    if(!word.empty())words.push_back(word);
    for(const auto& w:words)if(w.rfind(q,0)==0)return 80;
    // A multi-word query matches when each of its words starts some word.
    if(q.find(L' ')!=std::wstring::npos) {
        bool all=true;size_t from=0;
        while(from<q.size()) {
            const auto end=std::min(q.find(L' ',from),q.size());const auto part=q.substr(from,end-from);from=end+1;
            if(part.empty())continue;
            if(std::none_of(words.begin(),words.end(),[&](const std::wstring& w){return w.rfind(part,0)==0;})){all=false;break;}
        }
        if(all)return 78;
    }
    std::wstring initials;for(const auto& w:words)initials+=w[0];
    if(initials.rfind(q,0)==0)return 75;
    // Letters in order, starting at a word's first letter and staying close
    // together ("wtrm" finds Windows Terminal; "read" does not find
    // On-Screen Keyboard).
    for(size_t start=0;start<t.size();++start) {
        if(t[start]!=q[0]||(start>0&&!separator(t[start-1])))continue;
        size_t i=0,end=start;
        for(size_t k=start;k<t.size()&&i<q.size();++k)if(t[k]==q[i]){++i;end=k;}
        if(i==q.size()&&q.size()>=2&&end-start<q.size()*3)return 50-static_cast<double>(end-start+1-q.size())*.5;
    }
    if(q.size()>=4)for(const auto& w:words)if(edit_distance(std::wstring_view(w).substr(0,std::min(w.size(),q.size())),q)<=1)return 40;
    return -1;
}
// Characters of `text` to highlight for `query` (the same rules as match()).
inline std::vector<bool> highlight(std::wstring_view text,std::wstring_view query) {
    std::vector<bool> marks(text.size(),false);
    const auto t=lower(text),q=lower(query);if(q.empty())return marks;
    if(const auto at=t.find(q);at!=std::wstring::npos){for(size_t k=0;k<q.size();++k)marks[at+k]=true;return marks;}
    size_t i=0;for(size_t k=0;k<t.size()&&i<q.size();++k)if(t[k]==q[i]){marks[k]=true;++i;}
    if(i<q.size())std::fill(marks.begin(),marks.end(),false);
    return marks;
}
// Often and recently used results rise: uses decay with a 14-day half-life.
inline double frecency(std::uint32_t uses,double days_since_last) {
    if(!uses)return 0;
    return std::log2(1.0+uses)*3*std::pow(.5,std::max(0.0,days_since_last)/14);
}

// ---- Answers: sums, percentages, units, days until a date ----
namespace detail {
struct Parser {
    std::wstring_view s;size_t i=0;bool ok=true;
    void space(){while(i<s.size()&&s[i]==L' ')++i;}
    double number() {
        space();size_t start=i;bool dot=false;
        while(i<s.size()&&((s[i]>=L'0'&&s[i]<=L'9')||(s[i]==L'.'&&!dot))){if(s[i]==L'.')dot=true;++i;}
        if(start==i){ok=false;return 0;}
        return std::wcstod(std::wstring(s.substr(start,i-start)).c_str(),nullptr);
    }
    double primary() {
        space();
        if(i<s.size()&&s[i]==L'-'){++i;return -primary();}
        if(i<s.size()&&s[i]==L'('){++i;const double v=sum();space();if(i<s.size()&&s[i]==L')')++i;else ok=false;return postfix(v);}
        return postfix(number());
    }
    double postfix(double v){space();if(i<s.size()&&s[i]==L'%'){++i;return v/100;}return v;}
    double power(){const double base=primary();space();if(i<s.size()&&s[i]==L'^'){++i;return std::pow(base,power());}return base;}
    double product() {
        double v=power();
        for(;;){space();if(i<s.size()&&(s[i]==L'*'||s[i]==L'x'||s[i]==0xd7)){++i;v*=power();}else if(i<s.size()&&s[i]==L'/'){++i;const double d=power();if(d==0)ok=false;v/=d;}else return v;}
    }
    double sum(){double v=product();for(;;){space();if(i<s.size()&&s[i]==L'+'){++i;v+=product();}else if(i<s.size()&&s[i]==L'-'){++i;v-=product();}else return v;}}
};
inline std::wstring format(double v) {
    if(!std::isfinite(v))return {};
    const double rounded=std::round(v*1e6)/1e6;
    wchar_t text[64]{};swprintf(text,64,L"%.6f",rounded);
    std::wstring result=text;
    while(!result.empty()&&result.back()==L'0')result.pop_back();
    if(!result.empty()&&result.back()==L'.')result.pop_back();
    if(result==L"-0")result=L"0";
    return result;
}
inline std::optional<double> unit(std::wstring_view u) {
    static const std::pair<const wchar_t*,double> table[]={{L"km",1000},{L"m",1},{L"cm",.01},{L"mm",.001},{L"mi",1609.344},{L"ft",.3048},{L"in",.0254},{L"yd",.9144},
        {L"kg",1000},{L"g",1},{L"lb",453.59237},{L"oz",28.349523125},{L"l",1},{L"ml",.001},{L"gal",3.785411784}};
    for(const auto& [name,factor]:table)if(u==name)return factor;
    return std::nullopt;
}
inline int dimension(std::wstring_view u) {
    if(u==L"km"||u==L"m"||u==L"cm"||u==L"mm"||u==L"mi"||u==L"ft"||u==L"in"||u==L"yd")return 1;
    if(u==L"kg"||u==L"g"||u==L"lb"||u==L"oz")return 2;
    if(u==L"l"||u==L"ml"||u==L"gal")return 3;
    return 0;
}
inline std::vector<std::wstring> words(std::wstring_view text) {
    std::vector<std::wstring> out;std::wstring w;
    for(const auto c:text){if(c==L' '){if(!w.empty())out.push_back(w);w.clear();}else w+=c;}
    if(!w.empty())out.push_back(w);
    return out;
}
}
struct Answer {std::wstring value,detail;};
// `today` is days since 1970-01-01 (local); `year` the current year.
inline std::optional<Answer> answer(std::wstring_view raw,int year,long long today) {
    const auto q=lower(raw);const auto w=detail::words(q);
    if(w.empty())return std::nullopt;
    // "15% of 80"
    if(w.size()==3&&w[1]==L"of"&&!w[0].empty()&&w[0].back()==L'%') {
        detail::Parser a{std::wstring_view(w[0]).substr(0,w[0].size()-1)},b{w[2]};const double x=a.number(),y=b.number();
        if(a.ok&&b.ok&&a.i==a.s.size()&&b.i==b.s.size())return Answer{detail::format(x*y/100),std::wstring(raw)};
    }
    // "5 km in mi", "70 f to c"
    if(w.size()==4&&(w[2]==L"in"||w[2]==L"to")) {
        detail::Parser p{w[0]};const double v=p.number();
        if(p.ok&&p.i==p.s.size()) {
            if(w[1]==L"c"&&w[3]==L"f")return Answer{detail::format(v*9/5+32)+L" \x00B0""F",std::wstring(raw)};
            if(w[1]==L"f"&&w[3]==L"c")return Answer{detail::format((v-32)*5/9)+L" \x00B0""C",std::wstring(raw)};
            const auto a=detail::unit(w[1]),b=detail::unit(w[3]);
            if(a&&b&&detail::dimension(w[1])==detail::dimension(w[3]))return Answer{detail::format(v*(*a)/(*b))+L" "+w[3],std::wstring(raw)};
        }
    }
    // "days until dec 25"
    if(w.size()>=4&&(w[0]==L"days"||w[0]==L"day")&&(w[1]==L"until"||w[1]==L"till"||w[1]==L"to")) {
        static const wchar_t* months[]={L"jan",L"feb",L"mar",L"apr",L"may",L"jun",L"jul",L"aug",L"sep",L"oct",L"nov",L"dec"};
        int month=0;for(int m=0;m<12;++m)if(w[2].rfind(months[m],0)==0)month=m+1;
        detail::Parser p{w[3]};const int day=static_cast<int>(p.number());
        if(month&&p.ok&&day>=1&&day<=31) {
            const auto civil=[](int y,int m,int d)->long long { // days since 1970-01-01
                y-=m<=2;const long long era=(y>=0?y:y-399)/400;const unsigned yoe=static_cast<unsigned>(y-era*400);
                const unsigned doy=(153*(m+(m>2?-3:9))+2)/5+static_cast<unsigned>(d)-1;const unsigned doe=yoe*365+yoe/4-yoe/100+doy;
                return era*146097+static_cast<long long>(doe)-719468;
            };
            long long target=civil(year,month,day);if(target<today)target=civil(year+1,month,day);
            const long long n=target-today;
            return Answer{std::to_wstring(n)+(n==1?L" day":L" days"),std::wstring(raw)};
        }
    }
    // Arithmetic: digits with at least one operator.
    bool digit=false,op=false;
    for(const auto c:q) {
        if(c>=L'0'&&c<=L'9')digit=true;
        else if(c==L'+'||c==L'-'||c==L'*'||c==L'/'||c==L'^'||c==L'%'||c==L'x'||c==0xd7)op=true;
        else if(c!=L' '&&c!=L'.'&&c!=L'('&&c!=L')')return std::nullopt;
    }
    if(!digit||!op)return std::nullopt;
    detail::Parser p{q};const double v=p.sum();p.space();
    if(!p.ok||p.i!=q.size()||!std::isfinite(v))return std::nullopt;
    return Answer{detail::format(v),std::wstring(raw)};
}
}
