#pragma once
// Shared UIKit / CoreGraphics helpers (armv7: CGFloat is a float).
#include "objc/ObjC.h"
#include <cstdint>
#include <string>
#include <vector>

namespace uikit {
struct Rect { float x = 0, y = 0, w = 0, h = 0; };
Rect rectAt(int word);                                       // a CGRect argument starting at argument word `word`
void writeRect(std::uint64_t address, const Rect &r);
void writePoint(std::uint64_t address, float x, float y);  // also CGSize
void retRect(const Rect &r);                                 // into the stret result
void retPoint(float x, float y);                             // CGPoint / CGSize into the stret result
std::vector<std::uint64_t> rectWords(const Rect &r);         // a CGRect as send() argument words
bool colorComponents(objc::id color, float rgba[4]);         // UIColor / CGColor components
void setFillColor(const float rgba[4]);                      // the current UIGraphics context's fill colour
// Draws a white box with a line of text into the window (pixels, top-left origin); dim draws placeholder grey.
void drawOverlayBox(int x0, int y0, int x1, int y1, const std::u16string &text, bool dim);
} // namespace uikit
