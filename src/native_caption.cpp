// Chromium-style geometry implemented independently with Win32/GDI+.
// Geometry reference: Chromium 156 windows_caption_button.cc/windows_icon_painter.cc
// (BSD-style license); no code from the GPL-licensed companion applications.
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <MddBootstrap.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>
#include "tab_layout.h"

using namespace Gdiplus;
using namespace winrt::Microsoft::UI::Windowing;
using Callback = void (*)(void*, unsigned, unsigned long long);

namespace Metrics {
constexpr float band = 40, button = 46, glyph = 10;
constexpr float drag_gap = 40;
constexpr float tab_top = 5, tab_radius = 10, tab_foot = 12;
constexpr float control = 28, arrow_left = 9, arrow_radius = 8, close_hit = 24;
constexpr float panel_width = 360, panel_max = 600, panel_pad = 12;
constexpr float search_height = 32, row_height = 44, group_height = 20, group_gap = 8;
constexpr float scrollbar_visible = 6, scrollbar_hit = 8, scrollbar_edge = 2;
constexpr double tab_animation_ms = 200;
}

struct Item {
    unsigned long long id;
    std::wstring title, path;
};

struct TabDrop {
    unsigned long long document;
    HWND target;
    unsigned slot;
    int screen_x, screen_y, anchor_x, anchor_y;
};

struct Caption;
static std::vector<Caption*> captions;
static std::unordered_map<unsigned long long,TabDrop> pending_drops;
static unsigned long long next_drop = 1;


struct Caption {
    HWND parent{}, child{};
    AppWindow app{nullptr};
    Callback callback{};
    void* context{};
    std::vector<Item> tabs, opened, recent;
    unsigned long long active{};
    bool dark{}, high_contrast{};
    int height{40}, right{144}, hovered{-1};
    // Self-drawn caption buttons, as in SumatraX: the band is 40 DIP and the
    // three buttons fill it edge to edge instead of Windows' 32-DIP cells.
    int btn_left{}, btn_width{46};
    int caption_hover{0}, caption_pressed{0};
    float scale{1}, tab_width{240}, tab_left{46}, viewport{}, plus_x{}, scroll{};
    unsigned long long pressed_id{};
    int pressed_action{-1}, drop_slot{-1};
    bool dragging{};
    bool attached_drag{};
    bool frame_collapsed{};
    HRGN original_region{};
    unsigned long long pending_drop{};
    float incoming_x{};
    int drag_slot{-1};
    float drag_x{};
    struct TabMotion { float from{}, target{}; ULONGLONG start{}; };
    std::unordered_map<unsigned long long,TabMotion> tab_motion;
    POINT down_screen{};
    int anchor_x{}, anchor_y{};
    int ghost_width{}, ghost_height{}, source_tab_width{};
    HWND ghost{};
    std::wstring drag_title;
    struct SearchRow { std::wstring title, detail; unsigned command{}; unsigned long long id{}; };
    HWND panel{}, search_box{}, search_button{}, search_clear{}, search_edit{}, search_list_host{}, search_list{}, search_tip{}, search_scroll_host{}, search_scroll{};
    WNDPROC edit_proc{}, list_proc{};
    bool search_overflow{}, syncing_search_scroll{}, search_scroll_hover{}, search_composing{};
    int search_ime_key{};
    HFONT panel_font{}, detail_font{};
    HDC search_buffer_dc{};
    HBITMAP search_buffer_bitmap{};
    HGDIOBJ search_buffer_original{};
    int search_buffer_width{},search_buffer_height{};
    std::vector<SearchRow> search_rows;
    int search_hot{-1}, search_pressed{-1}, search_wheel_delta{};
    std::wstring search_tip_text;
    HWND tab_tip{};
    std::wstring tab_tip_text;
    bool suppress_panel_open{}, closing_panel{};

    Color system_color(int role) const { COLORREF c=GetSysColor(role);return Color(255,GetRValue(c),GetGValue(c),GetBValue(c)); }
    Color strip() const { return high_contrast?system_color(COLOR_BTNFACE):dark?Color(255,32,33,36):Color(255,211,227,253); }
    Color selected() const { return high_contrast?system_color(COLOR_WINDOW):dark?Color(255,53,54,58):Color(255,255,255,255); }
    Color panel_color() const { return high_contrast?system_color(COLOR_WINDOW):dark?Color(255,41,42,45):Color(255,255,255,255); }
    Color foreground() const { return high_contrast?system_color(COLOR_WINDOWTEXT):dark?Color(255,255,255,255):Color(255,0,0,0); }
    Color highlighted_foreground(bool highlighted) const { return high_contrast && highlighted?system_color(COLOR_HIGHLIGHTTEXT):foreground(); }
    Color overlay(BYTE alpha) const { auto c=foreground();return Color(alpha,c.GetR(),c.GetG(),c.GetB()); }
    Color muted() const { auto f=foreground(),b=strip();return Color(255,(f.GetR()*102+b.GetR()*153)/255,(f.GetG()*102+b.GetG()*153)/255,(f.GetB()*102+b.GetB()*153)/255); }
    Color hover() const { return high_contrast?system_color(COLOR_HIGHLIGHT):overlay(0x1A); }
    static void rounded(GraphicsPath& p,float x,float y,float w,float h,float radius) {
        float d=radius*2;p.AddArc(x,y,d,d,180,90);p.AddArc(x+w-d,y,d,d,270,90);
        p.AddArc(x+w-d,y+h-d,d,d,0,90);p.AddArc(x,y+h-d,d,d,90,90);p.CloseFigure();
    }

    // Windows collapses its own caption buttons; these three replace them and
    // fill the whole 40-DIP band, so hover/press covers the full band height.
    int caption_button(int x, int y) const {
        if (y < 0 || y >= height || x < btn_left) return 0;
        int width = std::max(1, btn_width);
        int index = (x - btn_left) / width;
        return index >= 0 && index < 3 ? index + 1 : 0;
    }

