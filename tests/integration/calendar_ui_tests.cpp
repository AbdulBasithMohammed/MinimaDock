// Verify that actual calendar pixels translate vertically, preserving columns.
#include "../../src/rendering/calendar.cpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace minima;
int main(){const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(apartment))return 1;int result=0;
try {
    CalendarPainter painter;painter.initialize();painter.model.reset({2026,10,11});
    ComPtr<IWICBitmap> bitmap;require(painter.wic->CreateBitmap(360,448,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));
    ComPtr<ID2D1RenderTarget> target;require(painter.factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(),target.put()));painter.bind(target.get());
    const auto grid=[&](float offset){target->BeginDraw();target->Clear(D2D1::ColorF(0,0,0,0));painter.draw_grid(2026,10,offset,1,true);require(target->EndDraw());std::vector<BYTE> data(360*448*4);require(bitmap->CopyPixels(nullptr,1440,static_cast<UINT>(data.size()),data.data()));return data;};
    const auto original=grid(0),down=grid(42);bool visible=false;
    for(size_t y=124;y<396;++y)for(size_t x=20*4;x<340*4;++x){const auto a=original[y*1440+x],b=down[(y+42)*1440+x];visible=visible||a!=0;if(a!=b)throw std::runtime_error("Month scroll changed columns instead of translating rows vertically");}
    if(!visible)throw std::runtime_error("Calendar pixel test rendered an empty grid");
    painter.draw(.98f);require(target->EndDraw());painter.draw();require(target->EndDraw());
    std::cout<<"Calendar UI passed: actual day glyphs and selected-day circle translate vertically with fixed columns; scaled flyout renders successfully.\n";
}catch(HRESULT hr){std::cerr<<"Calendar UI HRESULT="<<std::hex<<static_cast<unsigned long>(hr)<<'\n';result=1;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}CoUninitialize();return result;}
