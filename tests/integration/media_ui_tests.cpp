// Exercise real DirectWrite measurement, retained text, clipping and hit geometry.
#include "../../src/rendering/preview.cpp"
#include <iostream>
#include <stdexcept>
using namespace minima;
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(apartment))return 1;int result=0;
try {
    Scene scene;scene.initialize();scene.desktop=true;scene.width=1000;scene.height=88;
    scene.machine.reduced_motion(true);scene.machine.hold(Hold::Keyboard,true,0);
    ComPtr<IWICBitmap> bitmap;check(scene.wic->CreateBitmap(1000,88,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));
    ComPtr<ID2D1RenderTarget> target;check(scene.factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(),target.put()));scene.bind(target.get());
    scene.media.available=scene.media.playing=scene.media.toggle=scene.media.next=scene.media.previous=true;
    scene.media.title=L"A very long track title with enough words to scroll smoothly and reveal its last characters";scene.media.artist=L"An artist";
    scene.measure_media_title();expect(scene.media_marquee.distance()>0,"Native title width did not detect overflow");
    const auto retained=scene.media_title.get();const auto mark=scene.launcher_mark.get();const auto pill=scene.dock();const auto area=scene.media_rect();
    scene.media_marquee.visible(true,false,0);
    for(int t=0;t<10000;t+=33){scene.media_marquee.tick(t);scene.draw();check(target->EndDraw());expect(scene.media_title.get()==retained,"Scroll recreated its text layout");expect(scene.launcher_mark.get()==mark,"Scroll recreated dock geometry");expect(scene.dock().top==pill.top&&scene.dock().bottom==pill.bottom,"Scroll changed pill height");}
    const auto pixels=[&](size_t index){const auto r=scene.media_button(index);WICRect rect{static_cast<INT>(r.left),static_cast<INT>(r.top),static_cast<INT>(r.right-r.left),static_cast<INT>(r.bottom-r.top)};ComPtr<IWICBitmapLock> lock;check(bitmap->Lock(&rect,WICBitmapLockRead,lock.put()));UINT bytes=0,stride=0;BYTE* data=nullptr;check(lock->GetDataPointer(&bytes,&data));check(lock->GetStride(&stride));std::vector<BYTE> copy;for(INT y=0;y<rect.Height;++y)copy.insert(copy.end(),data+y*stride,data+y*stride+rect.Width*4);return copy;};
    const auto pause=pixels(1),previous=pixels(0),next=pixels(2);
    scene.media.playing=false;scene.draw();check(target->EndDraw());
    expect(pixels(1)!=pause,"Paused media did not replace pause bars with the play triangle");
    expect(pixels(0)==previous&&pixels(2)==next,"Play/pause switch changed adjacent controls");
    scene.media.playing=true;scene.draw();check(target->EndDraw());expect(pixels(1)==pause,"Playing media did not restore the pause bars");
    for(size_t i=0;i<3;++i){const auto button=scene.media_button(i);expect(button.right-button.left==32,"Media button hit area is too narrow");expect(button.left>=area.left&&button.right<=area.right&&button.bottom<=pill.bottom,"Media button escapes its wing");if(i)expect(scene.media_button(i-1).right<button.left,"Media hit targets overlap");}
    scene.media.artist=L"Another artist";scene.measure_media_title();expect(scene.media_title.get()==retained,"Artist update discarded retained title");
    scene.media.title=L"Short";scene.measure_media_title();expect(scene.media_marquee.distance()==0&&!scene.media_marquee.active(),"A new fitting title kept scrolling");
    scene.media.title=L"A long title which must stay still when reduced motion is enabled even though it does not fit";scene.measure_media_title();scene.reduced=true;scene.media_marquee.visible(true,true,0);expect(!scene.media_marquee.active(),"Reduced motion enabled scrolling");scene.draw();check(target->EndDraw());
    scene.media={};scene.measure_media_title();expect(scene.media_title_text==L"No media"&&scene.media_marquee.distance()==0,"Empty media kept the old title");
    std::cout<<"Media UI passed: real font measurement, retained text/layout, clipped scroll frames, fixed pill height, separated 32 DIP hit targets, pixel-verified play/pause switching, artist/track reset, reduced motion and empty state.\n";
}catch(HRESULT hr){std::cerr<<"Media UI HRESULT="<<std::hex<<static_cast<unsigned long>(hr)<<'\n';result=1;}catch(const std::exception& error){std::cerr<<error.what()<<'\n';result=1;}CoUninitialize();return result;}