    void activate_caption_button(int id) {
        switch (id) {
        case 1: PostMessageW(parent, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
        case 2: PostMessageW(parent, WM_SYSCOMMAND, IsZoomed(parent) ? SC_RESTORE : SC_MAXIMIZE, 0); break;
        case 3: PostMessageW(parent, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }
    void paint_caption_buttons(Graphics& g) {
        // Draw in physical pixels: a 150% stroke is two pixels, never 1.5.
        auto saved=g.Save();g.ResetTransform();
        bool window_active = GetForegroundWindow() == parent || (panel && GetForegroundWindow()==panel);
        const int size=std::max(1,int(std::lround(Metrics::glyph*scale)));
        const int stroke=std::max(1,int(std::lround(scale)));
        for (int i = 0; i < 3; ++i) {
            int x=btn_left+i*btn_width;
            int id = i + 1;
            bool hot = caption_hover == id, down = caption_pressed == id;
            if (i == 2) {
                if (hot || down) {
                    SolidBrush back(high_contrast?system_color(COLOR_HIGHLIGHT):down ? Color(0x98,232,17,35) : Color(255,232,17,35));
                    g.FillRectangle(&back,x,0,btn_width,height);
                }
            } else if (hot || down) {
                SolidBrush back(high_contrast?system_color(COLOR_HIGHLIGHT):overlay(down?0x33:0x1A));
                g.FillRectangle(&back,x,0,btn_width,height);
            }
            Color ink = high_contrast && (hot || down)?system_color(COLOR_HIGHLIGHTTEXT):(i == 2 && (hot || down)) ? Color(255,255,255,255)
                : (window_active ? foreground() : muted());
            float left=float(x+(btn_width-size)/2),top=float((height-size)/2);
            float edge=stroke/2.0f,span=float(size-stroke);
            Pen pen(ink,float(stroke));
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            if (i == 0) {
                SolidBrush line(ink);g.FillRectangle(&line,int(left),int(top)+size/2-stroke/2,size,stroke);
            } else if (i == 1) {
                if (IsZoomed(parent)) {
                    int offset=std::max(stroke,int(std::lround(2*scale)));
                    g.DrawLine(&pen,left+offset+edge,top+edge,left+size-edge,top+edge);
                    g.DrawLine(&pen,left+size-edge,top+edge,left+size-edge,top+size-offset-edge);
                    g.DrawLine(&pen,left+offset+edge,top+edge,left+offset+edge,top+offset-edge);
                    GraphicsPath front;rounded(front,left+edge,top+offset+edge,span-offset,span-offset,std::max(0.5f,float(std::floor(1.5f*scale))));
                    g.DrawPath(&pen,&front);
                } else {
                    GraphicsPath frame;rounded(frame,left+edge,top+edge,span,span,std::max(0.5f,float(std::floor(1.5f*scale))));
                    g.DrawPath(&pen,&frame);
                }
            } else {
                auto clipped=g.Save();g.SetClip(RectF(left,top,float(size),float(size)));
                pen.SetWidth(stroke*((hot||down)?1.10f:1.05f));
                g.DrawLine(&pen,left,top,left+size,top+size);
                g.DrawLine(&pen,left+size,top,left,top+size);g.Restore(clipped);
            }
        }
        g.Restore(saved);
    }

    void theme() {
        DWORD light = 1, bytes = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
        dark = light == 0;
        HIGHCONTRASTW contrast{sizeof(contrast)};
        high_contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0) && (contrast.dwFlags&HCF_HIGHCONTRASTON);
        auto bar = app.TitleBar();
        auto color = [](Color c) { return winrt::Windows::UI::Color{c.GetA(),c.GetR(),c.GetG(),c.GetB()}; };
        bar.ButtonBackgroundColor(color(strip()));
        bar.ButtonInactiveBackgroundColor(color(strip()));
        bar.ButtonForegroundColor(color(foreground()));
        bar.ButtonInactiveForegroundColor(color(muted()));
        bar.ButtonHoverBackgroundColor(color(hover()));
        bar.ButtonPressedBackgroundColor(color(hover()));
    }

    void layout() {
        RECT client{};
        GetClientRect(parent, &client);
        scale = GetDpiForWindow(parent) / 96.0f;
        height = int(std::lround(Metrics::band * scale));
        // 46-DIP cells fill the whole 40-DIP band, like SumatraX's own caption
        // buttons; Windows' caption buttons are collapsed, so nothing is drawn
        // by the system and the band needs no region workaround.
        btn_width = int(std::lround(Metrics::button * scale));
        right = btn_width * 3;
        int full = int(std::max(1L, client.right));
        MoveWindow(child, 0, 0, full, height, TRUE);
        SetWindowRgn(child, nullptr, TRUE);
        btn_left = full - right;
        float logical = full / scale;
        float buttons = right / scale;
        // Share the available strip across all tabs; the dropdown keeps narrow tabs identifiable.
        viewport = std::max(0.0f, logical - tab_left - 40 - Metrics::drag_gap - buttons);
        layout_tab_slots();
    }

    void layout_tab_slots() {
        int count = int(tabs.size()) + (drop_slot >= 0 ? 1 : 0);
        if (dragging && !attached_drag && pressed_index() >= 0) --count;
        tab_width = count > 0 ? std::clamp(viewport / count, 0.01f, 280.0f) : 240.0f;
        scroll = 0;
        plus_x = tab_left + std::min(viewport, tab_width * count);
        float drag_left = plus_x + 40;
        winrt::Windows::Graphics::RectInt32 drag{
            int(std::ceil(drag_left * scale)), 0,
            std::max(0, btn_left - int(std::ceil(drag_left * scale))), height};
        app.TitleBar().SetDragRectangles({drag});
        arrange_tabs();
        InvalidateRect(child, nullptr, FALSE);
    }

    // Same fixed vertical sides and reverse bottom arcs as ChromeTabBackground.OnRender.
    static void tab_path(GraphicsPath& path, float x, float y, float width, float height) {
        float top = std::min(Metrics::tab_radius,width/3), bottom = std::min(Metrics::tab_foot,width/3);
        path.AddArc(x-2*bottom, y+height-2*bottom, 2*bottom, 2*bottom, 90, -90);
        path.AddLine(x,y+height-bottom,x,y+top);
        path.AddArc(x,y,2*top,2*top,180,90);
        path.AddLine(x+top,y,x+width-top,y);
        path.AddArc(x+width-2*top,y,2*top,2*top,270,90);
        path.AddLine(x+width,y+top,x+width,y+height-bottom);
        path.AddArc(x+width,y+height-2*bottom,2*bottom,2*bottom,180,-90);
        path.CloseFigure();
    }

    void text(HDC dc, const std::wstring& value, float x, float y, float width, float h,
        bool selected_text = false, bool highlighted = false) {
        HFONT font = CreateFontW(-int(13*scale),0,0,0,selected_text ? FW_MEDIUM : FW_NORMAL,
            FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        auto old = SelectObject(dc,font);
        Color c = high_contrast && highlighted?system_color(COLOR_HIGHLIGHTTEXT):selected_text ? foreground() : muted();
        SetTextColor(dc,RGB(c.GetR(),c.GetG(),c.GetB()));
        SetBkMode(dc,TRANSPARENT);
        RECT rect{int(x*scale),int(y*scale),int((x+width)*scale),int((y+h)*scale)};
        DrawTextW(dc,value.c_str(),int(value.size()),&rect,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,old);
        DeleteObject(font);
    }

    static float motion_x(const TabMotion& motion) {
        if(!motion.start) return motion.target;
        double progress=std::min(1.0,double(GetTickCount64()-motion.start)/Metrics::tab_animation_ms);
        double remaining=1-progress;
        return motion.target+float((motion.from-motion.target)*remaining*remaining*remaining);
    }

    int pressed_index() const {
        for(size_t i=0;i<tabs.size();++i) if(tabs[i].id==pressed_id) return int(i);
        return -1;
    }

    float tab_x(size_t index) const {
        auto motion=tab_motion.find(tabs[index].id);
        return motion==tab_motion.end()?tab_left+index*tab_width-scroll:motion_x(motion->second);
    }

    void arrange_tabs() {
        int from=pressed_index();
        bool pending=false;
        for(size_t i=0;i<tabs.size();++i) {
            int slot=int(i);
            if(dragging && !attached_drag && from>=0 && slot>from) --slot;
            if(drop_slot>=0 && slot>=drop_slot) ++slot;
            if(dragging && attached_drag && from>=0) {
                if(from<drag_slot && int(i)>from && int(i)<=drag_slot) --slot;
                if(from>drag_slot && int(i)>=drag_slot && int(i)<from) ++slot;
            }
            float target=tab_left+slot*tab_width-scroll;
            auto [motion,inserted]=tab_motion.try_emplace(tabs[i].id,TabMotion{target,target,0});
            if(dragging && attached_drag && int(i)==from) {
                motion->second={drag_x,drag_x,0};
            } else if(std::abs(motion->second.target-target)>0.01f) {
                motion->second={motion_x(motion->second),target,GetTickCount64()};
            }
            pending=pending || (motion->second.start &&
                GetTickCount64()-motion->second.start<Metrics::tab_animation_ms);
        }
        if(pending || dragging) SetTimer(child,1,15,nullptr);
        else KillTimer(child,1);
        InvalidateRect(child,nullptr,FALSE);
    }

    void paint_tab(HDC dc, size_t i) {
        float x=tab_x(i),h=height/scale;
        if(x+tab_width<tab_left || x>tab_left+viewport ||
            (dragging && !attached_drag && tabs[i].id==pressed_id)) return;
        bool active_tab=tabs[i].id==active || (dragging && tabs[i].id==pressed_id);
        {
            Graphics g(dc);g.ScaleTransform(scale,scale);g.SetSmoothingMode(SmoothingModeAntiAlias);
            if(active_tab || hovered==int(i)) {
                GraphicsPath path;tab_path(path,x,Metrics::tab_top,tab_width,h-Metrics::tab_top);
                SolidBrush brush(active_tab?selected():hover());g.FillPath(&brush,&path);
            } else if(i+1<tabs.size() && tabs[i+1].id!=active) {
                Pen separator(dark?Color(255,65,69,73):Color(255,218,218,218),1);
                g.DrawLine(&separator,x+tab_width,h/2-7,x+tab_width,h/2+7);
            }
        }
        int saved=SaveDC(dc);
        IntersectClipRect(dc,int(std::ceil(x*scale)),0,int(std::floor((x+tab_width)*scale)),height);
        const auto content=TabLayout::Calculate(tab_width,active_tab);
        bool show_close=content.close_visible;
        float text_width=content.title_width;
        bool show_text=text_width>=20;
        float icon_size=content.icon_size;
        float icon_x=x+content.icon_left;
        HICON icon=reinterpret_cast<HICON>(SendMessageW(parent,WM_GETICON,ICON_SMALL,0));
        if(icon && content.icon_visible && icon_size>0) DrawIconEx(dc,int(icon_x*scale),int(((h+Metrics::tab_top-icon_size)/2)*scale),icon,int(icon_size*scale),int(icon_size*scale),0,nullptr,DI_NORMAL);
        if(show_text) text(dc,tabs[i].title,x+content.title_left,5,text_width,h-5,active_tab,!active_tab && hovered==int(i));
        {
            Graphics g(dc);g.ScaleTransform(scale,scale);g.SetSmoothingMode(SmoothingModeAntiAlias);
            float cx=x+content.close_center,cy=(h+5)/2;
            if(show_close && hovered==int(i) && close_hover && !dragging) {
                SolidBrush brush(hover());g.FillEllipse(&brush,cx-content.close_size/2,cy-content.close_size/2,content.close_size,content.close_size);
            }
            if(show_close) {
                bool highlighted=hovered==int(i) && (!active_tab || close_hover);
                Pen cross(high_contrast && highlighted?system_color(COLOR_HIGHLIGHTTEXT):active_tab?foreground():muted(),1.5f);
                g.DrawLine(&cross,cx-4,cy-4,cx+4,cy+4);g.DrawLine(&cross,cx+4,cy-4,cx-4,cy+4);
            }
        }
        RestoreDC(dc,saved);
    }

    void paint() {
        PAINTSTRUCT ps{};
        HDC destination = BeginPaint(child,&ps);
        RECT rect{}; GetClientRect(child,&rect);
        HDC dc = CreateCompatibleDC(destination);
        HBITMAP bitmap = CreateCompatibleBitmap(destination,std::max(1L,rect.right),std::max(1L,rect.bottom));
        auto old_bitmap = SelectObject(dc,bitmap);
        {
            Graphics g(dc); g.Clear(strip()); g.ScaleTransform(scale,scale);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            float h = height / scale;
            bool prefix_pressed=panel || (pressed_action==-2 && GetCapture()==child);
            Pen glyph(highlighted_foreground(prefix_pressed),1.5f);
            glyph.SetStartCap(LineCapRound); glyph.SetEndCap(LineCapRound);
            Color prefix_color=high_contrast?system_color(prefix_pressed?COLOR_HIGHLIGHT:COLOR_BTNFACE)
                :overlay(prefix_pressed?0x33:hovered==-2?0x1A:0x0D);
            SolidBrush prefix_background(prefix_color);
            GraphicsPath prefix;rounded(prefix,Metrics::arrow_left,h/2-Metrics::control/2,Metrics::control,Metrics::control,Metrics::arrow_radius);
            g.FillPath(&prefix_background,&prefix);
            g.DrawLine(&glyph,18.0f,h/2-2,23.0f,h/2+3);
            g.DrawLine(&glyph,23.0f,h/2+3,28.0f,h/2-2);
        }
        {
            int saved = SaveDC(dc);
            IntersectClipRect(dc,int((tab_left-12)*scale),0,int((tab_left+viewport+12)*scale),height);
            int top=-1;
            for (size_t i=0;i<tabs.size();++i) {
                if(tabs[i].id==(dragging?pressed_id:active)) top=int(i);
                else paint_tab(dc,i);
            }
            if(top>=0) paint_tab(dc,size_t(top));
            RestoreDC(dc,saved);
            Graphics g(dc); g.ScaleTransform(scale,scale); g.SetSmoothingMode(SmoothingModeAntiAlias);
            float cx=plus_x+20, cy=height/scale/2;
            if (hovered==-3) { SolidBrush brush(hover()); g.FillEllipse(&brush,cx-Metrics::control/2,cy-Metrics::control/2,Metrics::control,Metrics::control); }
            Pen pen(highlighted_foreground(hovered==-3),1.5f);
            g.DrawLine(&pen,cx-5,cy,cx+5,cy); g.DrawLine(&pen,cx,cy-5,cx,cy+5);
        }
        {
            Graphics g(dc); g.ScaleTransform(scale,scale); g.SetSmoothingMode(SmoothingModeAntiAlias);
            paint_caption_buttons(g);
        }
        BitBlt(destination,0,0,rect.right,rect.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,old_bitmap); DeleteObject(bitmap); DeleteDC(dc); EndPaint(child,&ps);
    }

    bool close_hover{};
    bool tab_close_visible(size_t index) const {
        return TabLayout::Calculate(tab_width,tabs[index].id==active).close_visible;
    }
    float tab_close_x(size_t index) const { return tab_x(index)+TabLayout::Calculate(tab_width,tabs[index].id==active).close_center; }
    float tab_close_size() const { return std::min(Metrics::close_hit,tab_width); }

    void update_tab_tip(int index,int button=0) {
        std::wstring value;
        if(button==1) value=L"最小化窗口";
        else if(button==3) value=L"关闭窗口";
        // The maximize area belongs to Snap; keep its native state name and
        // never overlay it with an application tooltip.
        else if(button==0) {
            if(index==-2) value=L"搜索标签页 (Ctrl+Shift+A)";
            else if(index==-3) value=L"打开文件 (Ctrl+O)";
            else if(index>=0 && index<int(tabs.size())) value=display_path(tabs[index].path.empty()?tabs[index].title:tabs[index].path);
        }
        if(value==tab_tip_text) return;
        if(!tab_tip && !value.empty()) {
            tab_tip=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE,TOOLTIPS_CLASSW,nullptr,
                WS_POPUP|TTS_NOPREFIX|TTS_ALWAYSTIP,0,0,0,0,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
            TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=child;
            tool.uId=reinterpret_cast<UINT_PTR>(child);tool.lpszText=const_cast<wchar_t*>(L"");
            SendMessageW(tab_tip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
            SendMessageW(tab_tip,TTM_SETMAXTIPWIDTH,0,int(600*scale));
        }
        if(!tab_tip) return;
        SendMessageW(tab_tip,TTM_POP,0,0);tab_tip_text=std::move(value);
        TOOLINFOW tool{sizeof(tool)};tool.hwnd=child;tool.uId=reinterpret_cast<UINT_PTR>(child);tool.lpszText=tab_tip_text.data();
        SendMessageW(tab_tip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
    }
    int hit(float x,float y) {
        float middle=height/scale/2;
        if (x>=9 && x<37 && y>=middle-14 && y<middle+14) return -2;
        if (x>=plus_x+6 && x<plus_x+34 && y>=middle-14 && y<middle+14) return -3;
        if (x>=tab_left && x<tab_left+viewport && y>=5) {
            for(size_t i=0;i<tabs.size();++i) if(tabs[i].id==active && x>=tab_x(i) && x<tab_x(i)+tab_width) return int(i);
            for(size_t i=tabs.size();i>0;--i) if(x>=tab_x(i-1) && x<tab_x(i-1)+tab_width) return int(i-1);
        }
        return -1;
    }
    std::wstring GetActiveTitle() const {
        for (const auto& item : tabs) if(item.id==active) return item.title;
        return L"Markdown Viewer";
    }

    static Caption* drop_target(POINT screen) {
        HWND frame = GetAncestor(WindowFromPoint(screen), GA_ROOT);
        for (auto* caption : captions) {
            if (caption->parent != frame || caption->frame_collapsed) continue;
            POINT local = screen;
            ScreenToClient(caption->child, &local);
            if (local.y >= 0 && local.y < caption->height && local.x >= caption->tab_left * caption->scale &&
                local.x < (caption->tab_left + caption->viewport) * caption->scale) return caption;
        }
        return nullptr;
    }

    void set_drop_slot(int slot) {
        if (drop_slot == slot) return;
        drop_slot = slot;
        layout_tab_slots();
    }

    void collapse_frame() {
        HRGN saved = CreateRectRgn(0, 0, 0, 0);
        if (saved && GetWindowRgn(parent, saved) == ERROR) {
            DeleteObject(saved);
            saved = nullptr;
        }
        HRGN empty = CreateRectRgn(0, 0, 0, 0);
        // Keep native capture on the title bar until the queued drop is accepted.
        if (empty && SetWindowRgn(parent, empty, TRUE)) {
            original_region = saved;
            frame_collapsed = true;
        } else {
            if (empty) DeleteObject(empty);
            if (saved) DeleteObject(saved);
        }
    }

    void restore_frame() {
        if (!frame_collapsed) return;
        HRGN saved = original_region;
        frame_collapsed = false;
        original_region = nullptr;
        if (!SetWindowRgn(parent, saved, TRUE) && saved) DeleteObject(saved);
    }

    void finish_drop(unsigned long long document) {
        if (pending_drop != document) return;
        pending_drop = 0;
        restore_frame();
        set_drop_slot(-1);
    }

    static LRESULT CALLBACK ghost_procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto self=reinterpret_cast<Caption*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Caption*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(message==WM_NCHITTEST) return HTTRANSPARENT;
        if(message==WM_ERASEBKGND) return 1;
        if(message==WM_PAINT && self) {
            PAINTSTRUCT ps{};auto dc=BeginPaint(window,&ps);RECT rect{};GetClientRect(window,&rect);
            {Graphics graphics(dc);graphics.Clear(self->selected());}
            self->text(dc,self->drag_title,12,0,rect.right/self->scale-24,rect.bottom/self->scale,true);
            EndPaint(window,&ps);return 0;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }

    void update_drag(const POINT* event_screen=nullptr) {
        if(!dragging) return;
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000) {cancel_drag();return;}
        POINT screen{};if(event_screen) screen=*event_screen;else GetCursorPos(&screen);
        Caption* target=drop_target(screen);
        POINT local=screen;ScreenToClient(child,&local);
        // Keep small vertical hand movements attached to the source strip.
        if(!frame_collapsed && !target && local.x>=tab_left*scale && local.x<(tab_left+viewport+40)*scale &&
            local.y>=-15*scale && local.y<height+15*scale) target=this;
        bool was_attached=attached_drag;
        attached_drag=target==this;
        if(was_attached!=attached_drag) layout_tab_slots();
        if(attached_drag && !tabs.empty()) {
            drag_x=std::clamp(local.x/scale-anchor_x/scale,tab_left,tab_left+(tabs.size()-1)*tab_width);
            drag_slot=std::clamp(int(std::floor((drag_x-tab_left)/tab_width+0.5f)),0,int(tabs.size())-1);
        }
        Caption* destination = target == this ? nullptr : target;
        for (auto* caption : captions) {
            if (caption != destination && !caption->pending_drop) caption->set_drop_slot(-1);
        }
        POINT position{screen.x-anchor_x,screen.y-anchor_y};
        int width=source_tab_width, preview_height=height;
        if(destination) {
            POINT hit=screen;ScreenToClient(destination->child,&hit);
            float dx=std::clamp(destination->viewport/(destination->tabs.size()+1),0.01f,280.0f);
            width=std::max(1,int(std::lround(dx*destination->scale)));
            float anchor=float(MulDiv(anchor_x,width,source_tab_width))/destination->scale;
            int count=int(destination->tabs.size());
            float left=std::clamp(hit.x/destination->scale-anchor,destination->tab_left,destination->tab_left+dx*count);
            int slot=std::clamp(int(std::lround((left-destination->tab_left)/dx)),0,count);
            destination->incoming_x=left;
            destination->set_drop_slot(slot);
            position={int(std::lround(left*destination->scale)),0};
            ClientToScreen(destination->child,&position);
            preview_height=destination->height;
        }
        if(ghost) {
            if(attached_drag) ShowWindow(ghost,SW_HIDE);
            else {
                if(width!=ghost_width || preview_height!=ghost_height) {
                    SetWindowRgn(ghost,CreateRoundRectRgn(0,0,width,preview_height,int(14*scale),int(14*scale)),FALSE);
                    ghost_width=width;ghost_height=preview_height;
                }
                SetWindowPos(ghost,HWND_TOPMOST,position.x,position.y,width,preview_height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            }
        }
        arrange_tabs();
    }

    void start_drag(const POINT& screen) {
        dragging=true;
        attached_drag=true;
        source_tab_width=std::max(1,int(tab_width*scale));
        WNDCLASSW cls{};cls.lpfnWndProc=ghost_procedure;cls.hInstance=GetModuleHandleW(nullptr);
        cls.style=CS_HREDRAW|CS_VREDRAW;
        cls.lpszClassName=L"MarkdownViewer.TabDragPreview";RegisterClassW(&cls);
        ghost=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,
            cls.lpszClassName,L"",WS_POPUP,0,0,source_tab_width,height,
            nullptr,nullptr,cls.hInstance,this);
        if(ghost) {
            SetLayeredWindowAttributes(ghost,0,220,LWA_ALPHA);
            ghost_width=0;ghost_height=0;
            if(tabs.size()==1) collapse_frame();
        }
        update_tab_tip(-1);
        SetTimer(child,1,15,nullptr);
        update_drag(&screen);
    }

    void cancel_drag(bool focus_reader=true) {
        // Clear state before ReleaseCapture, which synchronously sends WM_CAPTURECHANGED.
        bool restore_focus=pressed_id!=0;
        bool packed_source=dragging && !attached_drag;
        dragging=false;attached_drag=false;pressed_id=0;pressed_action=-1;drag_slot=-1;
        KillTimer(child,1);
        HWND old_ghost=ghost;ghost=nullptr;
        if(old_ghost) DestroyWindow(old_ghost);
        if (!pending_drop) restore_frame();
        for (auto* caption : captions) if (!caption->pending_drop) caption->set_drop_slot(-1);
        if(GetCapture()==child) ReleaseCapture();
        if(packed_source) layout_tab_slots();
        else arrange_tabs();
        if(restore_focus && focus_reader) callback(context,8,0);
    }

    void finish_drag(const POINT& screen) {
        update_drag(&screen);
        if (!dragging) return;
        Caption* target=drop_target(screen);
        if(attached_drag) target=this;
        int from=pressed_index(),slot=drag_slot;
        unsigned insertion=unsigned(target && target->drop_slot>=0?target->drop_slot:0);
        if(target==this && from>=0) insertion=unsigned(slot>from?slot+1:slot);
        if (target != this) {
            pending_drop = pressed_id;
            if (target) target->pending_drop = pressed_id;
        }
        TabDrop drop{pressed_id,target?target->parent:nullptr,
            insertion,screen.x,screen.y,anchor_x+int(std::lround(tab_left*scale)),anchor_y};
        cancel_drag(target==this);
        if(target==this && from>=0 && slot>=0 && from!=slot) {
            auto item=std::move(tabs[from]);tabs.erase(tabs.begin()+from);
            tabs.insert(tabs.begin()+slot,std::move(item));
            arrange_tabs();
        }
        auto token=next_drop++;pending_drops.emplace(token,drop);
        callback(context,7,token);
    }
    void menu() {
        if(suppress_panel_open) {suppress_panel_open=false;return;}
        if(panel) {close_panel(true);return;}
        WNDCLASSW cls{};cls.lpfnWndProc=panel_procedure;cls.hInstance=GetModuleHandleW(nullptr);
        cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));cls.lpszClassName=L"MarkdownViewer.TabSearch";RegisterClassW(&cls);
        POINT point{int(Metrics::arrow_left*scale),height+int(6*scale)};ClientToScreen(child,&point);
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&monitor);
        int width=std::min(int(Metrics::panel_width*scale),int(monitor.rcWork.right-monitor.rcWork.left));
        int panel_height=std::min(int(Metrics::panel_max*scale),int(monitor.rcWork.bottom-monitor.rcWork.top));
        point.x=std::clamp(int(point.x),int(monitor.rcWork.left),int(monitor.rcWork.right)-width);
        point.y=std::clamp(int(point.y),int(monitor.rcWork.top),int(monitor.rcWork.bottom)-panel_height);
        panel=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"搜索标签页",WS_POPUP|WS_CLIPCHILDREN,
            point.x,point.y,width,panel_height,parent,nullptr,cls.hInstance,this);
        if(!panel) return;
        int pad=int(Metrics::panel_pad*scale),search_height=int(Metrics::search_height*scale);
        panel_font=CreateFontW(-int(std::lround(13*scale)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
        detail_font=CreateFontW(-int(std::lround(12*scale)),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
        search_box=CreateWindowExW(0,L"STATIC",L"文件搜索",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|SS_NOTIFY,
            pad,pad,width-2*pad,search_height,panel,nullptr,cls.hInstance,nullptr);
        SetWindowSubclass(search_box,search_box_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        search_edit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
            0,0,1,1,search_box,reinterpret_cast<HMENU>(1),cls.hInstance,nullptr);
        search_button=CreateWindowExW(0,L"BUTTON",L"搜索文件名或路径",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
            0,0,1,1,search_box,reinterpret_cast<HMENU>(10),cls.hInstance,nullptr);
        search_clear=CreateWindowExW(0,L"BUTTON",L"清除搜索（Esc）",WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,
            0,0,1,1,search_box,reinterpret_cast<HMENU>(11),cls.hInstance,nullptr);
        for(HWND button:{search_button,search_clear}) SetWindowSubclass(button,search_button_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        search_list_host=CreateWindowExW(0,L"STATIC",L"文件列表",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
            0,0,1,1,panel,nullptr,cls.hInstance,nullptr);
        SetWindowSubclass(search_list_host,search_list_host_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        search_list=CreateWindowExW(0,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|
            LBS_OWNERDRAWVARIABLE|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,
            0,0,1,1,search_list_host,reinterpret_cast<HMENU>(2),cls.hInstance,nullptr);
        search_scroll_host=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_CLIPCHILDREN|SS_NOTIFY,
            0,0,1,1,panel,nullptr,cls.hInstance,nullptr);
        search_scroll=CreateWindowExW(0,L"SCROLLBAR",L"",WS_CHILD|WS_VISIBLE|SBS_VERT,
            0,0,1,1,search_scroll_host,nullptr,cls.hInstance,nullptr);
        SetWindowSubclass(search_scroll_host,search_scroll_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(search_scroll,search_scroll_procedure,1,reinterpret_cast<DWORD_PTR>(this));
        SetWindowTextW(search_list,L"已打开标签和最近文件");
        SendMessageW(search_edit,WM_SETFONT,reinterpret_cast<WPARAM>(panel_font),TRUE);
        SendMessageW(search_list,WM_SETFONT,reinterpret_cast<WPARAM>(panel_font),TRUE);
        SendMessageW(search_edit,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
        SendMessageW(search_edit,0x1501,TRUE,reinterpret_cast<LPARAM>(L"搜索文件名或路径")); // EM_SETCUEBANNER
        search_tip=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE,TOOLTIPS_CLASSW,nullptr,
            WS_POPUP|TTS_NOPREFIX|TTS_ALWAYSTIP,0,0,0,0,panel,nullptr,cls.hInstance,nullptr);
        if(search_tip) {
            TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND;
            tool.hwnd=panel;tool.uId=reinterpret_cast<UINT_PTR>(search_list);tool.lpszText=const_cast<wchar_t*>(L"");
            SendMessageW(search_tip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
            SendMessageW(search_tip,TTM_SETDELAYTIME,TTDT_INITIAL,500);
            SendMessageW(search_tip,TTM_SETDELAYTIME,TTDT_RESHOW,100);
            SendMessageW(search_tip,TTM_SETMAXTIPWIDTH,0,int(600*scale));
            SendMessageW(search_tip,WM_SETFONT,reinterpret_cast<WPARAM>(detail_font),FALSE);
            for(HWND button:{search_button,search_clear}) {
                TOOLINFOW button_tool{sizeof(button_tool)};button_tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;button_tool.hwnd=search_box;
                button_tool.uId=reinterpret_cast<UINT_PTR>(button);
                button_tool.lpszText=const_cast<wchar_t*>(button==search_button?L"搜索文件名或路径":L"清除搜索（Esc）");
                SendMessageW(search_tip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&button_tool));
            }
        }
        for(HWND control:{search_edit,search_list}) SetWindowLongPtrW(control,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        edit_proc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(search_edit,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(search_control_procedure)));
        list_proc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(search_list,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(search_control_procedure)));
        SetWindowRgn(panel,CreateRoundRectRgn(0,0,width+1,panel_height+1,int(24*scale),int(24*scale)),TRUE);
        filter_panel();ShowWindow(panel,SW_SHOWNORMAL);SetForegroundWindow(panel);SetFocus(search_edit);
        InvalidateRect(child,nullptr,FALSE);
    }

    void close_panel(bool focus_reader) {
        if(closing_panel) return;
        closing_panel=true;
        HWND old=panel;panel=nullptr;
        if(old) DestroyWindow(old);
        search_box=nullptr;search_button=nullptr;search_clear=nullptr;search_edit=nullptr;search_list_host=nullptr;search_list=nullptr;search_tip=nullptr;search_scroll_host=nullptr;search_scroll=nullptr;
        search_overflow=false;syncing_search_scroll=false;search_scroll_hover=false;search_composing=false;search_ime_key=0;
        search_hot=-1;search_pressed=-1;search_wheel_delta=0;search_tip_text.clear();
        if(panel_font) {DeleteObject(panel_font);panel_font=nullptr;}
        if(detail_font) {DeleteObject(detail_font);detail_font=nullptr;}
        release_search_buffer();
        search_rows.clear();InvalidateRect(child,nullptr,FALSE);
        closing_panel=false;
        if(focus_reader) callback(context,8,0);
    }

    static std::wstring lower(std::wstring value) {
        std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return wchar_t(std::towlower(c));});return value;
    }

    static LRESULT CALLBACK search_list_host_procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data) {
        auto self=reinterpret_cast<Caption*>(data);
        if(message==WM_NCDESTROY) {RemoveWindowSubclass(hwnd,search_list_host_procedure,1);return DefSubclassProc(hwnd,message,wparam,lparam);}
        if(message==WM_DRAWITEM || message==WM_MEASUREITEM || message==WM_CTLCOLORLISTBOX || message==WM_COMMAND)
            return SendMessageW(self->panel,message,wparam,lparam);
        if(message==WM_MOUSEWHEEL) return SendMessageW(self->search_list,message,wparam,lparam);
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }

    void search_escape() {
        if(GetWindowTextLengthW(search_edit)>0) {SetWindowTextW(search_edit,L"");SetFocus(search_edit);}
        else {close_panel(false);SetFocus(child);}
    }

    void focus_search() {
        if(search_composing) return;
        if(panel) {SetForegroundWindow(panel);SetFocus(search_edit);}
        else {suppress_panel_open=false;menu();}
    }

    void search_tab(HWND from) {
        HWND controls[]{search_button,search_edit,search_clear,search_list};
        int current=1;for(int index=0;index<4;++index) if(controls[index]==from) current=index;
        int direction=(GetKeyState(VK_SHIFT)&0x8000)?-1:1;
        for(int step=1;step<=4;++step) {
            int index=(current+direction*step+8)%4;
            if(controls[index]!=search_clear || GetWindowTextLengthW(search_edit)>0) {SetFocus(controls[index]);break;}
        }
    }

    void paint_search_button(DRAWITEMSTRUCT* item) {
        RECT bounds=item->rcItem;Graphics graphics(item->hDC);graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush background(panel_color());graphics.FillRectangle(&background,0,0,int(bounds.right),int(bounds.bottom));
        POINT pointer{};GetCursorPos(&pointer);ScreenToClient(item->hwndItem,&pointer);
        bool highlighted=(item->itemState&(ODS_SELECTED|ODS_FOCUS)) || PtInRect(&bounds,pointer);
        if(highlighted) {
            GraphicsPath shape;rounded(shape,1,1,float(bounds.right-2),float(bounds.bottom-2),6*scale);
            SolidBrush feedback(hover());graphics.FillPath(&feedback,&shape);
        }
        float cx=bounds.right/2.0f,cy=bounds.bottom/2.0f;Pen pen(highlighted_foreground(highlighted),std::max(1.0f,std::round(scale)));
        if(item->CtlID==10) {graphics.DrawEllipse(&pen,cx-6*scale,cy-6*scale,9*scale,9*scale);graphics.DrawLine(&pen,cx+2*scale,cy+2*scale,cx+6*scale,cy+6*scale);}
        else {graphics.DrawLine(&pen,cx-4*scale,cy-4*scale,cx+4*scale,cy+4*scale);graphics.DrawLine(&pen,cx+4*scale,cy-4*scale,cx-4*scale,cy+4*scale);}
    }

    static LRESULT CALLBACK search_box_procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data) {
        auto self=reinterpret_cast<Caption*>(data);
        if(message==WM_NCDESTROY) {RemoveWindowSubclass(hwnd,search_box_procedure,1);return DefSubclassProc(hwnd,message,wparam,lparam);}
        if(message==WM_COMMAND) {
            if(LOWORD(wparam)==1 && HIWORD(wparam)==EN_CHANGE) {self->filter_panel();return 0;}
            if((LOWORD(wparam)==10 || LOWORD(wparam)==11) && HIWORD(wparam)==BN_CLICKED) {
                if(!self->search_composing) {
                    if(LOWORD(wparam)==11) SetWindowTextW(self->search_edit,L"");else self->filter_panel();
                }
                SetFocus(self->search_edit);return 0;
            }
        }
        if(message==WM_DRAWITEM) {self->paint_search_button(reinterpret_cast<DRAWITEMSTRUCT*>(lparam));return TRUE;}
        if(message==WM_CTLCOLOREDIT || message==WM_CTLCOLORSTATIC) return SendMessageW(self->panel,message,wparam,lparam);
        if(message==WM_LBUTTONDOWN) {SetFocus(self->search_edit);return 0;}
        if(message==WM_PAINT) {
            PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);RECT bounds{};GetClientRect(hwnd,&bounds);Graphics graphics(dc);
            auto bg=self->panel_color();SetDCBrushColor(dc,RGB(bg.GetR(),bg.GetG(),bg.GetB()));
            FillRect(dc,&bounds,reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            float stroke=std::max(1.0f,std::round(self->scale));GraphicsPath shape;
            rounded(shape,stroke/2,stroke/2,float(bounds.right)-stroke,float(bounds.bottom)-stroke,12*self->scale);
            HWND focus=GetFocus();bool focused=focus==hwnd || IsChild(hwnd,focus);
            Color border_color=self->dark?(focused?Color(255,120,136,153):Color(255,80,86,95))
                :(focused?Color(255,170,184,200):Color(255,216,220,225));
            Pen border(self->high_contrast?self->foreground():border_color,stroke);graphics.DrawPath(&border,&shape);
            EndPaint(hwnd,&paint);return 0;
        }
        if(message==WM_ERASEBKGND) return 1;
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }

