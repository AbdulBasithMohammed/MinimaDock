#pragma once
#include "rendering/theme.h"
#include "rendering/glass.h"
#include <d2d1.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace minima {
// Menu: Windows 11's own flyout material (flat acrylic, hairline border) for
// surfaces that must look exactly like the system's, such as jump lists.
enum class GlassKind { Dock, Panel, Tooltip, Menu };
// Shared smoked-crystal recipe for the dock, flyouts, tooltips and calendar.
// Layers, bottom to top: outside-only soft shadow, (live blurred backdrop via
// GlassHost), tint gradient, fine grain, top sheen, low underglow, specular rim,
// top glint and a dark outer hairline. When `live` is set, each painted shape
// is recorded so the compositor can show the real backdrop through it.
class GlassPainter {
public:
    theme::Palette palette=theme::palette(false);
    bool live=false,boost=false,high_contrast=false;
    std::vector<GlassShape> shapes;
    D2D1_MATRIX_3X2_F origin=D2D1::Matrix3x2F::Identity();
    float layer_opacity=1;

    void bind(ID2D1RenderTarget* target,ID2D1Factory* value) {
        factory_.reset();value->AddRef();*factory_.put()=value;
        solid_.reset();grain_.reset();built_=false;
        check(target->CreateSolidColorBrush(palette.ink,solid_.put()));
        // Zero-mean luminance noise: premultiplied white and black specks.
        constexpr UINT size=64;std::array<std::uint32_t,size*size> pixels{};std::uint32_t seed=0x9e3779b9u;
        for(auto& pixel:pixels) {
            seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
            const std::uint32_t a=seed&0xff;
            pixel=(seed&0x100)?(a<<24)|(a<<16)|(a<<8)|a:(a<<24);
        }
        ComPtr<ID2D1Bitmap> noise;
        check(target->CreateBitmap(D2D1::SizeU(size,size),pixels.data(),size*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),noise.put()));
        check(target->CreateBitmapBrush(noise.get(),D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,D2D1_EXTEND_MODE_WRAP,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),grain_.put()));
        target_=target;cache_.clear();build();
    }
    // Swap to another drawing session of the same device (resources stay valid).
    void retarget(ID2D1RenderTarget* target){target_=target;}
    void begin_frame(){shapes.clear();layer_opacity=1;if(built_key_!=key())build();}
    static ComPtr<ID2D1Geometry> shape(ID2D1Factory* factory,D2D1_RECT_F r,float radius,bool smooth) {
        ComPtr<ID2D1Geometry> result;
        const float half=std::min(r.right-r.left,r.bottom-r.top)/2;radius=std::clamp(radius,0.0f,half);
        if(!smooth) {
            ComPtr<ID2D1RoundedRectangleGeometry> rounded;check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r,radius,radius),rounded.put()));
            check(rounded->QueryInterface(__uuidof(ID2D1Geometry),reinterpret_cast<void**>(result.put())));return result;
        }
        // Continuous ("squircle") corners: each corner begins 1.28 r from the
        // vertex and its controls sit 0.28 of that from it, keeping the midpoint
        // where a circular corner's would be while easing curvature in.
        const float p=std::min(radius*1.28f,half),k=p*.28f;
        ComPtr<ID2D1PathGeometry> path;check(factory->CreatePathGeometry(path.put()));
        ComPtr<ID2D1GeometrySink> sink;check(path->Open(sink.put()));
        const auto pt=[](float x,float y){return D2D1::Point2F(x,y);};
        sink->BeginFigure(pt(r.left+p,r.top),D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(pt(r.right-p,r.top));
        sink->AddBezier(D2D1::BezierSegment(pt(r.right-k,r.top),pt(r.right,r.top+k),pt(r.right,r.top+p)));
        sink->AddLine(pt(r.right,r.bottom-p));
        sink->AddBezier(D2D1::BezierSegment(pt(r.right,r.bottom-k),pt(r.right-k,r.bottom),pt(r.right-p,r.bottom)));
        sink->AddLine(pt(r.left+p,r.bottom));
        sink->AddBezier(D2D1::BezierSegment(pt(r.left+k,r.bottom),pt(r.left,r.bottom-k),pt(r.left,r.bottom-p)));
        sink->AddLine(pt(r.left,r.top+p));
        sink->AddBezier(D2D1::BezierSegment(pt(r.left,r.top+k),pt(r.left+k,r.top),pt(r.left+p,r.top)));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);check(sink->Close());
        check(path->QueryInterface(__uuidof(ID2D1Geometry),reinterpret_cast<void**>(result.put())));return result;
    }
    // `backdrop` optionally paints a simulated backdrop (framed preview only).
    void paint(D2D1_RECT_F r,float radius,GlassKind kind,ID2D1Brush* backdrop=nullptr) {
        auto* t=target_;const auto& p=palette;
        const bool smooth=kind==GlassKind::Panel;
        if(high_contrast) {
            solid_->SetColor(p.light?theme::rgb(0xffffff):theme::rgb(0x000000));t->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),solid_.get());
            solid_->SetColor(p.light?theme::rgb(0x000000):theme::rgb(0xffffff));t->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),solid_.get(),1.5f);
            return;
        }
        if(live) { // The compositor's blurred backdrop pane for this shape.
            D2D1_MATRIX_3X2_F current;t->GetTransform(&current);
            auto inverse=*D2D1::Matrix3x2F::ReinterpretBaseType(&origin);inverse.Invert();
            shapes.push_back({*D2D1::Matrix3x2F::ReinterpretBaseType(&current)*inverse,r,radius,kind!=GlassKind::Menu&&smooth,layer_opacity});
        }
        if(!backdrop&&cached(r,radius,kind))return;
        layers(r,radius,kind,backdrop);
    }
