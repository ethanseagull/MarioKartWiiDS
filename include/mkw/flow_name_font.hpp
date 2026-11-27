#pragma once
#include <mkw/race_flow_layout.hpp>
#include <mkw/renderer.hpp>
#include <array>
#include <cstdint>

namespace mkw {
// Result and award boards: a Mii racer's row shows its Mii's name (CtrlRaceResult
// FillName) instead of "Mii". DIAG A/B: no-p4-hud.txt.
inline bool flowMiiNames=true;
// One name in kart_kanji_font, the boards' own font: its glyphs come from the
// menus' name glyph pages (tools/cook_menu_names.py, every glyph of the font),
// copied into a small LA4 atlas when the boards load, so any character a name
// can hold draws without the boards' cooked subset holding it. Metrics follow
// the boards' font where it has the glyph, else the menus' glyph width.
class FlowNameFont {
public:
 // Builds the atlas for `utf8` (glyphs the font lacks are left out); `like`
 // gives the cell metrics (the font the name replaces), `slot` the texture id
 // the font's batches carry. False (and nothing kept) when no glyph is found.
 bool build(const char* utf8,const FlowFont& like,int slot);
 void reset();
 bool ready()const{return texture.ready;}
 const FlowFont& font()const{return font_;}
 const char* text()const{return text_;}
 Texture texture;
private:
 std::array<FlowGlyph,32> glyphs_{};FlowFont font_{};char text_[64]{};
};
}