    static LRESULT CALLBACK search_button_procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data) {
        auto self=reinterpret_cast<Caption*>(data);
        if(message==WM_NCDESTROY) {RemoveWindowSubclass(hwnd,search_button_procedure,1);return DefSubclassProc(hwnd,message,wparam,lparam);}
        if(message==WM_MOUSEWHEEL) return SendMessageW(self->search_list,message,wparam,lparam);
        if(message==WM_KEYDOWN) {
            if(wparam==VK_RETURN) {SendMessageW(hwnd,BM_CLICK,0,0);return 0;}
            if(wparam==VK_ESCAPE) {self->search_escape();return 0;}
            if(wparam==VK_TAB) {self->search_tab(hwnd);return 0;}
        }
        if(message==WM_CHAR && (wparam==VK_RETURN || wparam==VK_ESCAPE || wparam==VK_TAB)) return 0;
        if(message==WM_MOUSEMOVE) {TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);}
        if(message==WM_MOUSEMOVE || message==WM_MOUSELEAVE || message==WM_SETFOCUS || message==WM_KILLFOCUS) {
            InvalidateRect(hwnd,nullptr,FALSE);InvalidateRect(self->search_box,nullptr,FALSE);
        }
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }

    static std::wstring display_path(const std::wstring& path) {
        if(path.rfind(L"\\\\?\\UNC\\",0)==0) return L"\\\\"+path.substr(8);
        return path.rfind(L"\\\\?\\",0)==0?path.substr(4):path;
    }

    void update_search_tip(int index) {
        if(!search_tip) return;
        std::wstring detail;
        if(index>=0 && index<int(search_rows.size())) {
            const auto& row=search_rows[index];
            if(row.command==1 || row.command==6) detail=row.detail;
        }
        if(detail==search_tip_text) return;
        if(detail.empty()) SendMessageW(search_tip,TTM_POP,0,0);
        search_tip_text=std::move(detail);
        TOOLINFOW tool{sizeof(tool)};tool.hwnd=panel;tool.uId=reinterpret_cast<UINT_PTR>(search_list);
        tool.lpszText=search_tip_text.data();
        SendMessageW(search_tip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tool));
    }

    int search_row_height(size_t index) const {
        bool heading=index<search_rows.size() && !search_rows[index].command;
        return int(std::lround((heading?Metrics::group_height+(index?Metrics::group_gap:0):Metrics::row_height)*scale));
    }

    void sync_search_scroll() {
        if(!search_list || !search_scroll || syncing_search_scroll) return;
        syncing_search_scroll=true;
        SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};
        bool valid=GetScrollInfo(search_list,SB_VERT,&info)!=FALSE;
        SCROLLINFO previous{sizeof(previous),SIF_RANGE|SIF_PAGE|SIF_POS};
        if(valid && (!GetScrollInfo(search_scroll,SB_CTL,&previous) ||
            info.nMin!=previous.nMin || info.nMax!=previous.nMax || info.nPage!=previous.nPage || info.nPos!=previous.nPos)) {
            // Update native geometry without briefly painting its default appearance.
            SetScrollInfo(search_scroll,SB_CTL,&info,FALSE);
        }
        bool visible=(GetWindowLongPtrW(search_scroll_host,GWL_STYLE)&WS_VISIBLE)!=0;
        if(visible!=(search_overflow && valid)) ShowWindow(search_scroll_host,search_overflow && valid?SW_SHOWNA:SW_HIDE);
        syncing_search_scroll=false;
    }

    void set_search_hot(int hot) {
        if(hot==search_hot) return;
        int previous=search_hot;search_hot=hot;
        for(int index:{previous,hot}) {
            RECT row{};
            if(index>=0 && SendMessageW(search_list,LB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&row))!=LB_ERR)
                InvalidateRect(search_list,&row,FALSE);
        }
    }

    bool caption_shortcut(UINT message,WPARAM key,LPARAM flags) {
        HWND focus=GetFocus();
        if(message!=WM_KEYDOWN || (focus!=parent && focus!=child) || search_composing ||
            !(GetKeyState(VK_CONTROL)&0x8000) || (GetKeyState(VK_MENU)&0x8000)) return false;
        bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        if(key=='A' && shift) {if(!(flags&(1LL<<30))) focus_search();return true;}
        if(key=='B' && !shift) {if(!(flags&(1LL<<30))) callback(context,5,0);return true;}
        if(key=='O' && !shift) {callback(context,3,0);return true;}
        if(key=='W' && !shift) {callback(context,2,active);return true;}
        if(key==VK_TAB || (!shift && (key==VK_PRIOR || key==VK_NEXT))) {
            if(!tabs.empty()) {
                auto found=std::find_if(tabs.begin(),tabs.end(),[this](const Item& item){return item.id==active;});
                size_t current=found==tabs.end()?0:size_t(found-tabs.begin());
                bool previous=key==VK_PRIOR || (key==VK_TAB && shift);
                size_t next=(current+(previous?tabs.size()-1:1))%tabs.size();
                callback(context,1,tabs[next].id);
            }
            return true;
        }
        if(!shift && key>='1' && key<='9') {
            size_t index=key=='9'?tabs.size()-1:size_t(key-'1');
            if(index<tabs.size()) callback(context,1,tabs[index].id);
            return true;
        }
        if(key==VK_ADD || key==VK_OEM_PLUS) {callback(context,10,0);return true;}
        if(key==VK_SUBTRACT || (!shift && key==VK_OEM_MINUS)) {callback(context,11,0);return true;}
        if(key==VK_NUMPAD0 || (!shift && key=='0')) {callback(context,12,0);return true;}
        return false;
    }

    void scroll_search_wheel(WPARAM wparam) {
        UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
        search_wheel_delta+=GET_WHEEL_DELTA_WPARAM(wparam);
        int steps=search_wheel_delta/WHEEL_DELTA;search_wheel_delta%=WHEEL_DELTA;
        if(!steps || !lines) return;
        WPARAM request=lines==WHEEL_PAGESCROLL?(steps>0?SB_PAGEUP:SB_PAGEDOWN):(steps>0?SB_LINEUP:SB_LINEDOWN);
        size_t repeats=std::min(search_rows.size(),size_t(std::abs(steps))*(lines==WHEEL_PAGESCROLL?1:lines));
        // Keep LISTBOX's variable-height and page-scroll rules, but present only
        // the final position of one wheel message. Never suspend the WebView.
        bool batch=repeats>1 && (GetWindowLongPtrW(search_list,GWL_STYLE)&WS_VISIBLE);
        if(batch) CallWindowProcW(list_proc,search_list,WM_SETREDRAW,FALSE,0);
        for(size_t index=0;index<repeats;++index) CallWindowProcW(list_proc,search_list,WM_VSCROLL,request,0);
        if(batch) {
            CallWindowProcW(list_proc,search_list,WM_SETREDRAW,TRUE,0);
            InvalidateRect(search_list,nullptr,FALSE);
        }
        sync_search_scroll();
    }

    void paint_search_scroll(HDC dc) {
        RECT bounds{};GetClientRect(search_scroll,&bounds);
        int hit=int(std::lround(Metrics::scrollbar_hit*scale)),visible=int(std::lround(Metrics::scrollbar_visible*scale));
        int arrow=GetSystemMetricsForDpi(SM_CYVSCROLL,GetDpiForWindow(search_scroll));
        Graphics graphics(dc);graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetClip(Rect((bounds.right-hit)/2,arrow,hit,std::max(0,int(bounds.bottom)-2*arrow)));
        SolidBrush background(panel_color());graphics.FillRectangle(&background,0,0,int(bounds.right),int(bounds.bottom));
        SCROLLBARINFO info{sizeof(info)};
        if(!GetScrollBarInfo(search_scroll,OBJID_CLIENT,&info) || info.xyThumbBottom<=info.xyThumbTop) return;
        POINT top{info.rcScrollBar.left,info.rcScrollBar.top+info.xyThumbTop};
        POINT bottom{info.rcScrollBar.left,info.rcScrollBar.top+info.xyThumbBottom};
        ScreenToClient(search_scroll,&top);ScreenToClient(search_scroll,&bottom);
        float y=float(std::max(arrow,int(top.y))),end=float(std::min(int(bounds.bottom)-arrow,int(bottom.y)));
        if(end<=y) return;
        bool pressed=GetCapture()==search_scroll;
        Color color=high_contrast?foreground():Color(pressed?168:search_scroll_hover?140:82,140,149,159);
        GraphicsPath shape;rounded(shape,float((bounds.right-visible)/2),y,float(visible),end-y,std::min(float(visible),end-y)/2);
        SolidBrush thumb(color);graphics.FillPath(&thumb,&shape);
    }

    // The standard SCROLLBAR retains its own thumb geometry, drag capture and page handling.
    static LRESULT CALLBACK search_scroll_procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data) {
        auto self=reinterpret_cast<Caption*>(data);
        bool was_hovered=self->search_scroll_hover;
        if(message==WM_NCDESTROY) {RemoveWindowSubclass(hwnd,search_scroll_procedure,1);return DefSubclassProc(hwnd,message,wparam,lparam);}
        if(hwnd==self->search_scroll && (message==WM_PAINT || message==WM_PRINTCLIENT)) {
            PAINTSTRUCT paint{};bool begin=message==WM_PAINT && !wparam;
            HDC dc=begin?BeginPaint(hwnd,&paint):reinterpret_cast<HDC>(wparam);
            self->paint_search_scroll(dc);if(begin) EndPaint(hwnd,&paint);return 0;
        }
        if(hwnd==self->search_scroll && message==WM_ERASEBKGND) return 1;
        if(hwnd==self->search_scroll && message==WM_MOUSEMOVE) {
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};MapWindowPoints(hwnd,self->search_scroll_host,&point,1);
            RECT bounds{};GetClientRect(self->search_scroll_host,&bounds);self->search_scroll_hover=PtInRect(&bounds,point)!=FALSE;
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);
        }
        if(hwnd==self->search_scroll && message==WM_MOUSELEAVE) self->search_scroll_hover=false;
        if(hwnd==self->search_scroll_host && message==WM_VSCROLL && reinterpret_cast<HWND>(lparam)==self->search_scroll) {
            self->search_pressed=-1;self->set_search_hot(-1);self->update_search_tip(-1);
            int request=LOWORD(wparam);
            if(request==SB_THUMBTRACK || request==SB_THUMBPOSITION) {
                SCROLLINFO info{sizeof(info),SIF_TRACKPOS};GetScrollInfo(self->search_scroll,SB_CTL,&info);
                SendMessageW(self->search_list,LB_SETTOPINDEX,info.nTrackPos,0);
            } else SendMessageW(self->search_list,WM_VSCROLL,wparam,0);
            self->sync_search_scroll();return 0;
        }
        if(message==WM_MOUSEWHEEL) return SendMessageW(self->search_list,message,wparam,lparam);
        if(message==WM_KEYDOWN) return SendMessageW(self->search_list,message,wparam,lparam);
        if(hwnd==self->search_scroll_host && (message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_MOUSEMOVE)) {
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};MapWindowPoints(hwnd,self->search_scroll,&point,1);
            RECT bounds{};GetClientRect(self->search_scroll,&bounds);point.x=bounds.right/2;
            return SendMessageW(self->search_scroll,message,wparam,MAKELPARAM(point.x,point.y));
        }
        if(hwnd==self->search_scroll_host && (message==WM_PAINT || message==WM_ERASEBKGND || message==WM_CTLCOLORSCROLLBAR)) {
            auto color=self->panel_color();COLORREF rgb=RGB(color.GetR(),color.GetG(),color.GetB());
            if(message==WM_CTLCOLORSCROLLBAR) {SetDCBrushColor(reinterpret_cast<HDC>(wparam),rgb);return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));}
            PAINTSTRUCT paint{};HDC dc=message==WM_PAINT?BeginPaint(hwnd,&paint):reinterpret_cast<HDC>(wparam);
            RECT bounds{};GetClientRect(hwnd,&bounds);SetDCBrushColor(dc,rgb);FillRect(dc,&bounds,reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if(message==WM_PAINT) EndPaint(hwnd,&paint);return message==WM_PAINT?0:1;
        }
        // All geometry setters defer the custom paint; their native redraw flag
        // must stay off. Native mouse capture/tracking still owns interaction.
        if(hwnd==self->search_scroll && message==SBM_SETSCROLLINFO) wparam=FALSE;
        if(hwnd==self->search_scroll && message==SBM_SETPOS) lparam=FALSE;
        auto result=DefSubclassProc(hwnd,message,wparam,lparam);
        if(hwnd==self->search_scroll && (message==SBM_SETSCROLLINFO || message==SBM_SETPOS || message==WM_SIZE))
            InvalidateRect(hwnd,nullptr,FALSE);
        if(hwnd==self->search_scroll && ((message==WM_MOUSEMOVE && (was_hovered!=self->search_scroll_hover || GetCapture()==hwnd)) ||
            (message==WM_MOUSELEAVE && was_hovered) || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_CAPTURECHANGED)) {
            // The native tracking loop can draw synchronously. Retain this
            // immediate replacement only for actual pointer state transitions.
            InvalidateRect(hwnd,nullptr,FALSE);UpdateWindow(hwnd);
        }
        if(hwnd==self->search_scroll && message==WM_LBUTTONUP && self->search_list && GetForegroundWindow()==self->panel) SetFocus(self->search_list);
        return result;
    }

    void size_search_panel() {
        int content=0;for(size_t index=0;index<search_rows.size();++index) content+=search_row_height(index);
        RECT anchor{};GetWindowRect(child,&anchor);
        POINT point{anchor.left+int(Metrics::arrow_left*scale),anchor.bottom+int(6*scale)};
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&monitor);
        int pad=int(Metrics::panel_pad*scale),search_height=int(Metrics::search_height*scale),gap=int(Metrics::group_gap*scale);
        int width=std::min(int(Metrics::panel_width*scale),int(monitor.rcWork.right-monitor.rcWork.left));
        int chrome=2*pad+search_height+gap;
        int panel_height=std::min(chrome+content,std::min(int(Metrics::panel_max*scale),int(monitor.rcWork.bottom-monitor.rcWork.top)));
        int viewport=std::max(0,panel_height-chrome);
        if(content>viewport) {
            int tail_height=0;
            for(size_t remaining=search_rows.size();remaining>0;--remaining) {
                int row=search_row_height(remaining-1);
                if(tail_height+row>viewport) break;
                tail_height+=row;
            }
            // Native listboxes scroll by whole rows; fit the last page without a blank remainder.
            if(tail_height>0) panel_height-=viewport-tail_height;
        }
        point.x=std::clamp(int(point.x),int(monitor.rcWork.left),int(monitor.rcWork.right)-width);
        point.y=std::clamp(int(point.y),int(monitor.rcWork.top),int(monitor.rcWork.bottom)-panel_height);
        SetWindowPos(panel,nullptr,point.x,point.y,width,panel_height,SWP_NOZORDER|SWP_NOACTIVATE);
        int search_width=width-2*pad,button=int(std::lround(28*scale));
        MoveWindow(search_box,pad,pad,search_width,search_height,TRUE);
        MoveWindow(search_button,int(8*scale),(search_height-button)/2,button,button,TRUE);
        MoveWindow(search_clear,search_width-button-int(8*scale),(search_height-button)/2,button,button,TRUE);
        HDC edit_dc=GetDC(search_edit);auto previous_font=SelectObject(edit_dc,panel_font);TEXTMETRICW metrics{};GetTextMetricsW(edit_dc,&metrics);
        SelectObject(edit_dc,previous_font);ReleaseDC(search_edit,edit_dc);
        int edit_height=std::min(search_height-int(4*scale),int(metrics.tmHeight)+2);
        MoveWindow(search_edit,int(44*scale),(search_height-edit_height)/2,search_width-int(88*scale),edit_height,TRUE);
        int list_height=std::max(1,panel_height-chrome),hit_width=int(std::lround(Metrics::scrollbar_hit*scale));
        search_overflow=content>list_height;
        int scroll_left=width-int(std::lround(Metrics::scrollbar_edge*scale))-hit_width;
        int dpi=int(GetDpiForWindow(search_scroll_host));
        int native_width=GetSystemMetricsForDpi(SM_CXVSCROLL,dpi),arrow=GetSystemMetricsForDpi(SM_CYVSCROLL,dpi);
        int list_width=(search_overflow?scroll_left:width-pad)-pad;
        MoveWindow(search_list_host,pad,pad+search_height+gap,list_width,list_height,TRUE);
        MoveWindow(search_list,0,0,list_width+native_width,list_height,TRUE);
        MoveWindow(search_scroll_host,scroll_left,pad+search_height+gap,hit_width,list_height,TRUE);
        int crop_left=(native_width-hit_width)/2;
        MoveWindow(search_scroll,(hit_width-native_width)/2,-arrow,native_width,list_height+2*arrow,TRUE);
        SetWindowRgn(search_scroll,CreateRectRgn(crop_left,arrow,crop_left+hit_width,arrow+list_height),TRUE);
        sync_search_scroll();
        SetWindowRgn(panel,CreateRoundRectRgn(0,0,width+1,panel_height+1,int(24*scale),int(24*scale)),TRUE);
    }

    void filter_panel() {
        if(!search_edit || !search_list) return;
        int length=GetWindowTextLengthW(search_edit);std::wstring query(size_t(length)+1,L'\0');
        GetWindowTextW(search_edit,query.data(),length+1);query.resize(length);query=lower(query);
        ShowWindow(search_clear,length?SW_SHOWNA:SW_HIDE);EnableWindow(search_clear,length>0);
        InvalidateRect(search_box,nullptr,FALSE);
        search_rows.clear();
        auto add_group=[&](const wchar_t* label,const std::vector<Item>& source,unsigned command) {
            bool heading=false;
            for(const auto& item:source) {
                if(!query.empty() && lower(item.title+L" "+item.path).find(query)==std::wstring::npos) continue;
                if(!heading) {search_rows.push_back({label,L"",0,0});heading=true;}
                search_rows.push_back({item.title,display_path(item.path),command,item.id});
            }
        };
        add_group(L"已打开标签页",opened,1);add_group(L"最近打开",recent,6);
        if(search_rows.empty()) search_rows.push_back({L"没有匹配的标签或最近文件",L"",0,0});
        SendMessageW(search_list,WM_SETREDRAW,FALSE,0);SendMessageW(search_list,LB_RESETCONTENT,0,0);
        for(const auto& row:search_rows) {
            std::wstring accessible=row.title+L" "+row.detail;
            if(row.command==1 && row.id==active) accessible=L"当前活动标签 "+accessible;
            SendMessageW(search_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(accessible.c_str()));
        }
        search_hot=-1;search_pressed=-1;search_wheel_delta=0;update_search_tip(-1);size_search_panel();
        select_panel_row(1,true);SendMessageW(search_list,WM_SETREDRAW,TRUE,0);InvalidateRect(search_list,nullptr,TRUE);
    }

    void select_panel_row(int direction,bool first=false) {
        if(search_rows.empty()) return;
        int count=int(search_rows.size()),index=first?(direction>0?-1:count):int(SendMessageW(search_list,LB_GETCURSEL,0,0));
        for(int n=0;n<count;++n) {
            index+=direction;if(index<0 || index>=count) break;
            if(search_rows[index].command) {
                SendMessageW(search_list,LB_SETCURSEL,index,0);
                if(first && direction>0) SendMessageW(search_list,LB_SETTOPINDEX,0,0);
                sync_search_scroll();break;
            }
        }
    }

    void activate_panel_row() {
        int index=int(SendMessageW(search_list,LB_GETCURSEL,0,0));
        if(index<0 || index>=int(search_rows.size()) || !search_rows[index].command) return;
        auto row=search_rows[index];close_panel(false);
        callback(context,row.command,row.id);callback(context,8,0);
    }

    void release_search_buffer() {
        if(search_buffer_dc) {
            SelectObject(search_buffer_dc,search_buffer_original);
            DeleteObject(search_buffer_bitmap);DeleteDC(search_buffer_dc);
        }
        search_buffer_dc=nullptr;search_buffer_bitmap=nullptr;search_buffer_original=nullptr;
        search_buffer_width=0;search_buffer_height=0;
    }

    bool ensure_search_buffer(HDC target,int width,int height) {
        if(width<=0 || height<=0) return false;
        if(search_buffer_dc && width==search_buffer_width && height==search_buffer_height) return true;
        HDC dc=CreateCompatibleDC(target);
        HBITMAP bitmap=dc?CreateCompatibleBitmap(target,width,height):nullptr;
        if(!bitmap) {if(dc) DeleteDC(dc);return false;}
        HGDIOBJ previous=SelectObject(dc,bitmap);
        if(!previous || previous==HGDI_ERROR) {DeleteObject(bitmap);DeleteDC(dc);return false;}
        release_search_buffer();
        search_buffer_dc=dc;search_buffer_bitmap=bitmap;search_buffer_original=previous;
        search_buffer_width=width;search_buffer_height=height;return true;
    }

    void draw_search_row(DRAWITEMSTRUCT* item) {
        if(item->itemID>=search_rows.size()) return;
        const auto& row=search_rows[item->itemID];auto bg=panel_color(),fg=foreground();
        HDC dc=item->hDC;RECT bounds=item->rcItem,client{},clip{};GetClientRect(search_list_host,&client);
        bounds.right=std::min(bounds.right,client.right);
        if(!IntersectRect(&clip,&bounds,&client)) return;
        int saved_dc=SaveDC(dc);IntersectClipRect(dc,clip.left,clip.top,clip.right,clip.bottom);
        Graphics g(dc);g.SetClip(Rect(clip.left,clip.top,clip.right-clip.left,clip.bottom-clip.top));
        SolidBrush back(bg);g.FillRectangle(&back,int(bounds.left),int(bounds.top),int(bounds.right-bounds.left),int(bounds.bottom-bounds.top));
        bool current=row.command==1 && row.id==active;
        bool highlighted=row.command && (current || (item->itemState&ODS_SELECTED) || int(item->itemID)==search_hot);
        if(highlighted) {
            SolidBrush hot(high_contrast?system_color(COLOR_HIGHLIGHT):current?overlay(0x24):hover());g.FillRectangle(&hot,int(bounds.left),int(bounds.top),int(bounds.right-bounds.left),int(bounds.bottom-bounds.top));
        }
        fg=highlighted_foreground(highlighted);
        g.Flush(FlushIntentionSync);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(fg.GetR(),fg.GetG(),fg.GetB()));
        int pad=int(8*scale),icon=int(16*scale);
        if(row.command) {
            if(row.command==1 || row.command==6) {
                HICON file_icon=reinterpret_cast<HICON>(SendMessageW(parent,WM_GETICON,ICON_SMALL,0));
                if(file_icon) DrawIconEx(dc,bounds.left+pad,bounds.top+int(14*scale),file_icon,icon,icon,0,nullptr,DI_NORMAL);
            }
            bounds.left+=pad+icon+int(8*scale);bounds.right-=pad;
            g.Flush(FlushIntentionSync);
            auto old=SelectObject(dc,panel_font);RECT title=bounds;title.top+=int(4*scale);title.bottom=title.top+int(20*scale);
            DrawTextW(dc,row.title.c_str(),int(row.title.size()),&title,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
            SelectObject(dc,detail_font);RECT detail=bounds;detail.top+=int(24*scale);detail.bottom-=int(3*scale);
            auto secondary=high_contrast && highlighted?fg:muted();SetTextColor(dc,RGB(secondary.GetR(),secondary.GetG(),secondary.GetB()));
            DrawTextW(dc,row.detail.c_str(),int(row.detail.size()),&detail,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,old);
        } else {
            bounds.left+=pad;if(item->itemID) bounds.top+=int(Metrics::group_gap*scale);
            auto secondary=muted();SetTextColor(dc,RGB(secondary.GetR(),secondary.GetG(),secondary.GetB()));
            auto old=SelectObject(dc,detail_font);DrawTextW(dc,row.title.c_str(),int(row.title.size()),&bounds,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);SelectObject(dc,old);
        }
        g.Flush(FlushIntentionSync);RestoreDC(dc,saved_dc);
    }

    void paint_search_row(DRAWITEMSTRUCT* item) {
        if(item->itemID>=search_rows.size()) return;
        RECT viewport{},clip{};GetClientRect(search_list_host,&viewport);
        if(!IntersectRect(&clip,&item->rcItem,&viewport)) return;
        if(!ensure_search_buffer(item->hDC,viewport.right,viewport.bottom)) {draw_search_row(item);return;}
        auto buffered=*item;buffered.hDC=search_buffer_dc;draw_search_row(&buffered);
        BitBlt(item->hDC,clip.left,clip.top,clip.right-clip.left,clip.bottom-clip.top,
            search_buffer_dc,clip.left,clip.top,SRCCOPY);
    }

    void paint_search_list(HDC target,const RECT& dirty) {
        RECT viewport{},clip{};GetClientRect(search_list_host,&viewport);
        if(!IntersectRect(&clip,&dirty,&viewport)) return;
        HDC dc=ensure_search_buffer(target,viewport.right,viewport.bottom)?search_buffer_dc:target;
        int saved=SaveDC(dc);IntersectClipRect(dc,clip.left,clip.top,clip.right,clip.bottom);
        auto bg=panel_color();SetDCBrushColor(dc,RGB(bg.GetR(),bg.GetG(),bg.GetB()));
        FillRect(dc,&clip,reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        int selected=int(SendMessageW(search_list,LB_GETCURSEL,0,0));
        int top=int(SendMessageW(search_list,LB_GETTOPINDEX,0,0));
        for(int index=std::max(0,top);index<int(search_rows.size());++index) {
            DRAWITEMSTRUCT item{};item.CtlType=ODT_LISTBOX;item.CtlID=2;item.itemID=UINT(index);
            item.itemAction=ODA_DRAWENTIRE;item.itemState=index==selected?ODS_SELECTED:0;
            item.hwndItem=search_list;item.hDC=dc;
            if(SendMessageW(search_list,LB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&item.rcItem))==LB_ERR || item.rcItem.top>=clip.bottom) break;
            if(item.rcItem.bottom>clip.top) draw_search_row(&item);
        }
        RestoreDC(dc,saved);
        if(dc!=target) BitBlt(target,clip.left,clip.top,clip.right-clip.left,clip.bottom-clip.top,dc,clip.left,clip.top,SRCCOPY);
    }

    static LRESULT CALLBACK search_control_procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
        auto self=reinterpret_cast<Caption*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(!self) return DefWindowProcW(hwnd,message,wparam,lparam);
        WNDPROC original=hwnd==self->search_edit?self->edit_proc:self->list_proc;
        if(hwnd==self->search_list && message==WM_ERASEBKGND) return 1;
        if(hwnd==self->search_list && (message==WM_PAINT || message==WM_PRINTCLIENT)) {
            // Buffer the actual clipped list viewport, including any empty area,
            // rather than just buffering the popup parent around its children.
            PAINTSTRUCT paint{};bool begin=message==WM_PAINT && !wparam;
            HDC dc=begin?BeginPaint(hwnd,&paint):reinterpret_cast<HDC>(wparam);
            RECT dirty{};if(begin) dirty=paint.rcPaint;else GetClientRect(self->search_list_host,&dirty);
            if(dc) self->paint_search_list(dc,dirty);
            if(begin) EndPaint(hwnd,&paint);return 0;
        }
        if(hwnd==self->search_edit) {
            if(message==WM_IME_STARTCOMPOSITION) {
                self->search_composing=true;self->search_ime_key=0;
                SendMessageW(hwnd,0x1501,TRUE,reinterpret_cast<LPARAM>(L"")); // Hide cue during IME preedit.
            }
            if(message==WM_IME_ENDCOMPOSITION) {
                self->search_composing=false;
                self->search_ime_key=(GetKeyState(VK_RETURN)&0x8000)?VK_RETURN:(GetKeyState(VK_ESCAPE)&0x8000)?VK_ESCAPE:0;
                auto result=CallWindowProcW(original,hwnd,message,wparam,lparam);
                SendMessageW(hwnd,0x1501,TRUE,reinterpret_cast<LPARAM>(L"搜索文件名或路径"));
                return result;
            }
            if(message==WM_KEYUP && int(wparam)==self->search_ime_key) self->search_ime_key=0;
            if(self->search_composing && (message==WM_KEYDOWN || message==WM_CHAR)) return CallWindowProcW(original,hwnd,message,wparam,lparam);
            if(self->search_ime_key && (message==WM_KEYDOWN || message==WM_CHAR) && int(wparam)==self->search_ime_key) return 0;
            if(message==WM_SETFOCUS || message==WM_KILLFOCUS) InvalidateRect(self->search_box,nullptr,FALSE);
        }
        if(message==WM_MOUSEWHEEL && hwnd==self->search_edit) return SendMessageW(self->search_list,message,wparam,lparam);
        if(hwnd==self->search_list && (message==WM_VSCROLL || message==WM_MOUSEWHEEL || message==WM_NCLBUTTONDOWN || message==WM_CANCELMODE)) {
            self->search_pressed=-1;self->set_search_hot(-1);self->update_search_tip(-1);
            if(message==WM_MOUSEWHEEL) {self->scroll_search_wheel(wparam);return 0;}
            auto result=CallWindowProcW(original,hwnd,message,wparam,lparam);self->sync_search_scroll();return result;
        }
        if(message==WM_LBUTTONDOWN && hwnd==self->search_list) {
            LRESULT hit=SendMessageW(hwnd,LB_ITEMFROMPOINT,0,lparam);
            self->search_pressed=HIWORD(hit)?-1:LOWORD(hit);
        }
        if(message==WM_KEYDOWN) {
            if(wparam==VK_ESCAPE) {self->search_escape();return 0;}
            if(wparam==VK_RETURN) {self->activate_panel_row();return 0;}
            if(wparam==VK_UP || wparam==VK_DOWN) {self->select_panel_row(wparam==VK_DOWN?1:-1);return 0;}
            if(wparam==VK_HOME || wparam==VK_END) {
                self->select_panel_row(wparam==VK_HOME?1:-1,true);return 0;
            }
            if(wparam==VK_PRIOR || wparam==VK_NEXT) {
                RECT client{};GetClientRect(self->search_list,&client);
                int rows=std::max(1,int(client.bottom/std::max(1L,LONG(Metrics::row_height*self->scale))));
                for(int index=0;index<rows;++index) self->select_panel_row(wparam==VK_NEXT?1:-1);
                return 0;
            }
            if(wparam==VK_TAB) {
                self->search_tab(hwnd);
                return 0;
            }
        }
        if(message==WM_CHAR && (wparam==VK_RETURN || wparam==VK_ESCAPE || wparam==VK_TAB)) return 0;
        if(message==WM_MOUSEMOVE && hwnd==self->search_list) {
            LRESULT hit=SendMessageW(hwnd,LB_ITEMFROMPOINT,0,lparam);
            int hot=HIWORD(hit)?-1:LOWORD(hit);
            self->set_search_hot(hot);
            self->update_search_tip(hot);
            if(self->search_tip) {
                MSG relay{};relay.hwnd=hwnd;relay.message=message;relay.wParam=wparam;relay.lParam=lparam;relay.time=GetMessageTime();GetCursorPos(&relay.pt);
                SendMessageW(self->search_tip,TTM_RELAYEVENT,0,reinterpret_cast<LPARAM>(&relay));
            }
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);
        }
        if(message==WM_MOUSELEAVE && hwnd==self->search_list) {self->set_search_hot(-1);self->update_search_tip(-1);}
        if(message==WM_LBUTTONUP && hwnd==self->search_list) {
            int pressed=self->search_pressed;self->search_pressed=-1;
            auto result=CallWindowProcW(original,hwnd,message,wparam,lparam);
            LRESULT hit=SendMessageW(hwnd,LB_ITEMFROMPOINT,0,lparam);
            size_t index=LOWORD(hit);
            RECT bounds{};POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            bool inside=SendMessageW(hwnd,LB_GETITEMRECT,index,reinterpret_cast<LPARAM>(&bounds))!=LB_ERR && PtInRect(&bounds,point);
            if(HIWORD(hit)==0 && inside && pressed==int(index) && index<self->search_rows.size() && self->search_rows[index].command) {
                SendMessageW(hwnd,LB_SETCURSEL,index,0);self->activate_panel_row();
            }
            return result;
        }
        return CallWindowProcW(original,hwnd,message,wparam,lparam);
    }

    static LRESULT CALLBACK panel_procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
        auto self=reinterpret_cast<Caption*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Caption*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self) return DefWindowProcW(hwnd,message,wparam,lparam);
        if(message==WM_ACTIVATE && LOWORD(wparam)==WA_INACTIVE) {
            POINT point{};GetCursorPos(&point);ScreenToClient(self->child,&point);
            self->suppress_panel_open=(GetAsyncKeyState(VK_LBUTTON)&0x8000) && self->hit(point.x/self->scale,point.y/self->scale)==-2;
            self->close_panel(false);return 0;
        }
        if(message==WM_CLOSE) {self->close_panel(true);return 0;}
        if(message==WM_MOUSEWHEEL) return SendMessageW(self->search_list,message,wparam,lparam);
        if(message==WM_COMMAND && LOWORD(wparam)==1 && HIWORD(wparam)==EN_CHANGE) {self->filter_panel();return 0;}
        if(message==WM_MEASUREITEM) {
            auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);size_t index=item->itemID;
            item->itemHeight=UINT(self->search_row_height(index));return TRUE;
        }
        if(message==WM_DRAWITEM) {
            self->paint_search_row(reinterpret_cast<DRAWITEMSTRUCT*>(lparam));return TRUE;
        }
        if(message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX || message==WM_CTLCOLORSTATIC) {
            auto bg=self->panel_color(),fg=self->foreground();auto dc=reinterpret_cast<HDC>(wparam);
            SetBkColor(dc,RGB(bg.GetR(),bg.GetG(),bg.GetB()));SetTextColor(dc,RGB(fg.GetR(),fg.GetG(),fg.GetB()));
            SetDCBrushColor(dc,RGB(bg.GetR(),bg.GetG(),bg.GetB()));return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        if(message==WM_PAINT) {
            PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);RECT bounds{};GetClientRect(hwnd,&bounds);
            Graphics g(dc);g.SetSmoothingMode(SmoothingModeAntiAlias);g.Clear(self->panel_color());
            // The window region clips the popup to a rounded rect, so the frame must
            // follow the same arc or its corners would be cut away by that region.
            float stroke=std::max(1.0f,std::round(self->scale)),half=stroke/2;
            // Keep the stroke's outer edge on the radius-12 region so nothing is clipped away.
            GraphicsPath frame;rounded(frame,half,half,float(bounds.right)-stroke,float(bounds.bottom)-stroke,
                std::max(half,12*self->scale-half));
            Pen border(self->high_contrast?self->foreground():self->overlay(0x33),stroke);g.DrawPath(&border,&frame);
            g.Flush(FlushIntentionSync);
            EndPaint(hwnd,&paint);return 0;
        }
        if(message==WM_ERASEBKGND) return 1;
        return DefWindowProcW(hwnd,message,wparam,lparam);
    }

    static LRESULT CALLBACK parent_procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data) {
        auto self=reinterpret_cast<Caption*>(data);
        try {
        if(self->caption_shortcut(message,wparam,lparam)) return 0;
        if(message==WM_ACTIVATE || message==WM_SIZE || message==WM_DPICHANGED) {
            self->caption_hover=0;self->caption_pressed=0;
            if(message!=WM_ACTIVATE || LOWORD(wparam)==WA_INACTIVE) self->cancel_drag(false);
            // Owned search popups keep the document active; unrelated focus closes them.
            if(message!=WM_ACTIVATE || (LOWORD(wparam)==WA_INACTIVE && reinterpret_cast<HWND>(lparam)!=self->panel)) self->close_panel(false);
            InvalidateRect(self->child,nullptr,FALSE);
        }
        if(message==WM_THEMECHANGED || message==WM_SETTINGCHANGE) {
            self->close_panel(false);self->theme();InvalidateRect(self->child,nullptr,FALSE);
        }
        } catch(...) { /* Do not propagate C++ exceptions into user32. */ }
        return DefSubclassProc(hwnd,message,wparam,lparam);
    }

    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
        auto self=reinterpret_cast<Caption*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Caption*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); self->child=hwnd;
        }
        if(!self) return DefWindowProcW(hwnd,message,wparam,lparam);
        try {
            if(self->caption_shortcut(message,wparam,lparam)) return 0;
            if(message==WM_PAINT) {self->paint();return 0;}
            if(message==WM_ERASEBKGND) return 1;
            if(message==WM_LBUTTONDOWN) {
                int button=self->caption_button(GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam));
                if(button) {
                    self->cancel_drag();
                    self->caption_pressed=button;self->caption_hover=button;
                    SetCapture(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;
                }
                float x=GET_X_LPARAM(lparam)/self->scale,y=GET_Y_LPARAM(lparam)/self->scale;
                int index=self->hit(x,y);
                self->cancel_drag();
                self->pressed_action=index;
                if(index>=0) {
                    self->pressed_id=self->tabs[index].id;
                    self->drag_title=self->tabs[index].title;
                    self->anchor_x=int((x-self->tab_x(index))*self->scale);
                    self->anchor_y=GET_Y_LPARAM(lparam);
                    float close_x=self->tab_close_x(index);
                    float close_y=(self->height/self->scale+Metrics::tab_top)/2;
                    if(self->tab_close_visible(index) && std::abs(x-close_x)<self->tab_close_size()/2 && std::abs(y-close_y)<self->tab_close_size()/2)
                        self->pressed_action=-4;
                }
                self->down_screen={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
                ClientToScreen(hwnd,&self->down_screen);SetCapture(hwnd);SetFocus(hwnd);
                if(index>=0 && self->pressed_action>=0) self->callback(self->context,1,self->pressed_id);
                return 0;
            }
            if(message==WM_NCHITTEST) {
                POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd,&point);
                // As in SumatraX: only maximize asks for HTMAXBUTTON so Snap
                // Layouts keeps working; min/close use the client mouse path.
                if(self->caption_button(point.x,point.y)==2) return HTMAXBUTTON;
                if(self->caption_button(point.x,point.y)) return HTCLIENT;
                if(!IsZoomed(self->parent)) {
                    int edge=std::max(1,int(std::lround(5*self->scale)));RECT bounds{};GetClientRect(hwnd,&bounds);
                    bool left=point.x<edge,right=point.x>=bounds.right-edge;
                    if(point.y<edge) return left?HTTOPLEFT:right?HTTOPRIGHT:HTTOP;
                    if(left || right) return left?HTLEFT:HTRIGHT;
                }
                // The system caption is collapsed, so the empty band drags the window.
                return self->hit(point.x/self->scale,point.y/self->scale)==-1?HTCAPTION:HTCLIENT;
            }
            if(message==WM_NCMOUSEMOVE) {
                POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd,&point);
                int button=self->caption_button(point.x,point.y);
                self->update_tab_tip(-1,button);
                if(button!=self->caption_hover) {
                    self->caption_hover=button;InvalidateRect(hwnd,nullptr,FALSE);
                }
                if(button) {
                    TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE|TME_NONCLIENT,hwnd,0};TrackMouseEvent(&track);
                }
                return 0;
            }
            if(message==WM_NCMOUSELEAVE) {
                self->update_tab_tip(-1);
                if(self->caption_hover&&!self->caption_pressed) {
                    self->caption_hover=0;InvalidateRect(hwnd,nullptr,FALSE);
                }
                return 0;
            }
            if(message==WM_NCLBUTTONDOWN) {
                POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ScreenToClient(hwnd,&point);
                int button=self->caption_button(point.x,point.y);
                if(button) {
                    self->caption_pressed=button;self->caption_hover=button;
                    SetCapture(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;
                }
                return SendMessageW(self->parent,message,wparam,lparam);
            }
            if(message==WM_NCLBUTTONDBLCLK) return SendMessageW(self->parent,message,wparam,lparam);
            if(message==WM_NCRBUTTONUP && wparam==HTCAPTION) {
                HMENU system=GetSystemMenu(self->parent,FALSE);
                UINT command=TrackPopupMenu(system,TPM_RETURNCMD|TPM_RIGHTBUTTON,GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam),0,self->parent,nullptr);
                if(command) PostMessageW(self->parent,WM_SYSCOMMAND,command,0);return 0;
            }
            if(message==WM_NCLBUTTONUP) {
                POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ScreenToClient(hwnd,&point);
                int button=self->caption_button(point.x,point.y);
                int pressed=self->caption_pressed;
                self->caption_pressed=0;
                if(GetCapture()==hwnd) ReleaseCapture();
                if(pressed) {
                    if(button==pressed) self->activate_caption_button(pressed);
                    self->caption_hover=button;
                    InvalidateRect(hwnd,nullptr,FALSE);
                    return 0;
                }
            }
            if(message==WM_MOUSEMOVE) {
                int button=self->caption_button(GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam));
                if(button||self->caption_hover||self->caption_pressed) {
                    int before=self->caption_hover;
                    self->caption_hover=button;
                    if(!self->caption_pressed) {
                        TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track);
                    }
                    if(before!=button) InvalidateRect(hwnd,nullptr,FALSE);
                }
                if(button && GetCapture()!=hwnd) {self->update_tab_tip(-1,button);return 0;}
                float x=GET_X_LPARAM(lparam)/self->scale,y=GET_Y_LPARAM(lparam)/self->scale;
                if(self->pressed_id && self->pressed_action>=0 && GetCapture()==hwnd) {
                    if(!(wparam&MK_LBUTTON)) {self->cancel_drag();return 0;}
                    POINT screen{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ClientToScreen(hwnd,&screen);
                    UINT dpi=GetDpiForWindow(hwnd);
                    if(!self->dragging && (std::abs(screen.x-self->down_screen.x)>=GetSystemMetricsForDpi(SM_CXDRAG,dpi)
                        || std::abs(screen.y-self->down_screen.y)>=GetSystemMetricsForDpi(SM_CYDRAG,dpi))) self->start_drag(screen);
                    if(self->dragging) {self->update_drag(&screen);return 0;}
                }
                self->hovered=self->hit(x,y);
                self->update_tab_tip(self->hovered);
                self->close_hover=self->hovered>=0 &&
                    self->tab_close_visible(self->hovered) &&
                    std::abs(x-self->tab_close_x(self->hovered))<self->tab_close_size()/2 &&
                    std::abs(y-(self->height/self->scale+Metrics::tab_top)/2)<self->tab_close_size()/2;
                TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track);
                InvalidateRect(hwnd,nullptr,FALSE); return 0;
            }
            if(message==WM_MOUSELEAVE) {
                self->hovered=-1;self->caption_hover=0;
                self->update_tab_tip(-1);
                InvalidateRect(hwnd,nullptr,FALSE);return 0;
            }
            if(message==WM_TIMER && wparam==1) {
                if(self->dragging && (GetAsyncKeyState(VK_ESCAPE)&0x8000)) self->cancel_drag();
                else self->arrange_tabs();
                return 0;
            }
            if(message==WM_CANCELMODE || message==WM_CAPTURECHANGED ||
                (message==WM_KEYDOWN && wparam==VK_ESCAPE)) {
                self->caption_pressed=0;self->caption_hover=0;
                self->cancel_drag();return 0;
            }
            if(message==WM_NCDESTROY) {
                self->cancel_drag();SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);
                return DefWindowProcW(hwnd,message,wparam,lparam);
            }
            if(message==WM_MOUSEWHEEL) {
                self->scroll-=GET_WHEEL_DELTA_WPARAM(wparam)/120.0f*120;self->layout();return 0;
            }
            if(message==WM_LBUTTONUP) {
                if(self->dragging) {
                    POINT screen{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ClientToScreen(hwnd,&screen);
                    self->finish_drag(screen);return 0;
                }
                if(self->caption_pressed) {
                    int pressed=self->caption_pressed;
                    self->caption_pressed=0;
                    if(GetCapture()==hwnd) ReleaseCapture();
                    if(self->caption_button(GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam))==pressed)
                        self->activate_caption_button(pressed);
                    self->caption_hover=0;
                    InvalidateRect(hwnd,nullptr,FALSE);
                    return 0;
                }
                float x=GET_X_LPARAM(lparam)/self->scale,y=GET_Y_LPARAM(lparam)/self->scale;
                int index=self->hit(x,y);
                int action=self->pressed_action;auto document=self->pressed_id;
                self->cancel_drag();
                if(action==-2 && index==-2) self->menu();
                else if(action==-3 && index==-3) self->callback(self->context,3,0);
                else if(action==-4 && index>=0 && self->tabs[index].id==document)
                    self->callback(self->context,2,document);
                else if(action>=0 && index>=0 && self->tabs[index].id==document)
                    self->callback(self->context,1,document);
                return 0;
            }
            if(message==WM_MBUTTONUP) {
                int index=self->hit(GET_X_LPARAM(lparam)/self->scale,GET_Y_LPARAM(lparam)/self->scale);
                if(index>=0 && !self->dragging) self->callback(self->context,2,self->tabs[index].id);
                return 0;
            }
        } catch(...) { /* Never unwind through a native window procedure. */ }
        return DefWindowProcW(hwnd,message,wparam,lparam);
    }
};

