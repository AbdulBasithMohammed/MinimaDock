#include "taskbar/launcher_model.h"
#include <iostream>
#include <stdexcept>
using namespace minima::launcher;
namespace {
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::wstring value(std::wstring_view q){const auto a=answer(q,2026,20740);return a?a->value:L"<none>";} // 2026-10-14
}
int main() {
    try {
        // Matching tiers.
        expect(match(L"Brave",L"bra")>match(L"Visual Studio Code",L"code"),"Prefix beats word start");
        expect(match(L"Visual Studio Code",L"code")>=80,"Word start");
        expect(match(L"Visual Studio Code",L"vsc")>=75,"Initials");
        expect(match(L"Windows Terminal",L"wtrm")>0,"Letters in order");
        expect(match(L"On-Screen Keyboard",L"read")<0&&match(L"SA Communication Presentation Prompt.docx",L"read")<0,"Scattered letters are not a match");
        expect(match(L"Steam",L"stema")>0&&match(L"Discord",L"disocrd")>0,"One typo (incl. a swap) is forgiven");
        expect(match(L"Steam",L"xyz")<0&&match(L"Steam",L"")<0,"No match");
        expect(match(L"Turn Wi-Fi off",L"wifi off")<0||match(L"turn wi fi off",L"wi off")>0,"Multi-word queries match word starts");
        expect(match(L"Bluetooth & devices",L"blue dev")>=78,"Each query word starts a word");
        expect(match(L"BRAVE",L"bRa")==match(L"brave",L"BRA"),"Case-insensitive");
        auto marks=highlight(L"Visual Studio Code",L"code");expect(marks[14]&&marks[17]&&!marks[0],"Highlight the matched run");
        marks=highlight(L"Windows Terminal",L"wtrm");expect(marks[0]&&marks[8],"Highlight letters in order");
        // Ranking.
        expect(frecency(50,0)>frecency(5,0)&&frecency(5,0)>frecency(5,60)&&frecency(0,0)==0,"Frequent and recent rank higher");
        // Answers.
        expect(value(L"12*7+3")==L"87"&&value(L"2^10")==L"1024"&&value(L"(1+2)*3")==L"9"&&value(L"-4+10")==L"6","Arithmetic");
        expect(value(L"10/4")==L"2.5"&&value(L"1/3")==L"0.333333","Decimals");
        expect(value(L"50%*8")==L"4"&&value(L"15% of 80")==L"12","Percentages");
        expect(value(L"5 km in mi")==L"3.106856 mi"&&value(L"1 lb to g")==L"453.59237 g","Units");
        expect(value(L"100 c to f")==L"212 \x00B0""F"&&value(L"32 F in C")==L"0 \x00B0""C","Temperature");
        expect(value(L"5 kg in km")==L"<none>","Units of different kinds don't convert");
        expect(value(L"days until dec 25")==L"72 days"&&value(L"days until oct 15")==L"1 day"&&value(L"days until jan 1")==L"79 days","Days until (next occurrence)");
        expect(value(L"1/0")==L"<none>"&&value(L"brave")==L"<none>"&&value(L"42")==L"<none>"&&value(L"2+")==L"<none>","Not answers");
        std::cout<<"Launcher model passed: prefix/word/initials/letters/typo matching, highlights, frecency, arithmetic, percentages, units, temperatures, days until.\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
