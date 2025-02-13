/*
 *	Copyright (C) 2003-2006 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include "Rasterizer.h"
#include "SeparableFilter.h"
#include "OverlayMix.h"

#ifndef _MAX	/* avoid collision with common (nonconforming) macros */
#define _MAX	(max)
#define _MIN	(min)
#define _IMPL_MAX max
#define _IMPL_MIN min
#else
#define _IMPL_MAX _MAX
#define _IMPL_MIN _MIN
#endif

int Rasterizer::getOverlayWidth()
{
    return mOverlayWidth * 8;
}

Rasterizer::Rasterizer() : mpPathTypes(NULL), mpPathPoints(NULL), mPathPoints(0), mpOverlayBuffer(NULL)
{
    mOverlayWidth = mOverlayHeight = 0;
    mPathOffsetX = mPathOffsetY = 0;
    mOffsetX = mOffsetY = 0;
}

Rasterizer::~Rasterizer()
{
    _TrashPath();
    _TrashOverlay();
}

void Rasterizer::_TrashPath()
{
    free(mpPathTypes);
    free(mpPathPoints);
    mpPathTypes = NULL;
    mpPathPoints = NULL;
    mPathPoints = 0;
}

void Rasterizer::_TrashOverlay()
{
    delete [] mpOverlayBuffer;
    mpOverlayBuffer = NULL;
}

void Rasterizer::_ReallocEdgeBuffer(int edges)
{
    mEdgeHeapSize = edges;
    mpEdgeBuffer = (Edge*)realloc(mpEdgeBuffer, sizeof(Edge) * edges);
}

void Rasterizer::_EvaluateBezier(int ptbase, bool fBSpline)
{
    const POINT* pt0 = mpPathPoints + ptbase;
    const POINT* pt1 = mpPathPoints + ptbase + 1;
    const POINT* pt2 = mpPathPoints + ptbase + 2;
    const POINT* pt3 = mpPathPoints + ptbase + 3;

    double x0 = pt0->x;
    double x1 = pt1->x;
    double x2 = pt2->x;
    double x3 = pt3->x;
    double y0 = pt0->y;
    double y1 = pt1->y;
    double y2 = pt2->y;
    double y3 = pt3->y;

    double cx3, cx2, cx1, cx0, cy3, cy2, cy1, cy0;

    if(fBSpline)
    {
        // 1   [-1 +3 -3 +1]
        // - * [+3 -6 +3  0]
        // 6   [-3  0 +3  0]
        //	   [+1 +4 +1  0]

        double _1div6 = 1.0 / 6.0;

        cx3 = _1div6 * (-  x0 + 3 * x1 - 3 * x2 + x3);
        cx2 = _1div6 * (3 * x0 - 6 * x1 + 3 * x2);
        cx1 = _1div6 * (-3 * x0	   + 3 * x2);
        cx0 = _1div6 * (x0 + 4 * x1 + 1 * x2);

        cy3 = _1div6 * (-  y0 + 3 * y1 - 3 * y2 + y3);
        cy2 = _1div6 * (3 * y0 - 6 * y1 + 3 * y2);
        cy1 = _1div6 * (-3 * y0     + 3 * y2);
        cy0 = _1div6 * (y0 + 4 * y1 + 1 * y2);
    }
    else // bezier
    {
        // [-1 +3 -3 +1]
        // [+3 -6 +3  0]
        // [-3 +3  0  0]
        // [+1  0  0  0]

        cx3 = -  x0 + 3 * x1 - 3 * x2 + x3;
        cx2 =  3 * x0 - 6 * x1 + 3 * x2;
        cx1 = -3 * x0 + 3 * x1;
        cx0 =    x0;

        cy3 = -  y0 + 3 * y1 - 3 * y2 + y3;
        cy2 =  3 * y0 - 6 * y1 + 3 * y2;
        cy1 = -3 * y0 + 3 * y1;
        cy0 =    y0;
    }

    //
    // This equation is from Graphics Gems I.
    //
    // The idea is that since we're approximating a cubic curve with lines,
    // any error we incur is due to the curvature of the line, which we can
    // estimate by calculating the maximum acceleration of the curve.  For
    // a cubic, the acceleration (second derivative) is a line, meaning that
    // the absolute maximum acceleration must occur at either the beginning
    // (|c2|) or the end (|c2+c3|).  Our bounds here are a little more
    // conservative than that, but that's okay.
    //
    // If the acceleration of the parametric formula is zero (c2 = c3 = 0),
    // that component of the curve is linear and does not incur any error.
    // If a=0 for both X and Y, the curve is a line segment and we can
    // use a step size of 1.

    double maxaccel1 = fabs(2 * cy2) + fabs(6 * cy3);
    double maxaccel2 = fabs(2 * cx2) + fabs(6 * cx3);

    double maxaccel = maxaccel1 > maxaccel2 ? maxaccel1 : maxaccel2;
    double h = 1.0;

    if(maxaccel > 8.0) h = sqrt(8.0 / maxaccel);

    if(!fFirstSet)
    {
        firstp.x = (LONG)cx0;
        firstp.y = (LONG)cy0;
        lastp = firstp;
        fFirstSet = true;
    }

    for(double t = 0; t < 1.0; t += h)
    {
        double x = cx0 + t * (cx1 + t * (cx2 + t * cx3));
        double y = cy0 + t * (cy1 + t * (cy2 + t * cy3));
        _EvaluateLine(lastp.x, lastp.y, (int)x, (int)y);
    }

    double x = cx0 + cx1 + cx2 + cx3;
    double y = cy0 + cy1 + cy2 + cy3;
    _EvaluateLine(lastp.x, lastp.y, (int)x, (int)y);
}