private:
    // The glass layers of one shape (everything except the compositor backdrop).
    void layers(D2D1_RECT_F r,float radius,GlassKind kind,ID2D1Brush* backdrop) {
        auto* t=target_;const auto& p=palette;
        const bool smooth=kind==GlassKind::Panel;
        const auto geometry=shape(factory_.get(),r,radius,smooth);
        shadow(r,radius,kind,geometry.get());
        if(kind==GlassKind::Menu){menu(r,radius,geometry.get());return;}
        if(!live&&backdrop) t->FillGeometry(geometry.get(),backdrop);
        const float top=r.top,bottom=r.bottom,height=bottom-top;
        auto& tint=kind==GlassKind::Tooltip?tooltip_:tint_;
        tint->SetStartPoint(D2D1::Point2F(r.left,top));tint->SetEndPoint(D2D1::Point2F(r.left,bottom));
        t->FillGeometry(geometry.get(),tint.get());
        grain_->SetOpacity(p.grain);t->FillGeometry(geometry.get(),grain_.get());
        sheen_->SetStartPoint(D2D1::Point2F(r.left,top));sheen_->SetEndPoint(D2D1::Point2F(r.left,top+height*.55f));
        t->FillGeometry(geometry.get(),sheen_.get());
        underglow_->SetStartPoint(D2D1::Point2F(r.left,top+height*.55f));underglow_->SetEndPoint(D2D1::Point2F(r.left,bottom));
        t->FillGeometry(geometry.get(),underglow_.get());
        // Outer definition line, then the specular rim just inside it.
        const auto outer=shape(factory_.get(),D2D1::RectF(r.left-.5f,r.top-.5f,r.right+.5f,r.bottom+.5f),radius+.5f,smooth);
        solid_->SetColor(D2D1::ColorF(p.edge_color.r,p.edge_color.g,p.edge_color.b,p.edge));t->DrawGeometry(outer.get(),solid_.get(),.8f);
        const auto inner=shape(factory_.get(),D2D1::RectF(r.left+.5f,r.top+.5f,r.right-.5f,r.bottom-.5f),std::max(0.0f,radius-.5f),smooth);
        rim_->SetStartPoint(D2D1::Point2F(r.left,top));rim_->SetEndPoint(D2D1::Point2F(r.left,bottom));
        t->DrawGeometry(inner.get(),rim_.get(),1);
        // Liquid highlight: a soft glint along the top edge, fading at the ends.
        const float inset=std::max(radius*.7f,8.0f);
        if(r.right-r.left>inset*2+8) {
            glint_->SetStartPoint(D2D1::Point2F(r.left+inset,top));glint_->SetEndPoint(D2D1::Point2F(r.right-inset,top));
            t->DrawLine(D2D1::Point2F(r.left+inset,top+1.1f),D2D1::Point2F(r.right-inset,top+1.1f),glint_.get(),kind==GlassKind::Tooltip?.8f:1.1f);
        }
    }
    void menu(D2D1_RECT_F r,float radius,ID2D1Geometry* geometry) {
        auto* t=target_;const auto& p=palette;
        // Acrylic flyout tint (opaque when transparency effects are off).
        const auto fill=p.light?theme::rgb(0xfcfcfc,live?.82f:1):theme::rgb(0x2c2c2c,live?.86f:1);
        solid_->SetColor(fill);t->FillGeometry(geometry,solid_.get());
        grain_->SetOpacity(p.grain*.6f);t->FillGeometry(geometry,grain_.get());
        const auto border=shape(factory_.get(),D2D1::RectF(r.left+.5f,r.top+.5f,r.right-.5f,r.bottom-.5f),std::max(0.0f,radius-.5f),false);
        solid_->SetColor(p.light?theme::rgb(0x000000,.10f):theme::rgb(0xffffff,.09f));t->DrawGeometry(border.get(),solid_.get(),1);
    }
    // Rendered glass, reused while a shape keeps its size, sub-pixel position and
    // look: a hovered or idle dock then costs one bitmap draw instead of the
    // shadow rings, gradients, grain and rim. Only translated shapes are cached;
    // scaled ones (a flyout opening) are drawn directly.
    struct Cached {GlassKind kind;float width,height,radius,scale;int fx,fy;unsigned look;float left,top,bitmap_width,bitmap_height;ComPtr<ID2D1Bitmap> bitmap;unsigned long long used;};
    std::vector<Cached> cache_;unsigned long long frame_=0;
    static constexpr float margin_x=24,margin_top=14,margin_bottom=30; // Shadow extent.
    bool cached(D2D1_RECT_F r,float radius,GlassKind kind) {
        D2D1_MATRIX_3X2_F m;target_->GetTransform(&m);
        if(m._11!=1||m._22!=1||m._12!=0||m._21!=0)return false;
        float dpi_x=96,dpi_y=96;target_->GetDpi(&dpi_x,&dpi_y);const float s=dpi_x/96;
        // Device-pixel placement: the bitmap starts on a whole pixel, and the shape
        // keeps its exact sub-pixel offset inside it (no blur, no shimmer).
        const float device_left=(r.left-margin_x+m._31)*s,device_top=(r.top-margin_top+m._32)*s;
        const float whole_left=std::floor(device_left),whole_top=std::floor(device_top);
        const int fx=static_cast<int>(std::lround((device_left-whole_left)*8)),fy=static_cast<int>(std::lround((device_top-whole_top)*8));
        const float width=r.right-r.left,height=r.bottom-r.top;
        Cached* entry=nullptr;
        for(auto& c:cache_)if(c.kind==kind&&c.width==width&&c.height==height&&c.radius==radius&&c.scale==s&&c.fx==fx&&c.fy==fy&&c.look==built_key_){entry=&c;break;}
        if(!entry) {
            const float bw=std::ceil((width+2*margin_x)*s+2)/s,bh=std::ceil((height+margin_top+margin_bottom)*s+2)/s;
            ComPtr<ID2D1BitmapRenderTarget> layer;
            if(FAILED(target_->CreateCompatibleRenderTarget(D2D1::SizeF(bw,bh),layer.put())))return false;
            auto* previous=target_;target_=layer.get();
            layer->BeginDraw();layer->Clear(D2D1::ColorF(0,0));
            // Place r at its sub-pixel offset from the bitmap's whole-pixel origin.
            layer->SetTransform(D2D1::Matrix3x2F::Translation(((r.left+m._31)*s-whole_left)/s-r.left,((r.top+m._32)*s-whole_top)/s-r.top));
            bool ok=true;
            try{layers(r,radius,kind,nullptr);}catch(...){ok=false;}
            ok=SUCCEEDED(layer->EndDraw())&&ok;target_=previous;
            ComPtr<ID2D1Bitmap> bitmap;if(!ok||FAILED(layer->GetBitmap(bitmap.put())))return false;
            if(cache_.size()>=8)cache_.erase(std::min_element(cache_.begin(),cache_.end(),[](const Cached& a,const Cached& b){return a.used<b.used;}));
            cache_.push_back({kind,width,height,radius,s,fx,fy,built_key_,0,0,bw,bh,std::move(bitmap),0});
            entry=&cache_.back();
        }
        entry->used=++frame_;
        const float left=whole_left/s-m._31,top=whole_top/s-m._32;
        target_->DrawBitmap(entry->bitmap.get(),D2D1::RectF(left,top,left+entry->bitmap_width,top+entry->bitmap_height),1,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        return true;
    }
    ID2D1RenderTarget* target_=nullptr;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1SolidColorBrush> solid_;
    ComPtr<ID2D1BitmapBrush> grain_;
    ComPtr<ID2D1LinearGradientBrush> tint_,tooltip_,sheen_,underglow_,rim_,glint_;
    bool built_=false;unsigned built_key_=~0u;
    static void check(HRESULT hr){if(FAILED(hr))throw hr;}
    unsigned key() const {
        unsigned h=(palette.light?1u:0u)|(live?2u:0u)|(boost?4u:0u);
        for(const auto& c:{palette.tint_top,palette.tint_bottom,palette.solid_top,palette.solid_bottom,palette.tooltip_top,palette.tooltip_bottom})
            for(const float v:{c.r,c.g,c.b,c.a}){h^=static_cast<unsigned>(std::lround(v*255))+0x9e3779b9u+(h<<6)+(h>>2);}
        return h|1u<<31; // Never ~0u (the "not built" marker).
    }
    static D2D1_COLOR_F alpha(D2D1_COLOR_F c,float a){c.a=std::clamp(a,0.0f,1.0f);return c;}
    ComPtr<ID2D1LinearGradientBrush> gradient(std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        ComPtr<ID2D1GradientStopCollection> collection;std::vector<D2D1_GRADIENT_STOP> values(stops);
        check(target_->CreateGradientStopCollection(values.data(),static_cast<UINT32>(values.size()),collection.put()));
        ComPtr<ID2D1LinearGradientBrush> brush;
        check(target_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(0,1)),collection.get(),brush.put()));
        return brush;
    }
    void build() {
        cache_.clear();
        const auto& p=palette;const float extra=boost?p.boost:0;
        const auto top=live?alpha(p.tint_top,p.tint_top.a+extra):p.solid_top,bottom=live?alpha(p.tint_bottom,p.tint_bottom.a+extra):p.solid_bottom;
        tint_=gradient({{0,top},{1,bottom}});
        tooltip_=gradient({{0,live?alpha(p.tooltip_top,p.tooltip_top.a+extra):p.solid_top},{1,live?alpha(p.tooltip_bottom,p.tooltip_bottom.a+extra):p.solid_bottom}});
        const auto white=theme::rgb(0xffffff);
        sheen_=gradient({{0,alpha(white,p.sheen)},{1,alpha(white,0)}});
        underglow_=gradient({{0,alpha(white,0)},{1,alpha(white,p.underglow)}});
        rim_=gradient({{0,alpha(white,p.rim_top)},{.45f,alpha(white,p.rim_mid)},{1,alpha(white,p.rim_bottom)}});
        glint_=gradient({{0,alpha(white,0)},{.5f,alpha(white,p.glint)},{1,alpha(white,0)}});
        built_=true;built_key_=key();
    }
    // Soft contact shadow, masked to the outside so it never dirties the glass.
    void shadow(D2D1_RECT_F r,float radius,GlassKind kind,ID2D1Geometry* geometry) {
        auto* t=target_;const auto& p=palette;
        const int rings=kind==GlassKind::Panel||kind==GlassKind::Menu?9:kind==GlassKind::Dock?7:4;
        const float spread=kind==GlassKind::Panel||kind==GlassKind::Menu?2.0f:kind==GlassKind::Dock?1.5f:1.0f;
        ComPtr<ID2D1RectangleGeometry> bounds;
        if(FAILED(factory_->CreateRectangleGeometry(D2D1::RectF(r.left-60,r.top-60,r.right+60,r.bottom+80),bounds.put())))return;
        ID2D1Geometry* parts[]={bounds.get(),geometry};ComPtr<ID2D1GeometryGroup> outside;
        if(FAILED(factory_->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE,parts,2,outside.put())))return;
        t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),outside.get()),nullptr);
        for(int i=rings;i>0;--i) {
            const float grow=static_cast<float>(i)*spread,drop=1+static_cast<float>(i)*spread*.6f;
            const float a=p.shadow*std::max(.004f,.034f-static_cast<float>(i)*.0032f);
            solid_->SetColor(alpha(p.shadow_color,a));
            t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left-grow,r.top-grow*.35f+drop*.4f,r.right+grow,r.bottom+grow*.5f+drop),radius+grow,radius+grow),solid_.get());
        }
        t->PopLayer();
    }
};
}
