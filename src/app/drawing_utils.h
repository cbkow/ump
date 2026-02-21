#pragma once

// ============================================================================
// Raster drawing utilities for image buffers
// ============================================================================

// Draw a line on an RGBA image buffer (Bresenham's algorithm)
void DrawLineOnImage(unsigned char* image, int width, int height,
                     int x0, int y0, int x1, int y1,
                     unsigned char r, unsigned char g, unsigned char b, unsigned char a, int thickness = 2);

// Draw a rectangle on an RGBA image buffer
void DrawRectangleOnImage(unsigned char* image, int width, int height,
                          int x0, int y0, int x1, int y1,
                          unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                          int thickness = 2, bool filled = false);

// Draw an oval/ellipse on an RGBA image buffer (midpoint algorithm)
void DrawOvalOnImage(unsigned char* image, int width, int height,
                     int x0, int y0, int x1, int y1,
                     unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                     int thickness = 2, bool filled = false);

// Draw an arrow on an RGBA image buffer
void DrawArrowOnImage(unsigned char* image, int width, int height,
                      int x0, int y0, int x1, int y1,
                      unsigned char r, unsigned char g, unsigned char b, unsigned char a, int thickness = 2);