void Rasterizer::_EvaluateLine(int pt1idx, int pt2idx)
{
    const POINT* pt1 = mpPathPoints + pt1idx;
    const POINT* pt2 = mpPathPoints + pt2idx;

    _EvaluateLine(pt1->x, pt1->y, pt2->x, pt2->y);
}

void Rasterizer::_EvaluateLine(int x0, int y0, int x1, int y1)
{
    if(lastp.x != x0 || lastp.y != y0)
    {
        _EvaluateLine(lastp.x, lastp.y, x0, y0);
    }

    if(!fFirstSet)
    {
        firstp.x = x0;
        firstp.y = y0;
        fFirstSet = true;
    }
    lastp.x = x1;
    lastp.y = y1;

    if(y1 > y0)	// down
    {
        __int64 xacc = (__int64)x0 << 13;

        // prestep y0 down

        int dy = y1 - y0;
        int y = ((y0 + 3)&~7) + 4;
        int iy = y >> 3;

        y1 = (y1 - 5) >> 3;

        if(iy <= y1)
        {
            __int64 invslope = (__int64(x1 - x0) << 16) / dy;

            while(mEdgeNext + y1 + 1 - iy > mEdgeHeapSize)
                _ReallocEdgeBuffer(mEdgeHeapSize * 2);

            xacc += (invslope * (y - y0)) >> 3;

            while(iy <= y1)
            {
                int ix = (int)((xacc + 32768) >> 16);

                mpEdgeBuffer[mEdgeNext].next = mpScanBuffer[iy];
                mpEdgeBuffer[mEdgeNext].posandflag = ix * 2 + 1;

                mpScanBuffer[iy] = mEdgeNext++;

                ++iy;
                xacc += invslope;
            }
        }
    }
    else if(y1 < y0) // up
    {
        __int64 xacc = (__int64)x1 << 13;

        // prestep y1 down

        int dy = y0 - y1;
        int y = ((y1 + 3)&~7) + 4;
        int iy = y >> 3;

        y0 = (y0 - 5) >> 3;

        if(iy <= y0)
        {
            __int64 invslope = (__int64(x0 - x1) << 16) / dy;

            while(mEdgeNext + y0 + 1 - iy > mEdgeHeapSize)
                _ReallocEdgeBuffer(mEdgeHeapSize * 2);

            xacc += (invslope * (y - y1)) >> 3;

            while(iy <= y0)
            {
                int ix = (int)((xacc + 32768) >> 16);

                mpEdgeBuffer[mEdgeNext].next = mpScanBuffer[iy];
                mpEdgeBuffer[mEdgeNext].posandflag = ix * 2;

                mpScanBuffer[iy] = mEdgeNext++;

                ++iy;
                xacc += invslope;
            }
        }
    }
}

bool Rasterizer::BeginPath(HDC hdc)
{
    _TrashPath();

    return !!::BeginPath(hdc);
}

bool Rasterizer::EndPath(HDC hdc)
{
    ::CloseFigure(hdc);

    if(::EndPath(hdc))
    {
        mPathPoints = GetPath(hdc, NULL, NULL, 0);

        if(!mPathPoints)
            return true;

        mpPathTypes = (BYTE*)malloc(sizeof(BYTE) * mPathPoints);
        mpPathPoints = (POINT*)malloc(sizeof(POINT) * mPathPoints);

        if(mPathPoints == GetPath(hdc, mpPathPoints, mpPathTypes, mPathPoints))
            return true;
    }

    ::AbortPath(hdc);

    return false;
}

bool Rasterizer::PartialBeginPath(HDC hdc, bool bClearPath)
{
    if(bClearPath)
        _TrashPath();

    return !!::BeginPath(hdc);
}

