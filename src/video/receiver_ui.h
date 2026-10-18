#pragma once

#include "imgui.h"
#include <algorithm>
#include <cmath>

// Vector controls use the same renderer as the video; no browser or bitmap icons.
namespace ap::video::receiver_ui {
constexpr ImU32 ink = IM_COL32(32, 38, 41, 255);
constexpr ImU32 muted = IM_COL32(115, 123, 128, 255);
constexpr ImU32 blue = IM_COL32(36, 103, 236, 255);
constexpr ImU32 line = IM_COL32(230, 232, 231, 255);
constexpr ImU32 paper = IM_COL32(245, 246, 244, 255);
enum class Icon { Cast, Home, Music, Screen, Settings, Phone, Previous, Next, Play, Pause, Close, Minimize, Maximize, Fullscreen, Record };

inline void icon(ImDrawList* d, Icon kind, ImVec2 p, float size, ImU32 color) {
    auto v = [&](float x, float y) { return ImVec2(p.x + x * size / 24, p.y + y * size / 24); };
    auto stroke = [&](float x, float y, float xx, float yy) { d->AddLine(v(x,y), v(xx,yy), color, 1.6f); };
    switch (kind) {
    case Icon::Cast:
        stroke(3,15,3,5); stroke(3,5,21,5); stroke(21,5,21,15); stroke(3,15,8,15); stroke(16,15,21,15);
        stroke(12,12,6,20); stroke(6,20,18,20); stroke(18,20,12,12); break;
    case Icon::Home:
        stroke(3,10,12,3); stroke(12,3,21,10); stroke(5,9,5,21); stroke(5,21,10,21);
        stroke(10,21,10,15); stroke(10,15,14,15); stroke(14,15,14,21); stroke(14,21,19,21); stroke(19,21,19,9); break;
    case Icon::Music:
        stroke(9,18,9,5); stroke(9,5,20,3); stroke(20,3,20,16); stroke(9,8,20,6);
        d->AddEllipse(v(6,18), ImVec2(size/8,size/10), color, 0, 20, 1.6f);
        d->AddEllipse(v(17,16), ImVec2(size/8,size/10), color, 0, 20, 1.6f); break;
    case Icon::Screen:
        d->AddRect(v(2,3),v(22,17),color,2,0,1.6f); stroke(12,17,12,21); stroke(8,21,16,21); break;
    case Icon::Settings: {
        // A single closed outline avoids the overlaps and gaps produced by
        // drawing separate teeth on top of a circle at small icon sizes.
        ImVec2 outline[40];
        constexpr float angles[]={-22.5f,-15.f,-11.f,11.f,15.f};
        constexpr float radii[]={7.5f,7.5f,10.f,10.f,7.5f};
        for (int i=0; i<8; ++i) {
            for (int j=0;j<5;++j) {
                const float a=(i*45+angles[j])*3.14159265f/180;
                outline[i*5+j]=v(12+radii[j]*std::cos(a),12+radii[j]*std::sin(a));
            }
        }
        d->AddPolyline(outline,40,color,ImDrawFlags_Closed,1.5f);
        d->AddCircle(v(12,12),size*.13f,color,24,1.5f);
        break;
    }
    case Icon::Phone:
        d->AddRect(v(6,2),v(18,22),color,3,0,1.6f); stroke(10,5,14,5); stroke(11,19,13,19); break;
    case Icon::Previous:
        d->AddTriangleFilled(v(18,5),v(8,12),v(18,19),color); stroke(5,5,5,19); break;
    case Icon::Next:
        d->AddTriangleFilled(v(6,5),v(16,12),v(6,19),color); stroke(19,5,19,19); break;
    case Icon::Play: d->AddTriangleFilled(v(9,5),v(20,12),v(9,19),color); break;
    case Icon::Pause: d->AddRectFilled(v(7,5),v(10,19),color,1); d->AddRectFilled(v(14,5),v(17,19),color,1); break;
    case Icon::Close: stroke(7,7,17,17); stroke(17,7,7,17); break;
    case Icon::Minimize: stroke(7,12,17,12); break;
    case Icon::Maximize: d->AddRect(v(7,7),v(17,17),color,0,0,1.2f); break;
    case Icon::Fullscreen:
        stroke(3,9,3,3); stroke(3,3,9,3); stroke(15,3,21,3); stroke(21,3,21,9);
        stroke(3,15,3,21); stroke(3,21,9,21); stroke(15,21,21,21); stroke(21,21,21,15); break;
    case Icon::Record: d->AddCircleFilled(v(12,12),size*.18f,IM_COL32(205,92,92,255)); break;
    }
}

inline bool surface(const char* id, ImVec2 size) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));
    const bool clicked = ImGui::Button(id, size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return clicked;
}

inline bool icon_button(const char* id, Icon kind, const char* tooltip, float size=35, bool primary=false) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = surface(id, ImVec2(size,size));
    auto* d = ImGui::GetWindowDrawList();
    const bool hover = ImGui::IsItemHovered();
    const bool disabled = ImGui::GetStyle().Alpha < .9f;
    ImU32 color = disabled ? IM_COL32(160,168,162,255) : IM_COL32(83,97,88,255);
    if (primary) {
        d->AddCircleFilled(ImVec2(p.x+size/2,p.y+size/2),size/2, disabled ? IM_COL32(174,183,176,255) : hover ? IM_COL32(59,81,68,255) : IM_COL32(40,56,47,255),48);
        color = IM_COL32(255,255,255,255);
    } else if (hover) d->AddRectFilled(p,ImVec2(p.x+size,p.y+size),IM_COL32(235,239,235,255),8);
    icon(d,kind,ImVec2(p.x+(size-21)/2,p.y+(size-21)/2),21,color);
    if (hover && tooltip) ImGui::SetTooltip("%s",tooltip);
    return clicked;
}

inline bool nav(const char* id, const char* label, Icon kind, bool selected, float width, ImFont* font) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = surface(id, ImVec2(width,44));
    auto* d = ImGui::GetWindowDrawList();
    if (selected || ImGui::IsItemHovered())
        d->AddRectFilled(p,ImVec2(p.x+width,p.y+44),selected ? IM_COL32(238,243,255,255) : IM_COL32(243,245,243,255),9);
    const ImU32 color = selected ? IM_COL32(35,95,207,255) : IM_COL32(116,124,120,255);
    icon(d,kind,ImVec2(p.x+12,p.y+12),20,color);
    d->AddText(font,font->FontSize,ImVec2(p.x+44,p.y+(44-font->FontSize)/2),color,label);
    return clicked;
}

inline bool text_button(const char* label, ImVec2 size=ImVec2(0,34)) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,1);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,7);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(1,1,1,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.98f,.99f,.98f,1));
    const bool pressed = ImGui::Button(label,size);
    ImGui::PopStyleColor(2); ImGui::PopStyleVar(2);
    return pressed;
}
} // namespace ap::video::receiver_ui