extern "C" {
void* md_caption_create(HWND parent,Callback callback,void* context,long* error) noexcept {
    try {
        static HRESULT bootstrap=MddBootstrapInitialize(0x00010008,nullptr,PACKAGE_VERSION{});
        winrt::check_hresult(bootstrap);
        static ULONG_PTR gdiplus=[] {ULONG_PTR token{};GdiplusStartupInput input;GdiplusStartup(&token,&input,nullptr);return token;}();
        (void)gdiplus;
        auto self=std::make_unique<Caption>();self->parent=parent;self->callback=callback;self->context=context;
        self->app=AppWindow::GetFromWindowId(winrt::Microsoft::UI::GetWindowIdFromWindow(parent));
        self->app.TitleBar().ExtendsContentIntoTitleBar(true);
        // Collapsed removes Windows' own 32-DIP caption buttons so the 40-DIP
        // band can carry SumatraX-style buttons that fill the full height.
        self->app.TitleBar().PreferredHeightOption(TitleBarHeightOption::Collapsed);
        WNDCLASSW cls{};cls.lpfnWndProc=Caption::procedure;cls.hInstance=GetModuleHandleW(nullptr);
        cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));cls.lpszClassName=L"MarkdownViewer.NativeTallTabs";
        RegisterClassW(&cls);
        self->child=CreateWindowExW(0,cls.lpszClassName,L"文档标签栏",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
            0,0,1,1,parent,nullptr,cls.hInstance,self.get());
        if(!self->child) winrt::throw_last_error();
        if(!SetWindowSubclass(parent,Caption::parent_procedure,reinterpret_cast<UINT_PTR>(self.get()),reinterpret_cast<DWORD_PTR>(self.get()))) winrt::throw_last_error();
        self->theme();self->layout();captions.push_back(self.get());*error=0;return self.release();
    }catch(const winrt::hresult_error& e){*error=e.code();return nullptr;}
    catch(...){*error=E_FAIL;return nullptr;}
}
void md_caption_destroy(Caption* self) noexcept {
    if(self){RemoveWindowSubclass(self->parent,Caption::parent_procedure,reinterpret_cast<UINT_PTR>(self));self->close_panel(false);self->restore_frame();self->cancel_drag();captions.erase(std::remove(captions.begin(),captions.end(),self),captions.end());
        if(self->tab_tip) DestroyWindow(self->tab_tip);DestroyWindow(self->child);delete self;}
}
int md_caption_resize(Caption* self) noexcept {
    try{self->layout();return self->height;}catch(...){return self->height;}
}
void md_caption_search(Caption* self) noexcept {
    try{self->focus_search();}catch(...){}
}
void md_caption_begin(Caption* self,unsigned long long active) noexcept {
    self->active=active;self->tabs.clear();self->opened.clear();self->recent.clear();
}
void md_caption_add(Caption* self,unsigned kind,unsigned long long id,const wchar_t* title,const wchar_t* path) noexcept {
    try {
        Item item{id,title?title:L"",path?path:L""};
        (kind==0?self->tabs:kind==1?self->opened:self->recent).push_back(std::move(item));
    }catch(...){}
}
void md_caption_commit(Caption* self) noexcept {
    try {
        if (self->drop_slot >= 0 && self->pending_drop &&
            std::any_of(self->tabs.begin(),self->tabs.end(),
                [self](const Item& item){return item.id==self->pending_drop;})) {
            float x=self->incoming_x;
            self->tab_motion[self->pending_drop]={x,x,0};
            self->drop_slot=-1;
        }
        for(auto it=self->tab_motion.begin();it!=self->tab_motion.end();) {
            if(std::none_of(self->tabs.begin(),self->tabs.end(),[&](const Item& item){return item.id==it->first;})) it=self->tab_motion.erase(it);
            else ++it;
        }
        if(self->pressed_id && std::none_of(self->tabs.begin(),self->tabs.end(),
            [self](const Item& item){return item.id==self->pressed_id;})) self->cancel_drag();
        self->theme();self->layout();
        for(size_t i=0;i<self->tabs.size();++i) if(self->tabs[i].id==self->active) {
            float left=i*self->tab_width,right=left+self->tab_width;
            if(left<self->scroll)self->scroll=left;
            if(right>self->scroll+self->viewport)self->scroll=right-self->viewport;
            break;
        }
        self->layout();
    }catch(...){}
}
void md_caption_finish_drop(Caption* self, unsigned long long document) noexcept {
    try { self->finish_drop(document); } catch(...) {}
}
bool md_caption_take_drop(unsigned long long token,TabDrop* drop) noexcept {
    auto item=pending_drops.find(token);
    if(item==pending_drops.end()) return false;
    *drop=item->second;pending_drops.erase(item);return true;
}
}