bool Rasterizer::PartialEndPath(HDC hdc, long dx, long dy)
{
    ::CloseFigure(hdc);

    if(::EndPath(hdc))
    {
        int nPoints;
        BYTE* pNewTypes;
        POINT* pNewPoints;

        nPoints = GetPath(hdc, NULL, NULL, 0);

        if(!nPoints)
            return true;

        pNewTypes = (BYTE*)realloc(mpPathTypes, (mPathPoints + nPoints) * sizeof(BYTE));
        pNewPoints = (POINT*)realloc(mpPathPoints, (mPathPoints + nPoints) * sizeof(POINT));

        if(pNewTypes)
            mpPathTypes = pNewTypes;

        if(pNewPoints)
            mpPathPoints = pNewPoints;

        BYTE* pTypes = DNew BYTE[nPoints];
        POINT* pPoints = DNew POINT[nPoints];

        if(pNewTypes && pNewPoints && nPoints == GetPath(hdc, pPoints, pTypes, nPoints))
        {
            for(ptrdiff_t i = 0; i < nPoints; ++i)
            {
                mpPathPoints[mPathPoints + i].x = pPoints[i].x + dx;
                mpPathPoints[mPathPoints + i].y = pPoints[i].y + dy;
                mpPathTypes[mPathPoints + i] = pTypes[i];
            }

            mPathPoints += nPoints;

            delete[] pTypes;
            delete[] pPoints;
            return true;
        }
        else
            DebugBreak();

        delete[] pTypes;
        delete[] pPoints;
    }

    ::AbortPath(hdc);

    return false;
}

bool Rasterizer::ScanConvert()
{
    size_t lastmoveto = -1;
    size_t i;

    // Drop any outlines we may have.

    mOutline.clear();
    mWideOutline.clear();
    mWideBorder = 0;

    // Determine bounding box

    if(!mPathPoints)
    {
        mPathOffsetX = mPathOffsetY = 0;
        mWidth = mHeight = 0;
        return 0;
    }

    ptrdiff_t minx = INT_MAX;
    ptrdiff_t miny = INT_MAX;
    ptrdiff_t maxx = INT_MIN;
    ptrdiff_t maxy = INT_MIN;

    for(i = 0; i < mPathPoints; ++i)
    {
        ptrdiff_t ix = mpPathPoints[i].x;
        ptrdiff_t iy = mpPathPoints[i].y;

        if(ix < minx) minx = ix;
        if(ix > maxx) maxx = ix;
        if(iy < miny) miny = iy;
        if(iy > maxy) maxy = iy;
    }

    minx = (minx >> 3) & ~7;
    miny = (miny >> 3) & ~7;
    maxx = (maxx + 7) >> 3;
    maxy = (maxy + 7) >> 3;

    for(i = 0; i < mPathPoints; ++i)
    {
        mpPathPoints[i].x -= minx * 8;
        mpPathPoints[i].y -= miny * 8;
    }

    if(minx > maxx || miny > maxy)
    {
        mWidth = mHeight = 0;
        mPathOffsetX = mPathOffsetY = 0;
        _TrashPath();
        return true;
    }

    mWidth = maxx + 1 - minx;
    mHeight = maxy + 1 - miny;

    mPathOffsetX = minx;
    mPathOffsetY = miny;

    // Initialize edge buffer.  We use edge 0 as a sentinel.

    mEdgeNext = 1;
    mEdgeHeapSize = 2048;
    mpEdgeBuffer = (Edge*)malloc(sizeof(Edge) * mEdgeHeapSize);

    // Initialize scanline list.

    mpScanBuffer = DNew size_t[mHeight];
    memset(mpScanBuffer, 0, mHeight * sizeof(size_t));

    // Scan convert the outline.  Yuck, Bezier curves....

    // Unfortunately, Windows 95/98 GDI has a bad habit of giving us text
    // paths with all but the first figure left open, so we can't rely
    // on the PT_CLOSEFIGURE flag being used appropriately.

    fFirstSet = false;
    firstp.x = firstp.y = 0;
    lastp.x = lastp.y = 0;

    for(i = 0; i < mPathPoints; ++i)
    {
        BYTE t = mpPathTypes[i] & ~PT_CLOSEFIGURE;

        switch(t)
        {
        case PT_MOVETO:
            if(lastmoveto >= 0 && firstp != lastp)
                _EvaluateLine(lastp.x, lastp.y, firstp.x, firstp.y);
            lastmoveto = i;
            fFirstSet = false;
            lastp = mpPathPoints[i];
            break;
        case PT_MOVETONC:
            break;
        case PT_LINETO:
            if(mPathPoints - (i - 1) >= 2) _EvaluateLine(i - 1, i);
            break;
        case PT_BEZIERTO:
            if(mPathPoints - (i - 1) >= 4) _EvaluateBezier(i - 1, false);
            i += 2;
            break;
        case PT_BSPLINETO:
            if(mPathPoints - (i - 1) >= 4) _EvaluateBezier(i - 1, true);
            i += 2;
            break;
        case PT_BSPLINEPATCHTO:
            if(mPathPoints - (i - 3) >= 4) _EvaluateBezier(i - 3, true);
            break;
        }
    }

    if(lastmoveto >= 0 && firstp != lastp)
        _EvaluateLine(lastp.x, lastp.y, firstp.x, firstp.y);

    // Free the path since we don't need it anymore.

    _TrashPath();

    // Convert the edges to spans.  We couldn't do this before because some of
    // the regions may have winding numbers >+1 and it would have been a pain
    // to try to adjust the spans on the fly.  We use one heap to detangle
    // a scanline's worth of edges from the singly-linked lists, and another
    // to collect the actual scans.

    std::vector<int> heap;

    mOutline.reserve(mEdgeNext / 2);

    __int64 y = 0;

    for(y = 0; y < mHeight; ++y)
    {
        int count = 0;

        // Detangle scanline into edge heap.

        for(size_t ptr = (mpScanBuffer[y] & size_t(-1)); ptr; ptr = mpEdgeBuffer[ptr].next)
        {
            heap.push_back(mpEdgeBuffer[ptr].posandflag);
        }

        // Sort edge heap.  Note that we conveniently made the opening edges
        // one more than closing edges at the same spot, so we won't have any
        // problems with abutting spans.

        std::sort(heap.begin(), heap.end()/*begin() + heap.size()*/);

        // Process edges and add spans.  Since we only check for a non-zero
        // winding number, it doesn't matter which way the outlines go!

        std::vector<int>::iterator itX1 = heap.begin();
        std::vector<int>::iterator itX2 = heap.end(); // begin() + heap.size();

        size_t x1, x2;

        for(; itX1 != itX2; ++itX1)
        {
            size_t x = *itX1;

            if(!count)
                x1 = (x >> 1);

            if(x & 1)
                ++count;
            else
                --count;

            if(!count)
            {
                x2 = (x >> 1);

                if(x2 > x1)
                    mOutline.push_back(std::pair<__int64, __int64>((y << 32) + x1 + 0x4000000040000000i64, (y << 32) + x2 + 0x4000000040000000i64)); // G: damn Avery, this is evil! :)
            }
        }

        heap.clear();
    }

    // Dump the edge and scan buffers, since we no longer need them.

    free(mpEdgeBuffer);
    delete [] mpScanBuffer;

    // All done!

    return true;
}

using namespace std;

void Rasterizer::_OverlapRegion(tSpanBuffer& dst, tSpanBuffer& src, int dx, int dy)
{
    tSpanBuffer temp;

    temp.reserve(dst.size() + src.size());

    dst.swap(temp);

    tSpanBuffer::iterator itA = temp.begin();
    tSpanBuffer::iterator itAE = temp.end();
    tSpanBuffer::iterator itB = src.begin();
    tSpanBuffer::iterator itBE = src.end();

    // Don't worry -- even if dy<0 this will still work! // G: hehe, the evil twin :)

    unsigned __int64 offset1 = (((__int64)dy) << 32) - dx;
    unsigned __int64 offset2 = (((__int64)dy) << 32) + dx;

    while(itA != itAE && itB != itBE)
    {
        if((*itB).first + offset1 < (*itA).first)
        {
            // B span is earlier.  Use it.

            unsigned __int64 x1 = (*itB).first + offset1;
            unsigned __int64 x2 = (*itB).second + offset2;

            ++itB;

            // B spans don't overlap, so begin merge loop with A first.

            for(;;)
            {
                // If we run out of A spans or the A span doesn't overlap,
                // then the next B span can't either (because B spans don't
                // overlap) and we exit.

                if(itA == itAE || (*itA).first > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itA++).second);
                }
                while(itA != itAE && (*itA).first <= x2);

                // If we run out of B spans or the B span doesn't overlap,
                // then the next A span can't either (because A spans don't
                // overlap) and we exit.

                if(itB == itBE || (*itB).first + offset1 > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itB++).second + offset2);
                }
                while(itB != itBE && (*itB).first + offset1 <= x2);
            }

            // Flush span.

            dst.push_back(tSpan(x1, x2));
        }
        else
        {
            // A span is earlier.  Use it.

            unsigned __int64 x1 = (*itA).first;
            unsigned __int64 x2 = (*itA).second;

            ++itA;

            // A spans don't overlap, so begin merge loop with B first.

            for(;;)
            {
                // If we run out of B spans or the B span doesn't overlap,
                // then the next A span can't either (because A spans don't
                // overlap) and we exit.

                if(itB == itBE || (*itB).first + offset1 > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itB++).second + offset2);
                }
                while(itB != itBE && (*itB).first + offset1 <= x2);

                // If we run out of A spans or the A span doesn't overlap,
                // then the next B span can't either (because B spans don't
                // overlap) and we exit.

                if(itA == itAE || (*itA).first > x2)
                    break;

                do
                {
                    x2 = _MAX(x2, (*itA++).second);
                }
                while(itA != itAE && (*itA).first <= x2);
            }

            // Flush span.

            dst.push_back(tSpan(x1, x2));
        }
    }

    // Copy over leftover spans.

    while(itA != itAE)
        dst.push_back(*itA++);

    while(itB != itBE)
    {
        dst.push_back(tSpan((*itB).first + offset1, (*itB).second + offset2));
        ++itB;
    }
}

bool Rasterizer::CreateWidenedRegion(int rx, int ry)
{
    if(rx < 0) rx = 0;
    if(ry < 0) ry = 0;

    mWideBorder = max(rx, ry);

    if(ry > 0)
    {
        // Do a half circle.
        // _OverlapRegion mirrors this so both halves are done.
        for(ptrdiff_t y = -ry; y <= ry; ++y)
        {
            int x = (int)(0.5 + sqrt(float(ry * ry - y * y)) * float(rx) / float(ry));

            _OverlapRegion(mWideOutline, mOutline, x, y);
        }
    }
    else if(ry == 0 && rx > 0)
    {
        // There are artifacts if we don't make at least two overlaps of the line, even at same Y coord
        _OverlapRegion(mWideOutline, mOutline, rx, 0);
        _OverlapRegion(mWideOutline, mOutline, rx, 0);
    }

    return true;
}

void Rasterizer::DeleteOutlines()
{
    mWideOutline.clear();
    mOutline.clear();
}

bool Rasterizer::Rasterize(int xsub, int ysub, int fBlur, double fGaussianBlurX, double fGaussianBlurY)
{
    _TrashOverlay();

    if(!mWidth || !mHeight)
    {
        mOverlayWidth = mOverlayHeight = 0;
        return true;
    }

    xsub &= 7;
    ysub &= 7;

    int width = mWidth + xsub;
    int height = mHeight;// + ysub

    mOffsetX = mPathOffsetX - xsub;
    mOffsetY = mPathOffsetY - ysub;

    mWideBorder = (mWideBorder + 7)&~7;

    double fGaussianBlur = max(fGaussianBlurX, fGaussianBlurY);

    int direction = 0;
    if (fGaussianBlurX != 0 && fGaussianBlurY == 0)
        direction = 1;
    else if (fGaussianBlurX == 0 && fGaussianBlurY != 0)
        direction = 2;

    if(!mWideOutline.empty() || fBlur || fGaussianBlur > 0)
    {
        int bluradjust = 0;
        if(fGaussianBlur > 0)
            bluradjust += (int)(fGaussianBlur * 3 * 8 + 0.5) | 1;
        if(fBlur)
            bluradjust += 8;

        // Expand the buffer a bit when we're blurring, since that can also widen the borders a bit
        bluradjust = (bluradjust + 7)&~7;

        width += 2 * mWideBorder + bluradjust * 2;
        height += 2 * mWideBorder + bluradjust * 2;

        xsub += mWideBorder + bluradjust;
        ysub += mWideBorder + bluradjust;

        mOffsetX -= mWideBorder + bluradjust;
        mOffsetY -= mWideBorder + bluradjust;
    }

    mOverlayWidth = ((width + 7) >> 3) + 1;
    mOverlayHeight = ((height + 14) >> 3) + 1;

    mpOverlayBuffer = DNew byte[2 * mOverlayWidth * mOverlayHeight];
    memset(mpOverlayBuffer, 0, 2 * mOverlayWidth * mOverlayHeight);

    // Are we doing a border?

    tSpanBuffer* pOutline[2] = {&mOutline, &mWideOutline};

    for(ptrdiff_t i = countof(pOutline) - 1; i >= 0; i--)
    {
        tSpanBuffer::iterator it = pOutline[i]->begin();
        tSpanBuffer::iterator itEnd = pOutline[i]->end();

        for(; it != itEnd; ++it)
        {
            size_t y = (((*it).first >> 32) - 0x40000000 + ysub);
            size_t x1 = (((*it).first & 0xffffffff) - 0x40000000 + xsub);
            size_t x2 = (((*it).second & 0xffffffff) - 0x40000000 + xsub);

            if(x2 > x1)
            {
                size_t first = x1 >> 3;
                size_t last = (x2 - 1) >> 3;
                byte* dst = mpOverlayBuffer + 2 * (mOverlayWidth * (y >> 3) + first) + i;

                if(first == last)
                    *dst += x2 - x1;
                else
                {
                    *dst += ((first + 1) << 3) - x1;
                    dst += 2;

                    while(++first < last)
                    {
                        *dst += 0x08;
                        dst += 2;
                    }

                    *dst += x2 - (last << 3);
                }
            }
        }
    }

    // Do some gaussian blur magic
    if(fGaussianBlur > 0)
    {
        GaussianKernel filter(fGaussianBlur);
        if(mOverlayWidth >= filter.width && mOverlayHeight >= filter.width)
        {
            size_t pitch = mOverlayWidth * 2;

            byte *tmp = DNew byte[pitch*mOverlayHeight];
            if(!tmp) return(false);

            int border = !mWideOutline.empty() ? 1 : 0;

            byte *src = mpOverlayBuffer + border;

            

            SeparableFilterX<2>(src, tmp, mOverlayWidth, mOverlayHeight, pitch, filter.kernel, filter.width, filter.divisor, direction);
            SeparableFilterY<2>(tmp, src, mOverlayWidth, mOverlayHeight, pitch, filter.kernel, filter.width, filter.divisor, direction);

            delete[] tmp;
        }
    }

    // If we're blurring, do a 3x3 box blur
    // Can't do it on subpictures smaller than 3x3 pixels
    for(int pass = 0; pass < fBlur; pass++)
    {
        if(mOverlayWidth >= 3 && mOverlayHeight >= 3)
        {
            int pitch = mOverlayWidth * 2;

            byte* tmp = DNew byte[pitch*mOverlayHeight];
            if(!tmp) return(false);

            memcpy(tmp, mpOverlayBuffer, pitch * mOverlayHeight);

            int border = !mWideOutline.empty() ? 1 : 0;

            // This could be done in a separated way and win some speed
            for(ptrdiff_t j = 1; j < mOverlayHeight - 1; j++)
            {
                byte* src = tmp + pitch * j + 2 + border;
                byte* dst = mpOverlayBuffer + pitch * j + 2 + border;

                for(ptrdiff_t i = 1; i < mOverlayWidth - 1; i++, src += 2, dst += 2)
                {
                    *dst = (src[-2-pitch] + (src[-pitch] << 1) + src[+2-pitch]
                            + (src[-2] << 1) + (src[0] << 2) + (src[+2] << 1)
                            + src[-2+pitch] + (src[+pitch] << 1) + src[+2+pitch]) >> 4;
                }
            }

            delete [] tmp;
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////

static __forceinline void pixmix(DWORD *dst, DWORD color, DWORD alpha, MOD_BLEND mod_blendMode = BLEND_NORMAL)
{
    DWORD a = (((alpha) * (color >> 24)) >> 6) & 0xff;
    DWORD ia = 256 - a;
    a += 1;

    //DWORD tmp = (((((*dst >> 8) & 0x00ff0000) * ia) & 0xff000000) >> 24) & 0xFF;
    *dst = ((((*dst & 0x00ff00ff) * ia + (color & 0x00ff00ff) * a) & 0xff00ff00) >> 8)
           | ((((*dst & 0x0000ff00) * ia + (color & 0x0000ff00) * a) & 0x00ff0000) >> 8)
           | ((((*dst >> 8) & 0x00ff0000) * ia) & 0xff000000);
}

#include <xmmintrin.h>
#include <emmintrin.h>

static __forceinline void pixmix_sse2(DWORD* dst, DWORD color, DWORD alpha, MOD_BLEND mod_blendMode = BLEND_NORMAL)
{
    alpha = (((alpha) * (color >> 24)) >> 6) & 0xff;
    color &= 0xffffff;

    __m128i zero = _mm_setzero_si128();
    __m128i a = _mm_set1_epi32(((alpha + 1) << 16) | (0x100 - alpha));
    __m128i d = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*dst), zero);
    __m128i s = _mm_unpacklo_epi8(_mm_cvtsi32_si128(color), zero);
    __m128i r = _mm_unpacklo_epi16(d, s);

    r = _mm_madd_epi16(r, a);
    r = _mm_srli_epi32(r, 8);
    r = _mm_packs_epi32(r, r);
    r = _mm_packus_epi16(r, r);

    *dst = (DWORD)_mm_cvtsi128_si32(r);
}

// For CPUID usage in Rasterizer::Draw
#include "../dsutil/vd.h"

// Render a subpicture onto a surface.
// spd is the surface to render on.
// clipRect is a rectangular clip region to render inside.
// pAlphaMask is an alpha clipping mask.
// xsub and ysub ???
// switchpts seems to be an array of fill colours interlaced with coordinates.
//    switchpts[i*2] contains a colour and switchpts[i*2+1] contains the coordinate to use that colour from
// fBody tells whether to render the body of the subs.
// fBorder tells whether to render the border of the subs.
#ifdef _VSMOD // patch m004. gradient colors
CRect Rasterizer::Draw(SubPicDesc& spd, CRect& clipRect, byte* pAlphaMask, int xsub, int ysub,
                       const DWORD* switchpts, bool fBody, bool fBorder, int typ, MOD_GRADIENT& mod_grad, MOD_MOVEVC& mod_vc, MOD_BLEND mod_blendMode)
#else
CRect Rasterizer::Draw(SubPicDesc& spd, CRect& clipRect, byte* pAlphaMask, int xsub, int ysub,
                       const DWORD* switchpts, bool fBody, bool fBorder)
#endif
{
    CRect bbox(0, 0, 0, 0);

    if(!switchpts || !fBody && !fBorder) return(bbox);

    // Limit drawn area to intersection of rendering surface and rectangular clip area
    CRect r(0, 0, spd.w, spd.h);
    r &= clipRect;

    // Remember that all subtitle coordinates are specified in 1/8 pixels
    // (x+4)>>3 rounds to nearest whole pixel.
    // ??? What is xsub, ysub, mOffsetX and mOffsetY ?
    int x = (xsub + mOffsetX + 4) >> 3;
    int y = (ysub + mOffsetY + 4) >> 3;
    int w = mOverlayWidth;
    int h = mOverlayHeight;
    int xo = 0, yo = 0;

    int yBeforeClip = y;

    // Again, limiting?
    if(x < r.left)
    {
        xo = r.left - x;
        w -= r.left - x;
        x = r.left;
    }
    if(y < r.top)
    {
        yo = r.top - y;
        h -= r.top - y;
        y = r.top;
    }
    if(x + w > r.right) w = r.right - x;
    if(y + h > r.bottom) h = r.bottom - y;

    // Check if there's actually anything to render
    if(w <= 0 || h <= 0) return(bbox);

    bbox.SetRect(x, y, x + w, y + h);
    bbox &= CRect(0, 0, spd.w, spd.h);

    // CPUID from VDub
    bool fSSE2 = !!(g_cpuid.m_flags & CCpuID::sse2);
    //bool fSSE2 = false;

#ifdef _VSMOD // patch m006. moveable vector clip
    mod_vc.hfull = h;
    mod_vc.curpos = CPoint(x, y);
    mod_vc.alphamask = pAlphaMask + spd.w * y + x;
#endif

    // fill rasterize info
    RasterizerNfo rnfo;
    // Grab the first colour
    rnfo.color = switchpts[0];
    // How would this differ from src?
    rnfo.dst = (DWORD*)((char *)spd.bits + (spd.pitch * y)) + x;
    rnfo.sw = switchpts;

    rnfo.w = w;
    rnfo.h = h;
    rnfo.xo = xo;
    rnfo.yo = yo;
    rnfo.overlayp = mOverlayWidth;
    rnfo.pitch = spd.pitch;
    rnfo.spdw = spd.w;
    // The alpha bitmap of the subtitles?
    rnfo.src = mpOverlayBuffer + 2 * (mOverlayWidth * yo + xo);
    // s points to what the "body" to use is
    // If we're rendering body fill and border, src+1 points to the array of
    // widened regions which contain both border and fill in one.
    rnfo.s = fBorder ? (rnfo.src + 1) : rnfo.src;
#ifdef _VSMOD // patch m006. moveable vector clip
    rnfo.mod_vc = mod_vc;
    rnfo.mod_grad = mod_grad;
    rnfo.mod_grad.width = mOverlayWidth;
    rnfo.mod_grad.height = mOverlayHeight;
    rnfo.mod_grad.xoffset = xo;
    rnfo.mod_grad.yoffset = yo;
    rnfo.typ = typ;
    rnfo.mod_blendMode = mod_blendMode; // vpatch v003. blending mode
    rnfo.mod_grad.clipDiff = clipRect.bottom < yBeforeClip + mOverlayHeight ? yBeforeClip + mOverlayHeight - clipRect.bottom : 0;
#else
    // The complex "vector clip mask" I think.
    rnfo.am = pAlphaMask + spd.w * y + x;
#endif

    COverlayGetter * Color;
    
    // Every remaining line in the bitmap to be rendered...
    // Basic case of no complex clipping mask
#ifdef _VSMOD // patch m004. gradient colors
    if (
        ((typ == 0) && ((mod_grad.mode[0] == 0) && (mod_grad.mode[1] == 0)))
        || ((typ != 0) && (mod_grad.mode[typ] == 0))
        )
        // No gradient
    {
#endif
        Color = new COverlayColor(switchpts[0], switchpts[2]);
#ifdef _VSMOD // patch m004. gradient colors
    }
    else
        Color = new COverlayGradient(rnfo.mod_grad, typ);
#endif

#if defined (_VSMOD) && defined(_LUA)
    if (LuaRendererHandler.GetLength() > 0)
    {
        COverlayGetter* Alpha = (pAlphaMask) ? new COverlayAlpha(mod_vc) : NULL;

        if (fSSE2)
        {
            COverlayLuaMixer<COverlayMixerSSE2> Mix(&rnfo, Color, Alpha);
            Mix.L = L;
            Mix.LuaLog = LuaLog;
            Mix.m_entry = m_entry;
            Mix.m_layer = m_layer;
            Mix.Function = LuaRendererHandler;
            Mix.Draw(fBody);
        }
        else
        {
            COverlayLuaMixer<COverlayMixer> Mix(&rnfo, Color, Alpha);
            Mix.L = L;
            Mix.LuaLog = LuaLog;
            Mix.m_entry = m_entry;
            Mix.Function = LuaRendererHandler;
            Mix.Draw(fBody);
        }

        if (Alpha) delete Alpha;
    }
    else
    {
#endif
        if (!pAlphaMask)
        {
            if (fSSE2)
            {
                COverlayMixerSSE2 Mix(&rnfo, Color);
                Mix.Draw(fBody);
            }
            else
            {
                COverlayMixer Mix(&rnfo, Color);
                Mix.Draw(fBody);
            }
        }
        else
        {
            COverlayGetter* Alpha;

#ifdef _VSMOD
            Alpha = new COverlayAlpha(mod_vc);
#else
            Alpha = new COverlayAlpha(pAlphaMask, spd.w, spd.h);
#endif
            if (fSSE2)
            {
                COverlayAlphaMixer<COverlayMixerSSE2> Mix(&rnfo, Color, Alpha);
                Mix.Draw(fBody);
            }
            else
            {
                COverlayAlphaMixer<COverlayMixer> Mix(&rnfo, Color, Alpha);
                Mix.Draw(fBody);
            }

            delete Alpha;
        }
#if defined (_VSMOD) && defined(_LUA)
    }
#endif

    delete Color;
    // Remember to EMMS!
    // Rendering fails in funny ways if we don't do this.

#ifndef _M_X64
    _mm_empty();
#endif

    return bbox;
}


#if 0
void Rasterizer::FillSolidRect(SubPicDesc& spd, int x, int y, int nWidth, int nHeight, DWORD lColor)
{
    bool fSSE2 = !!(g_cpuid.m_flags & CCpuID::sse2);

    // Warning : lColor is AARRGGBB (same format as DirectX D3DCOLOR_ARGB)
    for(int wy = y; wy < y + nHeight; wy++)
    {
        DWORD* dst = (DWORD*)((BYTE*)spd.bits + spd.pitch * wy) + x;

        memsetd(dst, lColor, nWidth * 4);
    }
}
#else
void Rasterizer::FillSolidRect(SubPicDesc& spd, int x, int y, int nWidth, int nHeight, DWORD lColor)
{
    bool fSSE2 = !!(g_cpuid.m_flags & CCpuID::sse2);

    for(int wy = y; wy < y + nHeight; wy++)
    {
        DWORD* dst = (DWORD*)((BYTE*)spd.bits + spd.pitch * wy) + x;

        if(fSSE2)
            for(int wt = 0; wt < nWidth; ++wt)
                pixmix_sse2(&dst[wt], lColor, 0x40);	// 0x40 because >> 6 in pixmix (to preserve tranparency)
        else
            for(int wt = 0; wt < nWidth; ++wt)
                pixmix(&dst[wt], lColor, 0x40);
    }
}
#endif

#ifdef _VSMOD // patch m006. moveable vector clip
MOD_MOVEVC::MOD_MOVEVC()
{
    clear();
}

void MOD_MOVEVC::clear()
{
    enable = false;
    size = CSize(0, 0);
    pos = CPoint(0, 0);
//	canvas = CSize(0,0);
    spd = CSize(0, 0);
    curpos = CPoint(0, 0);
    hfull = 0;
    alphamask = NULL;
    isInverse = false;
}

byte MOD_MOVEVC::GetAlphaValue(int wx, int wy)
{
    byte alpham;
    if(!enable)
    {
//		return 0xFF;
        if((wx < 0) || (wx >= spd.cx)) return 0;
        if((wy <= 0) || (wy > spd.cy)) return 0;

        return alphamask[wx+((hfull-wy)*spd.cx)];
    }

    int xInCanvas = wx - pos.x + curpos.x;
    int yInCanvas = -pos.y + (hfull - wy) + curpos.y;

    //check if the point is in canvas, in case of crash
    if (xInCanvas < 0 || xInCanvas >= spd.cx)
        alpham = isInverse ? 0x40 : 0; // assume content outside canvas is visible when using iclip. mask is 6 bit
    else if (yInCanvas < 0 || yInCanvas >= spd.cy)
        alpham = isInverse ? 0x40 : 0;
    else
    {
        if ((wx - pos.x) < -curpos.x + 1) alpham = 0;
        else if ((wx - pos.x) > spd.cx - 1) alpham = 0; //canvas.cx
        else if ((hfull - wy - pos.y) < -curpos.y + 1) alpham = 0;
        else if ((hfull - wy - pos.y) > spd.cy - 1) alpham = 0;
        else alpham = alphamask[wx - pos.x - pos.y*spd.cx + ((hfull - wy)*spd.cx)];
    }

    return alpham;
}
#endif

RasterizerNfo::RasterizerNfo()
{
    /*
    w = 0;
    h = 0;
    spdw = 0;
    overlayp = 0;
    typ = 0;
    pitch = 0;
    color = 0;

    xo = 0;

    sw = NULL;
    s = NULL;
    src = NULL;
    dst = NULL;

    #ifdef _VSMOD
    mod_grad.clear();
    mod_vc.clear();
    #else
    am = NULL;
    #endif
    */
}