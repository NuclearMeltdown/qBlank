#pragma once

// HLSL for the two render passes, compiled at startup with D3DCompile.
//
// Pass 1 ("convert") turns whatever the card delivers into linear-indexed RGBA
// at the cropped source resolution: colour space conversion, range expansion,
// bob deinterlacing and cropping all happen here, all with texel fetches so no
// sampler filtering can smear anything before we mean it to.
//
// Pass 2 ("scale") resamples that intermediate onto the window with the
// selected filter, plus optional sharpening.

namespace cap {

inline const char* kFullscreenVS = R"HLSL(
struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

// Single oversized triangle -- no vertex or index buffer needed.
VSOut main(uint vid : SV_VertexID) {
  VSOut o;
  float2 uv = float2((vid << 1) & 2, vid & 2);
  o.uv = uv;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
)HLSL";

inline const char* kCleanPS =
R"HLSL(// Pass one: turn the captured planes into a picture and clean it up, still
// in the source's own geometry. Nothing here knows about fields, cropping,
// rotation or scaling -- those all come afterwards, and they all get to work
// on a signal that has already had the composite artefacts taken out of it.
Texture2D<float4> tex0 : register(t0);
Texture2D<float4> tex1 : register(t1);
Texture2D<float4> tex2 : register(t2);

// The same planes for the three preceding frames, newest first. YADIF needs one
// of them; the composite denoiser needs all three, because the PAL colour
// subcarrier walks through a four frame sequence and only a full four frame
// average cancels it. Bound but never sampled when nothing wants them, and the
// copies that fill them are skipped in that case.
Texture2D<float4> prvA0 : register(t3);
Texture2D<float4> prvA1 : register(t4);
Texture2D<float4> prvA2 : register(t5);
Texture2D<float4> prvB0 : register(t6);
Texture2D<float4> prvB1 : register(t7);
Texture2D<float4> prvB2 : register(t8);
Texture2D<float4> prvC0 : register(t9);
Texture2D<float4> prvC1 : register(t10);
Texture2D<float4> prvC2 : register(t11);

cbuffer ConvertCB : register(b0) {
  int   gFormatKind;      // see FormatKind in video_renderer.h
  int   gDeinterlaceMode; // 0 off, 1 bob, 2 bob linear, 3 motion adaptive,
                          // 4 edge directed, 5 yadif
  int   gFieldIndex;      // 0 = first field, 1 = second field
  int   gBottomUp;

  int   gCropLeft;
  int   gCropTop;
  int   gSrcWidth;
  int   gSrcHeight;

  int   gOutWidth;
  int   gOutHeight;
  int   gIsYuv;
  int   gHavePrev;        // previous frame's planes hold a usable picture

  float gYOffset;
  float gYScale;
  float gCScale;
  float gPixelScale;   // brings a ten-in-sixteen-bit sample back to 0..1

  int   gCoSitedPhase;    // -1 normal interlace, else the row a field pair starts on
  int   gRotation;        // 0 none, 1 quarter turn right, 2 half, 3 quarter turn left
  int   gLineDouble;      // 1 = every source line fills two output lines
  int   gChromaSoft;      // horizontal chroma blur radius in source pixels, 0 off

  float gTemporal;        // 0..1 strength of the temporal average
  int   gHistCount;       // how many previous frames actually hold a picture
  float gDotNotch;        // 0..1: width of the demodulation window, 0 = off
  float gCarrierPeriod;   // samples per cycle of the colour subcarrier

  int   gTransfer;        // 0 display referred, 1 PQ, 2 HLG
  int   gGamut;           // 1 = BT.2020 primaries, convert to BT.709
  float gMotionSlack;     // how much movement the temporal gate ignores
  float gMotionSlope;     // and how fast it lets go above that

  int   gMotionComp;      // 1 = follow the movement when averaging noise away
  int   gAdaptChroma;     // 1 = soften colour only where brightness invites it
  float gBandwidth;       // 0..1, how much of the rolled off luma band to restore
  // A/B comparison: the share of the cropped width left of the divider, where
  // this whole pass is skipped so the two halves can be looked at side by side.
  // Negative = off, which is why it is not simply zero: zero is a legitimate
  // setting and means the divider sits on the left edge.
  float gCompareSplit;

  float4 gCoef;           // Cr->R, Cb->G, Cr->G, Cb->B

  // Which way the A/B divider runs: 0 upright, cutting the cropped width, 1
  // across, cutting the cropped height. Either way the unfiltered part is the
  // one nearer the origin -- left, or above.
  int   gCompareAxis;
  // How many frames the temporal average covers, 2..4: one cycle of the dot
  // crawl, as measured on this source. Four until something is measured.
  int   gCrawlCycle;
  // Rows between two lines of the same field: 2 on a woven interlaced frame,
  // 1 otherwise. The motion search moves up and down in steps of this.
  int   gMotionRows;
  int   gChromaLinear;    // 1 = digital source, see FetchYuvCur
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

// `frame` is 0 for the picture being shown and 1..3 for the ones before it.
// HLSL has no way to put a texture behind a variable here, so picking one is a
// chain of comparisons rather than an index.
float4 LoadPlane0(int3 at, int frame) {
  if (frame == 0) return tex0.Load(at);
  if (frame == 1) return prvA0.Load(at);
  if (frame == 2) return prvB0.Load(at);
  return prvC0.Load(at);
}
float4 LoadPlane1(int3 at, int frame) {
  if (frame == 0) return tex1.Load(at);
  if (frame == 1) return prvA1.Load(at);
  if (frame == 2) return prvB1.Load(at);
  return prvC1.Load(at);
}
float4 LoadPlane2(int3 at, int frame) {
  if (frame == 0) return tex2.Load(at);
  if (frame == 1) return prvA2.Load(at);
  if (frame == 2) return prvB2.Load(at);
  return prvC2.Load(at);
}

float3 FetchYuv(int2 p, int frame) {
  // p is a coordinate in the full source frame.
  if (gFormatKind == 0 || gFormatKind == 1 || gFormatKind == 2) {
    // Packed 4:2:2. One texel of the RGBA8 view holds two pixels.
    float4 t = LoadPlane0(int3(p.x >> 1, p.y, 0), frame);
    bool odd = (p.x & 1) != 0;
    if (gFormatKind == 0) {          // YUY2: Y0 U Y1 V
      return float3(odd ? t.z : t.x, t.y, t.w);
    } else if (gFormatKind == 1) {   // UYVY: U Y0 V Y1
      return float3(odd ? t.w : t.y, t.x, t.z);
    } else {                         // YVYU: Y0 V Y1 U
      return float3(odd ? t.z : t.x, t.w, t.y);
    }
  } else if (gFormatKind == 3 || gFormatKind == 7) {
    // NV12, and P010/P016 which are the same arrangement in wider containers.
    // gPixelScale carries the difference: the ten bit form leaves the bottom
    // six bits of each sample empty.
    float y  = LoadPlane0(int3(p, 0), frame).x;
    float2 c = LoadPlane1(int3(p.x >> 1, p.y >> 1, 0), frame).xy;
    return float3(y, c.x, c.y) * gPixelScale;
  } else {
    // Planar 4:2:0. Plane order differs between YV12 and I420, which the
    // caller has already resolved by binding the planes accordingly.
    int3 ac = int3(p.x >> 1, p.y >> 1, 0);
    float y = LoadPlane0(int3(p, 0), frame).x;
    float u = LoadPlane1(ac, frame).x;
    float v = LoadPlane2(ac, frame).x;
    return float3(y, u, v);
  }
}

// ---------------------------------------------------------------------------
// High dynamic range.
//
// A capture card hands over pictures encoded against one of two curves, and
// neither is the one a screen expects. PQ says outright how many nits a code
// means, from nothing up to ten thousand; HLG says it relative to whatever the
// display can manage. Both are decoded here into linear light, scaled so that
// 1.0 is diffuse white -- the brightness a sheet of paper would have. Anything
// above 1.0 is a highlight, which is the whole point of the exercise.
//
// Checked against the numbers in the standards before it was written: PQ 0.5
// comes out at 92.246 nits where ST.2084 says 92.245, and HLG lands exactly on
// its three defined points.
// ---------------------------------------------------------------------------
static const float kPqM1 = 0.1593017578125;
static const float kPqM2 = 78.84375;
static const float kPqC1 = 0.8359375;
static const float kPqC2 = 18.8515625;
static const float kPqC3 = 18.6875;

float PqToNits(float e) {
  float p = pow(saturate(e), 1.0 / kPqM2);
  float num = max(p - kPqC1, 0.0);
  float den = kPqC2 - kPqC3 * p;
  return 10000.0 * pow(num / max(den, 1e-6), 1.0 / kPqM1);
}

float NitsToPq(float nits) {
  float y = saturate(nits / 10000.0);
  float p = pow(y, kPqM1);
  return pow((kPqC1 + kPqC2 * p) / (1.0 + kPqC3 * p), kPqM2);
}

float HlgToScene(float e) {
  const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
  e = saturate(e);
  return e <= 0.5 ? (e * e) / 3.0 : (exp((e - c) / a) + b) / 12.0;
}

// BT.2020 to BT.709, both through XYZ at D65. Components can come out negative:
// BT.2020 holds colours BT.709 cannot name, and clamping them here would turn a
// deep green into a flat one. That is left to the end of the pipeline.
float3 Bt2020ToBt709(float3 c) {
  return float3(
      dot(c, float3( 1.660491, -0.587641, -0.072850)),
      dot(c, float3(-0.124550,  1.132900, -0.008349)),
      dot(c, float3(-0.018151, -0.100579,  1.118730)));
}

float3 YuvToRgb(float3 yuv) {
  float y = (yuv.x - gYOffset) * gYScale;
  float cb = (yuv.y - 0.5) * gCScale;
  float cr = (yuv.z - 0.5) * gCScale;
  float3 rgb;
  rgb.r = y + gCoef.x * cr;
  rgb.g = y - gCoef.y * cb - gCoef.z * cr;
  rgb.b = y + gCoef.w * cb;
  return rgb;
}

float3 FetchRgbIn(int2 p, int frame) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  int2 q = p;
  if (gBottomUp != 0) q.y = gSrcHeight - 1 - q.y;

  if (gIsYuv != 0) {
    return YuvToRgb(FetchYuv(q, frame));
  }
  float3 rgb = LoadPlane0(int3(q, 0), frame).rgb;
  // RGB from a capture card can still be limited range.
  return (rgb - gYOffset) * gYScale;
}

// The current frame, read straight from its own textures. Worth writing twice:
// the deinterlacers call this dozens of times per pixel, and going through the
// four way choice of which frame to read expands every one of those calls into
// four branches for a decision that is a constant here. That expansion is
// nobody's runtime cost, but it is very much the shader compiler's -- and the
// compiler runs while the user waits for the window to appear.
//
// Each colour sample sits on the even pixel of its pair, so the odd pixel lies
// halfway between two of them. A digital source has colour edges as sharp as
// its brightness, and copying the left sample there moves every such edge half
// a pixel and turns it into a step; the midpoint of the two is where it
// belongs. Composite colour has no edges that sharp, and everything in the clean
// pass was measured on the copy, so an analogue source keeps it -- and since a
// digital one runs none of the filters, only main asks for the midpoint, and
// every other caller compiles without it.
float2 PackedChroma(float4 t) {
  if (gFormatKind == 0) return t.yw;   // YUY2: Y0 U Y1 V
  if (gFormatKind == 1) return t.xz;   // UYVY: U Y0 V Y1
  return t.wy;                         // YVYU: Y0 V Y1 U
}
float3 FetchYuvCur(int2 p, bool between) {
  const bool odd = (p.x & 1) != 0;
  const bool mid = odd && between;
  const int cx = p.x >> 1;
  const int nx = min(cx + 1, ((gSrcWidth + 1) >> 1) - 1);
  if (gFormatKind <= 2) {
    float4 t = tex0.Load(int3(cx, p.y, 0));
    float y = gFormatKind == 1 ? (odd ? t.w : t.y) : (odd ? t.z : t.x);
    float2 c = PackedChroma(t);
    if (mid) c = (c + PackedChroma(tex0.Load(int3(nx, p.y, 0)))) * 0.5;
    return float3(y, c);
  }
  const int cy = p.y >> 1;
  float2 c;
  if (gFormatKind == 3 || gFormatKind == 7) {
    c = tex1.Load(int3(cx, cy, 0)).xy;
    if (mid) c = (c + tex1.Load(int3(nx, cy, 0)).xy) * 0.5;
    return float3(tex0.Load(int3(p, 0)).x, c) * gPixelScale;
  }
  c = float2(tex1.Load(int3(cx, cy, 0)).x, tex2.Load(int3(cx, cy, 0)).x);
  if (mid) c = (c + float2(tex1.Load(int3(nx, cy, 0)).x, tex2.Load(int3(nx, cy, 0)).x)) * 0.5;
  return float3(tex0.Load(int3(p, 0)).x, c);
}

float3 FetchRgbAtL(int2 p, bool linearChroma) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  int2 q = p;
  if (gBottomUp != 0) q.y = gSrcHeight - 1 - q.y;

  float3 rgb;
  if (gIsYuv != 0) {
    rgb = YuvToRgb(FetchYuvCur(q, linearChroma));
  } else {
    rgb = tex0.Load(int3(q, 0)).rgb;
    rgb = (rgb - gYOffset) * gYScale;
  }
  return rgb;
}
float3 FetchRgbAt(int2 p) { return FetchRgbAtL(p, false); }
float3 FetchRgbPrev(int2 p) { return FetchRgbIn(p, 1); }
float3 FetchRgbFrame(int2 p, int frame) { return FetchRgbIn(p, frame); }

float Luma(float3 c) {
  return dot(c, float3(0.299, 0.587, 0.114));
}

// Brightness alone, without the colour conversion. The motion test below needs
// a lot of samples and none of the colour, and converting each one would triple
// its cost for nothing.
float LumaFrameAt(int2 p, int frame) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  int2 q = p;
  if (gBottomUp != 0) q.y = gSrcHeight - 1 - q.y;

  if (gIsYuv == 0) return Luma(LoadPlane0(int3(q, 0), frame).rgb);
  if (gFormatKind == 0 || gFormatKind == 1 || gFormatKind == 2) {
    float4 t = LoadPlane0(int3(q.x >> 1, q.y, 0), frame);
    bool odd = (q.x & 1) != 0;
    if (gFormatKind == 1) return odd ? t.w : t.y;   // UYVY
    return odd ? t.z : t.x;                          // YUY2 and YVYU
  }
  return LoadPlane0(int3(q, 0), frame).x;
}

// The same fetch, but keeping the colour. The motion test below asks only how
// far each sample moved between frames, and a difference does not care what
// the values are centred on, so the offsets that YuvToRgb subtracts are left
// out and only the scales are kept: a step of a given size in any of the three
// numbers is then a step of that size in the picture, whichever one it came
// from. Weighted with the matrix's own coefficients rather than constants, so
// this follows the picture from BT.601 to BT.709 the way everything else does.
//
// On a packed format all three arrive in the Load that used to fetch the
// brightness alone, which is what makes looking at colour affordable here at
// all; NV12 and the planar forms pay one or two more fetches for it.
float3 GateSampleAt(int2 p, int frame) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  int2 q = p;
  if (gBottomUp != 0) q.y = gSrcHeight - 1 - q.y;

  if (gIsYuv == 0) return LoadPlane0(int3(q, 0), frame).rgb * gYScale;

  float3 yuv = FetchYuv(q, frame);
  return float3(yuv.x * gYScale,
                yuv.y * gCScale * gCoef.w,
                yuv.z * gCScale * gCoef.x);
}

// Composite carries colour at roughly a quarter of the bandwidth it carries
// brightness, so a picture decoded from it has no fine colour detail in it to
// begin with. Averaging the colour sideways therefore costs nothing real, and it
// takes the rainbow shimmer with it -- that shimmer is dense luma detail the
// decoder mistook for colour, and it lives in the chroma.
//
// The brightness is put back sharp afterwards. Only the colour is softened.
)HLSL"
R"HLSL(// How sharply a neighbour is discounted for having a different colour from the
// centre. Measured against a gold-on-blue edge from this card: at 0 -- a plain
// box average -- the edge loses 0.42 of its saturation, at 4 it loses 0.19, at
// 8 about 0.09, at 20 it loses 0.05. Smoothing inside a flat area is unchanged
// by any of it, because there every weight is near one anyway, so the whole
// scale is free of charge on the half of the job that matters.
//
// Eight, because past that the returns are small and the risk is not: a gentle
// chroma gradient -- a sky, a fade -- is made of small differences, and a
// weight that discounts those stops smoothing the very thing it is for.
static const float kChromaKeepEdge = 8.0;

float3 SoftenChroma(float3 rgb, int x, int row) {
  // Averaging colour sideways across a *colour edge* cannot work unweighted,
  // and the reason is not a bug in the arithmetic: gold and blue are close to
  // complementary, so the mean of their chroma genuinely is grey. That is what
  // put a grey ring around anything gold on a blue background.
  //
  // Note that keeping the original luma does not help with this, and that the
  // obvious "convert to YCbCr, average only Cb and Cr" is not a different
  // formulation -- adding (Luma(rgb) - Luma(soft)) to all three channels is
  // algebraically the same thing, to the last decimal. The fix has to be in
  // *which* neighbours are averaged, not in what is averaged.
  //
  // So each neighbour is weighted by how close its colour is to the centre's.
  // In a flat area every weight is near one and this is the box average it was
  // before; across an edge the far side falls away and the colour survives.
  const float centreLuma = Luma(rgb);
  const float3 centreChroma = rgb - centreLuma;

  float3 sum = 0.0;
  float n = 0.0;
  // Deliberately not unrolled. Every FetchRgbAt expands into a pixel format
  // branch and a four way choice of which frame to read, so seventeen unrolled
  // copies of it cost the shader compiler seconds -- which the user waits
  // through at every start. A real loop over the actual radius costs nothing at
  // runtime and compiles in a fraction of the time.
  for (int dx = -gChromaSoft; dx <= gChromaSoft; ++dx) {
    float3 s = FetchRgbAt(int2(x + dx, row));
    float3 sc = s - Luma(s);
    float3 d = sc - centreChroma;
    float w = exp(-dot(d, d) * kChromaKeepEdge);
    sum += sc * w;
    n += w;
  }
  // The centre's own luma, with the neighbourhood's colour. Luma of a chroma
  // vector is zero by construction, so this cannot shift brightness.
  return centreLuma + sum / max(n, 1e-4);
}

// Dot crawl is the other half of the same crosstalk, going the other way: colour
// leaking into brightness, as that crawling zip along vertical colour edges.
// Blurring the chroma does nothing for it, because it is not in the chroma.
//
// What does work is time. The colour subcarrier inverts its phase from one frame
// to the next, so the pattern it leaves is inverted too, and averaging two
// consecutive frames cancels it -- but only where the picture stood still.
// Anywhere something moved, the average would be a smear, so the blend is let go
// exactly there. Analogue noise, which is uncorrelated between frames, goes the
// same way for free.
// How many frames comes from the source rather than from a textbook. Measured on
// a PAL signal through this card, the mean frame to frame difference of a still
// picture runs 1.91 at a lag of one frame, 2.62 at two, 1.90 at three and 0.78
// at four -- a clean four frame cycle, which is the PAL subcarrier walking
// through its sequence. Averaging two frames there cancels little; it picks two
// points of that cycle. Averaging four covers it exactly, and what is left is
// the picture. A textbook NTSC signal is back after two, and a console that
// picks its own clock can land on three, so the renderer measures the cycle of
// whatever is plugged in and hands it over as gCrawlCycle. Until it has, the
// average covers four frames, as it always did.
//
// The correction is computed on the woven pixels and then applied to whatever
// the deinterlacer produced, so cleaning the picture never undoes the
// reconstruction.
// What one crawl cycle of averaging would change about one source pixel. Kept
// apart from the decision of whether to apply it, because a deinterlaced pixel does
// not come from the row it is drawn on -- and the pattern has no vertical
// correlation whatsoever, so a correction taken from the neighbouring line does
// not remove the crawl, it lays a second one on top. That is why the filter
// worked with deinterlacing off and fell apart with it on.
float3 TemporalDelta(int x, int row) {
  float3 f0 = FetchRgbFrame(int2(x, row), 0);
  float3 sum = f0 + FetchRgbFrame(int2(x, row), 1);
  if (gCrawlCycle >= 3) sum += FetchRgbFrame(int2(x, row), 2);
  if (gCrawlCycle >= 4) sum += FetchRgbFrame(int2(x, row), 3);
  return sum / (float)gCrawlCycle - f0;
}

// How much of that correction to trust here: everything where the picture is
// standing still, nothing where it is moving.
float TemporalGate(int x, int row) {
  // How much this spot actually moved, as opposed to shimmered -- and the
  // difference between those two is the whole problem. Dot crawl crawls: it is
  // not still, so any detector that simply asks "did this pixel change" calls it
  // movement and switches the filter off exactly where the artefact is. Measured
  // that way the filter took 48 % off the mild half of the picture and 15 % off
  // the worst one per cent, which is the wrong way round and invisible.
  //
  // What separates them is not amount but scale. The shimmer is a fine pattern,
  // a few pixels across -- that is what makes it look like dots -- while a
  // picture that is really moving moves in broad shapes. So the decision is made
  // on a smoothed picture, where the shimmer has averaged itself away before it
  // is ever asked about and real movement comes through untouched.
  //
  // Across the line the window is exactly two cycles of the subcarrier wide,
  // edges weighted by how much of them it covers, the same as BoxLuma below: a
  // box that wide has a null right on the carrier, on every standard. It used to
  // be seven samples, taken for "a little over two cycles", and that is the
  // trouble: two cycles are 6.09 samples on PAL, and seven let 14 % of the crawl
  // through (9 % on NTSC). Down the picture it takes three lines, because the
  // crawl has no correlation from one line to the next and movement does, so
  // what leaks at all is cut again.
  //
  // But three lines on their own are blind to the thinnest things there are: a
  // line one pixel high that appears or goes shows at a third of its contrast,
  // where one line saw all of it. That is what left the rim of a menu's
  // selection ring standing for three frames after the ring had moved on -- top
  // and bottom, where the rim runs along the line, which the seven samples had
  // caught. So the centre line is asked on its own as well, at 0.7 of what it
  // says: nearly all of a thin line, and little enough of the crawl a single
  // line lets through that the three lines keep most of their gain.
  //
  // The leak was worst with Avoid Ghosting on, and worst on the most detailed
  // parts of a still picture: the steep gate read the leaked crawl as movement
  // and let go exactly where the crawl was strongest. Measured on 152 frames of
  // a PAL menu from this card, a held picture next to a scrolling background:
  // of the correction the hardest crawling 5 % of the held picture should get,
  // the seven samples withheld 14 % and this withholds 10 % -- off, 3.2 % and
  // now 2 %. And it ghosts less for it: the area where the average smears more
  // than six levels into something that moved goes from 0.93 % to 0.61 % (off,
  // 3.1 % to 1.9 %), the worst smear from 34 levels to 20 (off, 35 to 27).
  //
  // And it is asked of the colour as well as the brightness, which it was not
  // at first. Written on brightness alone this missed the one thing it most
  // needed to catch. A caption in yellow with a red outline on a dark ground
  // moves its colour far more than its brightness: from the ground to the
  // outline the red channel steps by four fifths of the range while the
  // brightness steps by a quarter of it, because red carries less than a third
  // of the brightness. So the gate read a quarter, called it standing still,
  // and let the average smear an outline that had plainly moved.
  //
  // Measured on a pulsing menu caption, four frames at a time: the pixels that
  // ghosted showed 7/255 to a test on brightness while the correction let
  // through at those same pixels was up to 60/255. Reading all three the worst
  // ghost drops from 54 to 30 out of 255 and the area that shows one at all
  // from 3.8 % to 0.8 %, at a cost of 0.95 to 0.92 in how much of the
  // correction a standing picture keeps. That last number is the one to watch:
  // it is the crawl removal this filter exists for, and it barely moved.
  //
  // Seven samples on three lines over four frames on PAL, 84 loads where there
  // were 28, all in the pass that already runs; nine a line on NTSC. The centre
  // line's reading is kept aside on the way and costs no load of its own. The
  // width is capped so that a composite picture some scaler has spread over a
  // wide raster does not multiply that.
  float reach = clamp(gCarrierPeriod, 1.0, 6.0);
  int whole = (int)floor(reach - 0.5 + 1e-4);
  float frac = reach - 0.5 - float(whole);
  float3 s0 = float3(0.0, 0.0, 0.0), s1 = s0, s2 = s0, s3 = s0;
  float3 c0 = s0, c1 = s0, c2 = s0, c3 = s0;
  for (int dy = -1; dy <= 1; ++dy) {
    float3 r0 = float3(0.0, 0.0, 0.0), r1 = r0, r2 = r0, r3 = r0;
    for (int dx = -whole - 1; dx <= whole + 1; ++dx) {
      float w = (abs(dx) <= whole) ? 1.0 : frac;
      int2 q = int2(x + dx, row + dy);
)HLSL"
R"HLSL(      r0 += GateSampleAt(q, 0) * w;
      r1 += GateSampleAt(q, 1) * w;
      if (gCrawlCycle >= 3) r2 += GateSampleAt(q, 2) * w;
      if (gCrawlCycle >= 4) r3 += GateSampleAt(q, 3) * w;
    }
    s0 += r0; s1 += r1; s2 += r2; s3 += r3;
    if (dy == 0) { c0 = r0; c1 = r1; c2 = r2; c3 = r3; }
  }
  // Only the frames the average takes in. One it leaves out may move all it
  // likes; standing in for it, the current frame changes neither end of the
  // range.
  if (gCrawlCycle < 3) { s2 = s0; c2 = c0; }
  if (gCrawlCycle < 4) { s3 = s0; c3 = c0; }
  float width = float(2 * whole + 1) + 2.0 * frac;
  float3 d = (max(max(s0, s1), max(s2, s3)) - min(min(s0, s1), min(s2, s3))) / (3.0 * width);
  float3 e = (max(max(c0, c1), max(c2, c3)) - min(min(c0, c1), min(c2, c3))) * (0.7 / width);
  // Whichever of the three moved furthest, not their sum: a shift that shows
  // in one channel is a shift, and averaging it with two that stood still
  // would talk it back down to nothing.
  float moved = max(max(d.x, max(d.y, d.z)), max(e.x, max(e.y, e.z)));
  // Where the gate lets go, which is the whole trade and therefore a setting.
  //
  // Held on: five levels out of 255 of slack, fully let go at 18. That is
  // deliberately late, because the artefact this filter exists for moves by
  // itself -- let go early and the filter switches off exactly where the crawl
  // is. The price is that a slowly moving edge is averaged with three older
  // copies of itself, which is the ghosting.
  //
  // Let go early: from four levels, fully let go at nine. Moving edges come out
  // clean; slow parts of the picture keep some of their crawl, where the
  // demodulator below picks the work back up.
  return 1.0 - saturate((moved - gMotionSlack) * gMotionSlope);
}

// The spatial half of the same job, for everything the temporal filter cannot
// reach. Averaging frames only works where the picture stands still; in a
// running game most of the screen does not, and there the crawl stays.
//
// Measured on this card, the crawling pattern repeats every three pixels across
// a line -- the horizontal autocorrelation runs -0.35, -0.37, +0.43 at one, two
// and three pixels -- and shows no correlation at all from one line to the next.
// That is the colour subcarrier, 4.43 MHz against roughly 13.5 MHz of sampling,
// and it means a three sample average has its null exactly on top of it.
//
// So the pattern is what a three tap average removes: the residual between a
// pixel and that average is the crawl. Subtracting all of it would soften every
// sharp vertical edge as well, because a real edge has a residual too -- a much
// larger one. Hence the falloff: small residuals, which is all the crawl ever
// is, are taken out completely, and the bigger a residual gets the more of it
// survives.
//
// Costs a little horizontal sharpness at strong edges. That is the price of
// cleaning a picture that is moving, and it is why this is a separate knob.
// Dot crawl is the colour subcarrier leaking into brightness, so the way to
// take it out is to treat it as what it is: a carrier of known frequency,
// amplitude modulated by however much colour is at that point in the picture.
//
// A plain notch cannot do this well. Measured on this card, isolating the
// pattern through its own four frame cycle and looking at its spectrum, a band
// of thirty-two bins around the peak holds under half its energy -- the
// modulation smears it far too wide for any narrow filter to catch, and a wide
// one takes the picture with it.
//
// Synchronous detection does not care. Multiplying the line by the carrier and
// by the same carrier a quarter cycle over moves the pattern down to nothing,
// where a short average recovers exactly how much of it is there; multiplying
// back up reconstructs it, and it is subtracted. Picture detail is not tied to
// the carrier's phase and survives, apart from whatever happened to sit right on
// the frequency.
//
// Measured against this card's own frames, at a window of nine samples that
// removes 81 % of the pattern for 17 % of the horizontal sharpness, where the
// notch it replaces managed 67 % for 18 %. At five samples it reaches 99 %.
float DotDemodDelta(int x, int row) {
  // The slider is the window: wide and gentle at the left, narrow and thorough
  // at the right. It is counted in cycles of the subcarrier rather than in
  // samples, because the two are not the same thing across standards: NTSC
  // carries 3.77 samples per cycle where PAL carries 3.04, so a window fixed at
  // a pixel count hands NTSC a fifth fewer cycles to detect with. Measured
  // against an amplitude modulated pattern, that cost it 61 % removed at the far
  // end of the slider where PAL managed 69 %, and it invented a quarter more
  // pattern in the process. Counted in cycles they land at 67 % and 72 %.
  //
  // The same reasoning covers capture widths other than 720, which is why
  // gCarrierPeriod is scaled by the source width before it arrives here.
  float cycles = 8.2 - gDotNotch * 5.9;  // a little over eight, down to just over two
  int r = (int)floor(cycles * gCarrierPeriod * 0.5 + 0.5);
  r = clamp(r, 2, 12);

  const float kTwoPi = 6.28318531;
  float w = kTwoPi / max(gCarrierPeriod, 1.5);

  // Two passes over the window. The first is only there to find the local
  // average, and it is not optional: the carrier does not sum to zero over a
  // window of a few samples, so a flat area of picture correlates with it and
  // gets a pattern invented on top of itself. Measured on a constant field of
  // 128, leaving this out produced swings of ninety levels -- which is what
  // "the picture goes oddly bright" was.
  float mean = 0.0, norm = 0.0;
  for (int k = -12; k <= 12; ++k) {
    if (abs(k) > r) continue;
    // Raised cosine over the window, so the ends do not ring.
    float hann = 0.5 + 0.5 * cos(3.14159265 * float(k) / float(r + 1));
    mean += hann * Luma(FetchRgbAt(int2(x + k, row)));
    norm += hann;
  }
  if (norm <= 0.0) return 0.0;
  mean /= norm;

  float acc = 0.0, accQ = 0.0;
  for (int k = -12; k <= 12; ++k) {
    if (abs(k) > r) continue;
    float hann = 0.5 + 0.5 * cos(3.14159265 * float(k) / float(r + 1));
    float l = Luma(FetchRgbAt(int2(x + k, row))) - mean;
    float ph = w * float(x + k);
    acc += hann * l * cos(ph);
    accQ += hann * l * sin(ph);
  }

  float ph0 = w * float(x);
  float pattern = 2.0 * (acc * cos(ph0) + accQ * sin(ph0)) / norm;

  // Subtracting the same amount from all three channels moves brightness and
  // leaves colour alone, which is the half of the picture this belongs to.
  return -pattern;
}
)HLSL"
R"HLSL(// Brightness averaged over a window exactly one cycle of the subcarrier wide.
//
// The width is not a round number and must not be rounded to one. A box of N
// samples has its first null at 1/N, so a box of exactly gCarrierPeriod samples
// puts a null precisely on the carrier -- for every standard, without a table.
// Rounded to three samples it would land on 1/3 of the sampling rate, which is
// the carrier on PAL and nowhere near it on NTSC, where the period is 3.77.
//
// So the two samples at the edge of the window count by however much of them
// the window actually covers. Everything that uses this wants the same thing
// from it: the picture with the crawl taken out, and nothing else changed.
//
// The loop is bounded by the carrier rather than by a number, and that is not
// cosmetic either. Written with a constant limit the compiler unrolls it, and
// every copy carries a pixel format branch and a four way choice of frame with
// it; with the search below calling this a hundred times over, that took the
// startup compile from a third of a second to three and a half. Bounded by
// something out of the constant buffer it has to emit a real loop, which costs
// nothing at runtime and compiles in a fraction of the time -- the same trade
// SoftenChroma above is written for.
float BoxLuma(int x, int row, int frame, float width) {
  float half = max(width, 2.0) * 0.5;
  int whole = (int)floor(half - 0.5 + 1e-4);
  float frac = half - 0.5 - float(whole);
  float s = LumaFrameAt(int2(x, row), frame);
  float n = 1.0;
  for (int k = 1; k <= whole + 1; ++k) {
    float w = (k <= whole) ? 1.0 : frac;
    s += (LumaFrameAt(int2(x - k, row), frame) + LumaFrameAt(int2(x + k, row), frame)) * w;
    n += 2.0 * w;
  }
  return s / n;
}

// ---------------------------------------------------------------------------
// Following the movement, for the noise the gate above cannot reach.
//
// The four frame average only runs where the picture stands still, so in a
// running game most of the screen gets no temporal filtering at all and keeps
// every bit of the noise the analogue chain put on it. Averaging along the
// movement instead of across it fixes that.
//
// What it cannot fix is the dot crawl, and that is worth writing down because
// it looks like it should. The crawl is fixed to the raster, not to the
// picture: at sample x it carries the phase w*x + phi_f, where phi_f walks the
// four frame sequence and the four terms sum to zero. Fetch frame f from
// x - d_f instead and the phase becomes w*x - w*d_f + phi_f, so the sum turns
// into
//
//     sum over f of exp(i*(phi_f - w*d_f))
//
// and with a constant velocity d_f = f*v that is a geometric series in
// exp(i*(pi/2 - w*v)). A geometric series of four terms vanishes only when its
// ratio is a fourth root of unity other than one, which here means v has to be
// a multiple of a quarter of the carrier period -- about 0.76 samples a frame
// on PAL, 0.94 on NTSC. At every other speed motion compensation destroys the
// very cancellation the average exists for, and at exactly a quarter period it
// lines all four frames up in phase and removes nothing at all.
//
// So the two jobs are separated rather than merged. The co-located average
// keeps the crawl, exactly as before; this one is band limited to stay out of
// its way, and what it removes is noise. Noise is the same defect in every
// colour system, which is why this one knob helps NTSC, PAL, PAL M, PAL N and
// SECAM alike -- and why the crawl work below it still has to be told which
// carrier it is looking for.
//
// It pays a second time, indirectly: the demodulator estimates how much pattern
// is present from the pixels in front of it, and noise is what makes that
// estimate wrong. A cleaner line means a better estimate means less of the
// picture taken away with the pattern.

// Three points of the line, far enough apart to describe a shape rather than a
// sample. Taken from the current frame once and handed to every candidate,
// because recomputing them per candidate was the whole cost.
float3 MatchPatch(int x, int row, float width) {
  return float3(BoxLuma(x - 4, row, 0, width), BoxLuma(x, row, 0, width),
                BoxLuma(x + 4, row, 0, width));
}

// The mean of the three, so the number means the same thing whatever is asked
// of it and one threshold can be written down for all of them. The shift is in
// samples along the line and in rows of the frame.
float MatchCost(float3 cur, int x, int row, int2 d, int frame, float width) {
  int r = row - d.y;
  return (abs(cur.x - BoxLuma(x - 4 - d.x, r, frame, width))
        + abs(cur.y - BoxLuma(x - d.x, r, frame, width))
        + abs(cur.z - BoxLuma(x + 4 - d.x, r, frame, width))) * 0.3333333;
}

// A match this good is a match, not the best of several poor ones. The right
// shift on a card's usual noise, two levels in 255, costs about half a
// hundredth; this sits a little above that.
static const float kMatchGood = 0.007;

// A match this poor is none at all. The right shift on that noise scatters by
// about a quarter of a hundredth from pixel to pixel, and this sits three times
// that above it, so a picture that only shivers with noise almost never gets
// here. It has to: pixels are shaded in groups that run the loop together, and
// a group pays for one pixel searching further as if all of them did. With the
// line drawn at kMatchGood instead, nearly every group on a picture standing
// still or moving sideways paid for the search up and down and found nothing.
static const float kMatchPoor = 0.012;

// Along the line first, by a logarithmic search: start at standing still, then
// halve the step three times. Seven candidates cover plus or minus seven
// samples, which at fifty or sixty frames a second is a faster scroll than
// anything a console produces.
//
// Up and down only where that found no match at all. A picture moving
// vertically has no shift along the line that fits it, but over any texture
// one of seven candidates fits a little better than standing still by
// accident, and following that averages the wrong lines together: with the
// search along the line alone, a vertical scroll over fine detail came out
// noisier than it went in. So the same search runs up and down from wherever
// the first one ended, in lines of one field -- two rows of a woven frame,
// because the row between belongs to the other field and to another moment.
// Starting there rather than at standing still also finds a diagonal.
//
// A shift up or down is followed only when it is good by the measure above.
// The lines of one field lie twice as far apart as the samples of a line,
// detail changes more from one to the next, and a near miss up or down costs
// more than one sideways. A vertical answer that is merely the best of poor
// ones is taken as movement the search cannot follow, and then nothing is
// corrected at all -- the accidental shift along the line included, which is
// what made such a scroll noisier before.
//
// Only the nearest frame is searched, and the two behind it are then asked
// separately whether the answer fits them too. That is one search instead of
// three, and it is the checking below rather than the search that decides what
// survives.
//
// Both candidates of a pass are measured against the same starting point, not
// against each other: taking the first improvement and stepping off it turns
// the search into a greedy walk that can wander away from the minimum it was
// closing in on.
//
// The six passes run in one loop with one call of the cost function, rather
// than as two searches written out: every call inlines three BoxLuma, and the
// startup compile grows with each copy (see BoxLuma).
int2 SearchShift(float3 cur, int x, int row, float width, float still, out float cost) {
  int2 best = int2(0, 0);
  cost = still;
  // Not "pass": that is a keyword to fxc, though dxc takes it.
  [loop] for (int stage = 0; stage < 6; ++stage) {
    if (stage == 3 && cost <= kMatchPoor) break;
    // Steps 4, 2, 1 along the line, then 4, 2, 1 field lines up and down.
    int step = int(4) >> (stage < 3 ? stage : stage - 3);
    int2 dir = stage < 3 ? int2(1, 0) : int2(0, gMotionRows);
    int2 base = best;
    [loop] for (int s = -1; s <= 1; s += 2) {
      int2 d = base + dir * (s * step);
      float c = MatchCost(cur, x, row, d, 1, width);
      if (c < cost) { cost = c; best = d; }
    }
  }
  // Moved up or down, and not good enough to follow: reported as no better
  // than standing still, which the caller drops.
  if (best.y != 0 && cost > kMatchGood) cost = still;
  return best;
}

// Where a match stops being believable. A mean absolute difference of a
// smoothed line, so a well matched patch on a noisy signal lands a good deal
// under a hundredth; by three hundredths there is something in the way that
// moving the window did not explain, and averaging across it would smear
// rather than clean.
static const float kMatchGiveUp = 0.03;

// How much better than standing still a shift has to be before it is believed.
// The search always returns something, and on a picture that only shivers with
// noise the something is noise: taking the smallest of seven noisy numbers
// finds one about a third of the noise below the honest one, every time, and
// following that lines the noise up and averages it into the picture instead of
// out of it. The margin is set above that much luck.
static const float kMatchMargin = 0.005;

// The largest correction this filter may make, per channel. It is here to take
// noise off a moving picture, and noise is small; a correction bigger than this
// is not noise but a mismatch, and a mismatch held small is invisible where a
// mismatch let through whole is a ghost. Every test above can still be fooled
// by the right piece of picture -- this is the line that decides what being
// fooled costs.
static const float kMotionClamp = 0.06;

float3 MotionCompDelta(int x, int row) {
  float width = max(gCarrierPeriod, 2.0);
  float3 cur = MatchPatch(x, row, width);

  // Where standing still already fits within the margin, no shift can beat it
  // by the margin, and the search is skipped rather than asked to fail.
  float still = MatchCost(cur, x, row, int2(0, 0), 1, width);
  if (still <= kMatchMargin) return float3(0.0, 0.0, 0.0);
  float cost;
  int2 d = SearchShift(cur, x, row, width, still, cost);
  if (cost > still - kMatchMargin) return float3(0.0, 0.0, 0.0);

  // Each frame is then asked on its own whether that shift explains it, rather
  // than the nearest one answering for all three. This is the part that decides
  // whether the filter cleans a moving picture or damages one.
  //
  // The shift is measured between this frame and the last, and carried on as
  // f*d for the two behind it -- a constant velocity, which is what a scroll is
  // and what very little else is. On an interlaced source the third frame back
  // is a tenth of a second old. An object that turns, stops, speeds up, or is
  // passed in front of by another one is not where f*d says it is, and taken on
  // trust it arrives as half the output pixel fetched from somewhere else
  // entirely. That is a trailing ghost, and it lands exactly where the picture
  // moves, which is the only place this filter runs at all.
  //
  // Measuring each frame costs two more cost functions and settles all of it at
  // once. Acceleration drops the far frames and keeps the near one. A turn has
  // no shift that fits the frames behind, so they drop. An edge that uncovers
  // background drops whichever frames were still covered, because there no
  // shift can be right: the pixel was not in them. A vertical shift stays in
  // step with the fields at every frame, since f times an even number of rows
  // is still even.
  float3 w;
  w.x = saturate(1.0 - cost / kMatchGiveUp);
  w.y = saturate(1.0 - MatchCost(cur, x, row, 2 * d, 2, width) / kMatchGiveUp);
  w.z = saturate(1.0 - MatchCost(cur, x, row, 3 * d, 3, width) / kMatchGiveUp);
  float wsum = w.x + w.y + w.z;
  if (wsum <= 0.0) return float3(0.0, 0.0, 0.0);

  // The correction, smoothed over that same one period window. This is what
  // keeps the two halves apart: the window's null sits on the carrier, so
  // nothing this returns can add to or take from the crawl, and neither the
  // average above nor the demodulator below can be undone by it.
  float half = width * 0.5;
  int whole = (int)floor(half - 0.5 + 1e-4);
  float frac = half - 0.5 - float(whole);

  float3 sum = float3(0.0, 0.0, 0.0);
  float n = 0.0;
  for (int k = -(whole + 1); k <= whole + 1; ++k) {
    float wk = (abs(k) <= whole) ? 1.0 : frac;
    int2 q = int2(x + k, row);
    float3 f0 = FetchRgbAt(q);
    float3 acc = f0 + FetchRgbFrame(q - d, 1) * w.x
                    + FetchRgbFrame(q - 2 * d, 2) * w.y
                    + FetchRgbFrame(q - 3 * d, 3) * w.z;
    sum += (acc / (1.0 + wsum) - f0) * wk;
    n += wk;
  }

  // And the last word regardless of everything above it: see kMotionClamp.
  return clamp(sum / max(n, 1e-4), -kMotionClamp, kMotionClamp);
}
)HLSL"
R"HLSL(// ---------------------------------------------------------------------------
// Putting back the top of the brightness band, which the transmission rolled
// off and which is therefore known rather than guessed.
//
// Composite carries brightness up to where the colour subcarrier sits, and no
// encoder or decoder has a brick wall there -- both roll off gently towards it,
// and the decoder's chroma trap takes the rest. That rolloff is the softness a
// composite picture has and a component one does not, and because its shape
// follows the carrier it can be undone with a filter built from the carrier.
//
// This is not the sharpening on the Scaling section. That one runs after
// scaling, on whatever size the window happens to be, and raises contrast at
// edges that are already there -- it makes the picture look sharper. This one
// runs in the source's own samples with a kernel derived from the standard, and
// lifts a band of frequencies that the chain actually attenuated.
//
// The band is picked out by the difference of two triangular windows: one a
// carrier period wide, one two. A triangle is a box convolved with itself, so
// its response is a squared sinc -- it has the same null on the carrier a box
// of that width has, but a null of second order, and no sidelobe anywhere near
// it. That is the property this whole pass stands on.
//
// It was written with plain boxes first, and that was wrong, in a way worth
// leaving on the record because the reasoning sounded complete. A box does have
// a null on the carrier. But dot crawl is not a line at the carrier, it is the
// chroma band leaking into brightness, and that band is a good megahertz wide
// on either side of it. A box is only zero at the point: on PAL the lower
// chroma edge sits at seven tenths of the carrier, and there the difference of
// two boxes measures 0.62 against a passband peak of 0.81 -- it was passing
// three quarters of exactly the thing two filters upstream had just spent their
// effort removing. With triangles the same point measures 0.108 against a peak
// of 0.545, and from eight tenths of the carrier upward it is under 0.03. The
// stopband is a region now instead of a point.
//
// An ordinary unsharp mask has neither, rising with frequency all the way to
// Nyquist, which is the one shape this pass must never have.
//
// And the triangles alone are still not enough, which took a picture to notice
// rather than a derivation. A triangle's response is a squared sinc, and a
// squared sinc has sidelobes -- on PAL the first one lands almost exactly on
// Nyquist, where the difference measures 0.100, as much as the chroma edge. The
// wide triangle is near a null of its own there and cancels none of it. So the
// filter was lifting the finest pattern the sampling grid can hold by a quarter
// at full strength, which in a flat saturated area is visible as a fine crawl
// that no dot crawl filter can reach: it is not at the carrier, so neither the
// four frame average nor the demodulator can see it, and it was being added
// after both of them had run.
//
// The cure is one convolution: every weight below is the triangle smoothed with
// [1, 2, 1] / 4, which is zero at Nyquist exactly and by construction. Measured
// on the normalised weights, PAL: Nyquist 0.100 -> 0.000, the chroma edge at
// seven tenths 0.108 -> 0.061, eight tenths 0.029 -> 0.013, and the passband
// peak 0.545 -> 0.471, which the scale below takes back. NTSC never had the
// Nyquist problem -- its carrier sits lower, so the sidelobe falls inside the
// band rather than on the edge of it -- and gains the same in the stopband.
//
// Horizontal only, and that is not a shortcut. The band limit is a limit along
// the line. Vertically the picture is limited by how many lines the standard
// has, which no filter can undo, and reaching vertically would sharpen the line
// structure and fight the deinterlacer in the pass after this one.
// `carrierMag` is the raw carrier amplitude CarrierEnergy below measures at this
// pixel -- the amplitude, not the judgement it also returns, because the gate
// further down needs to weigh it against the local excursion rather than against
// a fixed threshold. Computed once in main and handed to whoever needs it.
)HLSL"
R"HLSL(float Triangle(float t, float slope) { return max(0.0, 1.0 - t * slope); }

// The same triangle smoothed with [1, 2, 1] / 4. Written on the weight rather
// than on the samples, which is the same filter and costs no extra fetches.
float SmoothTriangle(float k, float slope) {
  return (Triangle(abs(k - 1.0), slope) + 2.0 * Triangle(abs(k), slope) +
          Triangle(abs(k + 1.0), slope)) * 0.25;
}

)HLSL"
R"HLSL(float3 BandwidthRestore(float3 rgb, int x, int row, float carrierMag) {
  float width = max(gCarrierPeriod, 2.0);
  float slopeA = 1.0 / width;              // a triangle one carrier period wide
  float slopeB = 0.5 / width;              // and one of two
  int r = (int)ceil(2.0 * width) + 1;      // the wider of them, plus the smoothing

  float y = Luma(rgb);
  float sa = 0.0, na = 0.0, sb = 0.0, nb = 0.0;
  float lo = y, hi = y;                    // the sample either side: the bound
  float wlo = y, whi = y, step = 0.0;      // the narrow window: the measurement
  float prev = 0.0;
  bool havePrev = false;
  for (int k = -r; k <= r; ++k) {
    float l = Luma(FetchRgbAt(int2(x + k, row)));
    float t = abs(float(k));
    float wa = SmoothTriangle(float(k), slopeA);
    float wb = SmoothTriangle(float(k), slopeB);
    // What the limiter at the end goes by: the sample either side, and no
    // further. The narrow window was tried there first and is too wide to hold
    // anything -- it is a carrier period, three samples on PAL, so a pixel three
    // samples into a bright plateau still has the dark side of the edge inside
    // it, the bound opens to the full step, and the overshoot lobe sits exactly
    // there.
    if (t <= 1.0) { lo = min(lo, l); hi = max(hi, l); }
    // Over one carrier period, how far the line travels and the largest distance
    // it covers between two neighbouring samples. Both are needed below, and
    // only their ratio is used, so neither has to mean anything on its own. The
    // width is stated rather than taken from `wa > 0`, so that changing the
    // kernel above cannot quietly move the ruler this measures with.
    if (t <= width) {
      wlo = min(wlo, l);
      whi = max(whi, l);
      if (havePrev) step = max(step, abs(l - prev));
      prev = l;
      havePrev = true;
    }
    sa += l * wa;
    na += wa;
    sb += l * wb;
    nb += wb;
  }
  float narrow = sa / max(na, 1e-4);
  float wide = sb / max(nb, 1e-4);

  // At the peak of that band the difference of the two windows comes to 0.471 of
  // the signal, and this used to stand at 2.8 -- the top of the slider a little
  // past double -- because the limiter below was cutting six sevenths of it away
  // again and the number had to cover that. It no longer does, so the same
  // slider position would now arrive four to five times too strong.
  //
  // Matching the two on the passband alone put this at 0.65, and that was the
  // wrong band to match on. Sharpness is not read there. At equal passband the
  // old filter carried three and a half times as much at 0.71 of the carrier and
  // twelve times as much at 0.90, and that is the band an eye calls sharp -- so
  // the two measured identical and looked nothing alike.
  //
  // Matched on 0.71 instead it takes about four, and that is the top of the
  // useful range rather than a setting anyone wants: judged on picture it starts
  // inventing carrier residue well before it. Half of that is the top of the
  // slider, so the whole travel is usable. The old one was dead above a fifth --
  // the clamp saturated and turning it further bought nothing but more of the
  // pattern it drew -- and this is close to linear.
  float add = (narrow - wide) * gBandwidth * 2.0;

  // And the part the shape of the filter cannot do. The band that composite
  // rolled off and the band its colour crosstalk lives in are the same band --
  // that is not a flaw in any particular filter, it is what putting colour on a
  // brightness carrier means, and no fixed kernel gets round it. So ask what is
  // actually here: where the line carries energy at the carrier's own frequency
  // there is no telling detail from crawl, and lifting is a coin toss played
  // against two filters upstream, so do not lift. Where it does not, the
  // softness is genuinely rolloff and gets its full lift back.
  //
  // What that question must not be is an absolute one, and it was. CarrierEnergy
  // answers `saturate(mag * 12)`, a threshold set for a different job -- whether
  // there is enough carrier detail here to suspect the *colour* over -- and a
  // pattern has to be strong before that is worth doing. A flat saturated area
  // carries a residue far below it: the decoder's notch left a little carrier
  // behind, there is nothing else in that band because the area is flat, and the
  // gate read it as near zero and opened all the way. The filter then lifted the
  // one thing in the neighbourhood, which was the residue, by its full amount.
  // On an even purple that is the entire visible pattern -- switch the filter off
  // and it is gone, because the filter is what drew it -- and in a yellow letter
  // it is a residue you have to look for turned into one you do not.
  //
  // The scale-free question instead: what share of the local excursion *is*
  // carrier? `mag` is the amplitude of that component, so a line carrying
  // nothing but carrier spans two of it, and the share reads a half. Real detail
  // spans many times its own carrier content and reads a few hundredths. The
  // reading no longer depends on how strong the pattern is in absolute terms --
  // which is the whole point, because on a flat area it is weak and it is still
  // all there is.
  //
  // No flat part to the ramp, which is the part that took measuring. Full lift
  // up to a fifth and closed at a half was the obvious shape and it is the wrong
  // one: it hands a free pass to everything under the plateau, and a fifth of
  // the local excursion being carrier is already most of what a flat area has.
  // Splitting the static picture into places where the source is flat -- where
  // whatever comes back is invented, because there was no detail to restore --
  // and places where it has structure, and asking each separately: a plateau at
  // any height lifts the flat half by nearly as much as no gate at all, while a
  // ramp that starts at zero costs the structured half about a tenth and takes
  // the flat half from 1.71 down to 1.30. Nothing here is free of carrier, so
  // nothing should be exempt from the question.
  float share = carrierMag / max(whi - wlo, 1e-4);
  add *= saturate((0.24 - share) / 0.24);

  // And the question the limiter cannot answer, because by the time it runs the
  // lift already exists: was anything lost here at all?
  //
  // A rolloff has a width. Brightness limited to where the subcarrier sits
  // cannot cross from black to white inside one sample -- it needs two or three,
  // and that spread is the entire thing this filter undoes. So a transition that
  // *does* cross in one sample was never rolled off. It was drawn after the band
  // limit, or it survived the chain intact, and either way there is nothing
  // underneath it to put back. Lifting the band there does not restore contrast,
  // it manufactures it, and the visible result is the one edge case the limiter
  // is blind to: the pixel beside a dark outline gets pulled down towards the
  // outline, which is inside its neighbours' range the whole way, so nothing
  // stops it and the outline simply comes out a sample wider on each side.
  //
  // `step / (whi - wlo)` is that width, measured rather than assumed: the share
  // of the local travel that one sample already covers. A third or so for an
  // edge the chain actually softened, one whole for an edge that is already
  // hard. Below half, full lift; from there it closes, and by three quarters
  // this does nothing at all.
  add *= saturate((0.75 - step / max(whi - wlo, 1e-4)) * 4.0);

  // Bounded by what the line actually holds around here -- and bounded against
  // the neighbourhood, not against the size of the correction, which is the
  // whole of the difference.
  //
  // A ceiling on |add| does not know which way the pixel is being pushed. On the
  // dark side of an edge the lift is negative and a symmetric ceiling lets it
  // run below everything nearby; on the bright side it runs above. That pair is
  // exactly what a halo is: a shadow beside the edge and a bright line on the
  // other side of it. It stood at a quarter of the local excursion here, which
  // at a black to white edge permits a quarter of full scale in each direction,
  // several times what is visible.
  //
  // Sharpen() further down has always been bounded the other way -- clamped into
  // the min and max of its own immediate neighbours, itself included -- and does
  // not ring. This is that rule, on the axis this filter works along.
  //
  // Drawn straight through the neighbours, though, it stops being a safety net
  // and becomes the operator. Fit the slider on both so they reach the same
  // sharpening in the passband and it takes 1.30 with the bound hard against the
  // neighbours against 0.20 without one: six sevenths of the correction is being
  // cut away, so the output is sitting *on* the bound nearly everywhere. That is
  // not filtering any more, it is snapping each pixel to whichever neighbour is
  // nearer -- a decision taken per pixel, which writes a new signal at pixel
  // rate, which is broadband. On a flat saturated area the thing it snaps to is
  // the carrier residue. Measured at the subcarrier's own frequency the filter
  // was adding 32 times the power the source has there, and that floor is the
  // whole of the fine pattern visible on an even purple or inside a yellow
  // letter -- switch the filter off and it goes, because the filter drew it.
  //
  // So keep the bound and allow a fixed overshoot past it. Where the line is
  // flat the excursions are far under the allowance, the bound never engages at
  // all, and there is nothing to decide and nothing broadband. Where there is an
  // edge it still caps the halo, at the allowance and no further. At 6/255 that
  // floor drops from 32 to 2.7 and no pixel anywhere overshoots by more than the
  // allowance; dropping the bound entirely reaches 1.6, but then 1.6% of the
  // picture runs past 12/255 and the worst of it reaches 33.
  float slack = 6.0 / 255.0;
  float lifted = clamp(y + add, lo - slack, hi + slack);
  return rgb + (lifted - y);
}

// ---------------------------------------------------------------------------
// How much of the brightness right here is sitting at the subcarrier's own
// frequency -- which is the thing that turns into false colour.
//
// The decoder cannot tell dense brightness detail at the carrier from an actual
// colour. It demodulates whatever is there and paints it, and that is the
// rainbow over a pinstripe, a dither pattern, a brick wall. So where this reads
// high the colour is suspect and wants softening; where it reads low the colour
// is real and softening it only throws detail away.
//
// Same detection as the demodulator below, over a shorter window, and it costs
// one pass rather than two: the local mean is subtracted afterwards instead of
// beforehand, using the fact that the sum of (l - mean)*cos is the sum of l*cos
// minus mean times the sum of cos.
//
// Returns both halves of the answer. `.y` is the judgement above, for softening
// the colour. `.x` is the amplitude it was formed from, because the bandwidth
// filter asks a different question of the same measurement -- not "is there
// enough of this to act on" but "what share of what is here is this", which
// needs the number before a threshold was put on it.
)HLSL"
R"HLSL(float2 CarrierEnergy(int x, int row) {
  const float kTwoPi = 6.28318531;
  float w = kTwoPi / max(gCarrierPeriod, 1.5);
  int r = (int)floor(gCarrierPeriod + 0.5);   // two cycles, near enough
  r = clamp(r, 2, 8);

  float sl = 0.0, nrm = 0.0;
  float ac = 0.0, aq = 0.0, wc = 0.0, wq = 0.0;
  for (int k = -8; k <= 8; ++k) {
    if (abs(k) > r) continue;
    float hann = 0.5 + 0.5 * cos(3.14159265 * float(k) / float(r + 1));
    float l = Luma(FetchRgbAt(int2(x + k, row)));
    float ph = w * float(x + k);
    float c = cos(ph), s = sin(ph);
    sl += hann * l;
    nrm += hann;
    ac += hann * l * c;
    aq += hann * l * s;
    wc += hann * c;
    wq += hann * s;
  }
  if (nrm <= 0.0) return float2(0.0, 0.0);
  float mean = sl / nrm;
  float re = (ac - mean * wc) / nrm;
  float im = (aq - mean * wq) / nrm;
  float mag = 2.0 * sqrt(re * re + im * im);

  // Where that amount of carrier band detail is enough to be worth suspecting
  // the colour over. A flat area reads near zero; a dithered gradient or a
  // pinstripe -- the patterns that produce the rainbow in the first place --
  // reach several hundredths, so a twelvefold scale saturates on exactly the
  // material this is for and leaves plain pictures alone.
  return float2(mag, saturate(mag * 12.0));
}

float4 main(VSOut i) : SV_Target {
  // Source coordinates, already the right way up: FetchRgbAt resolves a bottom
  // up layout, so everything downstream of this pass can forget about it.
  int2 p = int2(i.pos.xy);
  float3 rgb = FetchRgbAtL(p, gChromaLinear != 0);

  // The A/B divider, and the only place it can sit. Everything that moves the
  // picture around -- cropping, rotation, scaling -- happens in the passes after
  // this one, so a divider measured in the window would slide off the picture
  // the moment any of them changed. Measured across the cropped picture in the
  // source's own grid it stays where it was put and turns with the picture.
  const bool across = gCompareAxis != 0;
  const float edge = across ? (float)gCropTop + gCompareSplit * (float)gOutHeight
                            : (float)gCropLeft + gCompareSplit * (float)gOutWidth;
  const float at = across ? (float)p.y : (float)p.x;
  const bool raw = gCompareSplit >= 0.0 && at < edge;

  // Everything that cleans the signal happens here, before any deinterlacer has
  // touched it. That is the entire reason this pass exists. A cleanup that runs
  // afterwards has to work out which source line the pixel in front of it came
  // from, and for a mode that interpolates there is no single answer -- so it
  // subtracts a pattern that pixel never carried, which is not a cleanup but a
  // new artefact. Doing it first removes the question.
  //
  // One branch takes out every composite artefact this program knows how to
  // take out, so skipping it shows the signal as the card handed it over. The
  // scale pass leaves out the rest on the same side -- sharpening, the picture
  // controls, the native grid, scanlines and the mask -- so the comparison is
  // the same one Shift+F12 makes, side by side. Deinterlacing runs on both,
  // because combing is not the signal but two fields shown at once.
  if (!raw) {
    // The two halves have to be told about each other. Both work out how much
    // pattern is present from the *captured* pixels, so where the four frame
    // average has already taken it away, the demodulator would go and subtract
    // it a second time -- putting an inverted copy back. That is why turning
    // both up made the picture worse than either one alone.
    //
    // So the temporal half reports how much of the job it did, and the
    // demodulator only handles the rest. Still picture: the average does it all,
    // at no cost in sharpness. Moving picture: the average steps back and the
    // demodulator takes over.
    float handled = 0.0;
    if (gTemporal > 0.0 && gHistCount >= gCrawlCycle - 1) {
      handled = TemporalGate(p.x, p.y) * gTemporal;
      rgb += TemporalDelta(p.x, p.y) * handled;
    }

    // Following the movement, over exactly the part of the picture the average
    // above has just let go of. The two are complements by construction: this
    // gets (1 - handled), which is what is moving, and it is band limited away
    // from the carrier, so the part it does take cannot disturb the crawl work
    // on either side of it. Where the picture stands still `handled` is one and
    // this does nothing, which is right -- there the free filter has won -- and
    // is then not worked out at all.
    if (gMotionComp != 0 && gHistCount >= 3 && handled < 1.0) {
      rgb += MotionCompDelta(p.x, p.y) * (1.0 - handled);
    }

    if (gDotNotch > 0.0) rgb += DotDemodDelta(p.x, p.y) * (1.0 - handled);

    // One reading of how much of this pixel sits at the carrier's own frequency,
    // for the two filters below that both want to know. Seventeen taps with a
    // sine and a cosine in each, so asking twice would be paying twice for the
    // same answer.
    float2 carrier = float2(0.0, 0.0);
    if (gBandwidth > 0.0 || (gChromaSoft > 0 && gAdaptChroma != 0)) {
      carrier = CarrierEnergy(p.x, p.y);
    }

    // After all three of those, and not before any of them: the band this lifts
    // runs up towards the carrier, and the pattern the filters above remove sits
    // in the top of it. Lifting a picture that still had the pattern in it would
    // hand them a harder job than they started with.
    if (gBandwidth > 0.0) rgb = BandwidthRestore(rgb, p.x, p.y, carrier.x);

    if (gChromaSoft > 0) {
      float3 soft = SoftenChroma(rgb, p.x, p.y);
      // Softening the colour only where the brightness carries enough at the
      // carrier to have invented some of it. Everywhere else the colour is as
      // real as composite ever gets it, and blurring that sideways is pure loss
      // -- which is what the slider did at every setting before this.
      rgb = gAdaptChroma != 0 ? lerp(rgb, soft, carrier.y) : soft;
    }
  }

  // Out of its curve and into linear light, once per pixel rather than once per
  // fetch -- which is why it sits here and not in FetchRgbIn, where the
  // deinterlacers would pay for it dozens of times over. Everything above this
  // line is analogue repair and belongs in the encoded domain anyway.
  if (gTransfer == 1) {
    // PQ says what it means in nits. Diffuse white is 203 of them, per BT.2408.
    rgb = float3(PqToNits(rgb.r), PqToNits(rgb.g), PqToNits(rgb.b)) / 203.0;
  } else if (gTransfer == 2) {
    // HLG is relative, and its system gamma depends on the display. 1.2 is the
    // reference value for a thousand nit screen, which is the common case.
    rgb = float3(HlgToScene(rgb.r), HlgToScene(rgb.g), HlgToScene(rgb.b));
    float luma = max(dot(rgb, float3(0.2627, 0.6780, 0.0593)), 1e-6);
    rgb *= pow(luma, 0.2);          // system gamma 1.2 applied to luminance
    rgb *= 1000.0 / 203.0;          // reference white of that display, in units of paper white
  }
  if (gGamut != 0) rgb = Bt2020ToBt709(rgb);

  // The divider drawn last, so it is a fixed value rather than a colour that
  // went through a transfer curve: diffuse white, which stays white on an HDR
  // screen too. A single grey line disappeared into grey material, so the white
  // core carries a darkened pixel on either side -- against a bright picture the
  // dark edges hold it, against a dark one the core does, and there is no
  // content it can match on both counts at once.
  //
  // Measured in source pixels, so it scales with the picture instead of getting
  // thinner as the window grows, and laid across the boundary rather than inside
  // the left half: the line marks where the two halves meet, so it should not
  // belong to one of them.
  //
  // Lying across, it is drawn twice as thick while a deinterlacer runs. Every
  // row of this pass belongs to one field, and bob shows one field at a time: a
  // line one row thick would be there in every other picture and flicker at
  // the field rate. Two rows give each field a line and a dark edge each side.
  if (gCompareSplit >= 0.0) {
    const float rows = (across && gDeinterlaceMode != 0) ? 2.0 : 1.0;
    const float d = at - edge;
    if (d >= -rows && d < 0.0) {
      rgb = float3(1.0, 1.0, 1.0);
    } else if (d >= -2.0 * rows && d < rows) {
      rgb *= 0.25;
    }
  }

  // Not clamped: the target is floating point and limited range material
  // legitimately reaches past both ends after expansion.
  return float4(rgb, 1.0);
}
)HLSL";

inline const char* kConvertPS =
R"HLSL(// Pass two: fields, cropping, line doubling and rotation. Reads the cleaned
// picture from pass one rather than the captured planes, so it needs to know
// nothing about pixel formats, frame history or where a pattern sat in a line --
// all of that is already dealt with.
Texture2D<float4> texClean : register(t0);
// The same, one frame earlier. Only YADIF looks at it.
Texture2D<float4> texCleanPrev : register(t1);

cbuffer ConvertCB : register(b0) {
  int   gFormatKind;      // see FormatKind in video_renderer.h
  int   gDeinterlaceMode; // 0 off, 1 bob, 2 bob linear, 3 motion adaptive,
                          // 4 edge directed, 5 yadif
  int   gFieldIndex;      // 0 = first field, 1 = second field
  int   gBottomUp;

  int   gCropLeft;
  int   gCropTop;
  int   gSrcWidth;
  int   gSrcHeight;

  int   gOutWidth;
  int   gOutHeight;
  int   gIsYuv;
  int   gHavePrev;        // previous frame's planes hold a usable picture

  float gYOffset;
  float gYScale;
  float gCScale;
  float gPad1;

  int   gCoSitedPhase;    // -1 normal interlace, else the row a field pair starts on
  int   gRotation;        // 0 none, 1 quarter turn right, 2 half, 3 quarter turn left
  int   gLineDouble;      // 1 = every source line fills two output lines
  int   gChromaSoft;      // horizontal chroma blur radius in source pixels, 0 off

  float gTemporal;        // 0..1 strength of the temporal average
  int   gHistCount;       // how many previous frames actually hold a picture
  float gDotNotch;        // 0..1: width of the demodulation window, 0 = off
  float gCarrierPeriod;   // samples per cycle of the colour subcarrier

  // Stops here on purpose, and anything added to ConvertCB from here down must
  // be added at the end of it. This pass and the one above share the buffer but
  // not the fields: everything past this point belongs to the clean pass, and
  // repeating it here would only mean two declarations to keep in step.
  //
  // There used to be a gCoef in this spot. It was never read, and it had not
  // matched the C++ layout since the transfer and motion fields went in above
  // it -- so anything that had read it would have got the motion gate's numbers
  // and called them colour coefficients.
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

float3 FetchRgbAt(int2 p) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  return texClean.Load(int3(p, 0)).rgb;
}

float3 FetchRgbPrev(int2 p) {
  p.x = clamp(p.x, 0, gSrcWidth - 1);
  p.y = clamp(p.y, 0, gSrcHeight - 1);
  return texCleanPrev.Load(int3(p, 0)).rgb;
}

float Luma(float3 c) {
  return dot(c, float3(0.299, 0.587, 0.114));
}

// Brightness with the fine pattern averaged out of it, for deciding *where* an
// edge runs. What is left of the composite shimmer repeats every three pixels
// across a line, and a direction search that scores raw brightness will happily
// lock onto that instead of onto the picture -- it finds a diagonal along which
// the dots line up, and reports it as an edge. The result is the grain that
// shows up on edge directed and, in the modes that use it, on YADIF.
//
// The value of a pixel still comes from the pixel. Only the decision about which
// direction to take it from is made on the smoothed version.
float LumaSmooth(int2 p) {
  return (Luma(FetchRgbAt(int2(p.x - 1, p.y))) + Luma(FetchRgbAt(p)) +
          Luma(FetchRgbAt(int2(p.x + 1, p.y)))) * 0.3333333;
}

// Interpolates a missing line by looking for the direction in which the line
// above and the line below agree, instead of straight down the column. On a
// diagonal edge the vertical average produces a staircase; following the edge
// does not.
float3 EdgeDirected(int x, int rowAbove, int rowBelow) {
  float3 a0 = FetchRgbAt(int2(x, rowAbove));
  float3 b0 = FetchRgbAt(int2(x, rowBelow));
  float3 best = (a0 + b0) * 0.5;

  // Scored over three pixels, not one. A single pair matches by coincidence all
  // over a flat or noisy picture -- dozens of directions come out near zero and
  // the winner is essentially arbitrary, which is exactly the speckle this mode
  // was producing. Three neighbouring pixels have to agree, and noise does not
  // manage that.
  float bestCost = abs(LumaSmooth(int2(x - 1, rowAbove)) - LumaSmooth(int2(x - 1, rowBelow))) +
                   abs(LumaSmooth(int2(x, rowAbove)) - LumaSmooth(int2(x, rowBelow))) +
                   abs(LumaSmooth(int2(x + 1, rowAbove)) - LumaSmooth(int2(x + 1, rowBelow)));

  [unroll]
  for (int d = 1; d <= 3; ++d) {
    // Slanting costs a little, so a genuinely vertical edge is not talked out of
    // being vertical by a marginally better diagonal match. Three terms now, so
    // the charge is three times what it was.
    float penalty = float(d) * 0.018;

    float costR = abs(LumaSmooth(int2(x - 1 + d, rowAbove)) -
                      LumaSmooth(int2(x - 1 - d, rowBelow))) +
                  abs(LumaSmooth(int2(x + d, rowAbove)) - LumaSmooth(int2(x - d, rowBelow))) +
                  abs(LumaSmooth(int2(x + 1 + d, rowAbove)) -
                      LumaSmooth(int2(x + 1 - d, rowBelow))) +
                  penalty;
    if (costR < bestCost) {
      bestCost = costR;
      best = (FetchRgbAt(int2(x + d, rowAbove)) + FetchRgbAt(int2(x - d, rowBelow))) * 0.5;
    }

    float costL = abs(LumaSmooth(int2(x - 1 - d, rowAbove)) -
                      LumaSmooth(int2(x - 1 + d, rowBelow))) +
                  abs(LumaSmooth(int2(x - d, rowAbove)) - LumaSmooth(int2(x + d, rowBelow))) +
                  abs(LumaSmooth(int2(x + 1 - d, rowAbove)) -
                      LumaSmooth(int2(x + 1 + d, rowBelow))) +
                  penalty;
    if (costL < bestCost) {
      bestCost = costL;
      best = (FetchRgbAt(int2(x - d, rowAbove)) + FetchRgbAt(int2(x + d, rowBelow))) * 0.5;
    }
  }

  // The answer still has to be plausible. A pixel interpolated from some
  // diagonal that lies outside the range its own two neighbours span did not
  // come from this edge -- it came from wherever the search wandered off to.
  // Pulling it back inside is the standard safeguard, and it is what removes the
  // gross misses rather than merely making them rarer.
  return clamp(best, min(a0, b0), max(a0, b0));
}

// YADIF's own directional predictor. Where EdgeDirected above compares single
// pixels, this scores a three-pixel window, which is what stops it from latching
// onto a coincidental match in noise -- and it only widens the search to two
// pixels of slant if one pixel already looked better than straight down.
float3 YadifSpatial(int x, int rowAbove, int rowBelow) {
  // Same three pixel window as before, but scored on the smoothed brightness for
  // the same reason as EdgeDirected: an unsmoothed score follows the composite
  // shimmer's own diagonal instead of the picture's.
  float3 pred = (FetchRgbAt(int2(x, rowAbove)) + FetchRgbAt(int2(x, rowBelow))) * 0.5;
  float best = abs(LumaSmooth(int2(x - 1, rowAbove)) - LumaSmooth(int2(x - 1, rowBelow))) +
               abs(LumaSmooth(int2(x, rowAbove)) - LumaSmooth(int2(x, rowBelow))) +
               abs(LumaSmooth(int2(x + 1, rowAbove)) - LumaSmooth(int2(x + 1, rowBelow))) -
               0.004;

  // Slant by j, and only widen to 2j when j already looked better than straight
  // down. The score compares the window on one line against the window on the
  // other, shifted by the slant being tried.
  [unroll]
  for (int side = 0; side < 2; ++side) {
    int j = side == 0 ? -1 : 1;
    float s1 = abs(LumaSmooth(int2(x - 1 + j, rowAbove)) - LumaSmooth(int2(x - 1 - j, rowBelow))) +
               abs(LumaSmooth(int2(x + j, rowAbove)) - LumaSmooth(int2(x - j, rowBelow))) +
               abs(LumaSmooth(int2(x + 1 + j, rowAbove)) - LumaSmooth(int2(x + 1 - j, rowBelow)));
    if (s1 < best) {
      best = s1;
      pred = (FetchRgbAt(int2(x + j, rowAbove)) + FetchRgbAt(int2(x - j, rowBelow))) * 0.5;

      int k = j * 2;
      float s2 = abs(LumaSmooth(int2(x - 1 + k, rowAbove)) -
                     LumaSmooth(int2(x - 1 - k, rowBelow))) +
                 abs(LumaSmooth(int2(x + k, rowAbove)) - LumaSmooth(int2(x - k, rowBelow))) +
                 abs(LumaSmooth(int2(x + 1 + k, rowAbove)) -
                     LumaSmooth(int2(x + 1 - k, rowBelow)));
      if (s2 < best) {
        best = s2;
        pred = (FetchRgbAt(int2(x + k, rowAbove)) + FetchRgbAt(int2(x - k, rowBelow))) * 0.5;
      }
    }
  }
  return pred;
}

// One missing line, reconstructed the way YADIF does it: predict it spatially,
// then refuse to let that prediction stray further from the temporal evidence
// than the surrounding lines say it plausibly could.
//
// The temporal pair is the same line in this frame and in the previous one --
// both belong to the field that is *not* being shown, half a field apart on
// either side of it. Textbook YADIF also looks at the frame after this one,
// which would mean holding every frame back until its successor arrives. In a
// viewer whose entire point is latency that trade is not worth making, so the
// filter runs one-sided; where that costs it, it falls back towards the spatial
// prediction, which is the safe direction to be wrong in.
float3 Yadif(int x, int row, int rowTop, int rowBottom) {
  int rowAbove = clamp(row - 1, rowTop, rowBottom);
  int rowBelow = clamp(row + 1, rowTop, rowBottom);
  float3 spatial = YadifSpatial(x, rowAbove, rowBelow);
  if (gHavePrev == 0) return spatial;

  float3 c = FetchRgbAt(int2(x, rowAbove));
  float3 e = FetchRgbAt(int2(x, rowBelow));

  float3 tPrev = FetchRgbPrev(int2(x, row));   // missing line, one frame ago
  float3 tCur  = FetchRgbAt(int2(x, row));     // missing line, woven into this frame
  float3 d = (tPrev + tCur) * 0.5;

  // How much this part of the picture is moving, measured on the lines we do
  // have as well as on the line we are guessing.
  float3 diff = max(abs(tPrev - tCur) * 0.5,
                    (abs(FetchRgbPrev(int2(x, rowAbove)) - c) +
                     abs(FetchRgbPrev(int2(x, rowBelow)) - e)) * 0.5);

  // Two lines out, same parity as the missing one: the local gradient the
  // reconstruction has to stay inside.
  int rowUp2 = clamp(row - 2, rowTop, rowBottom);
  int rowDn2 = clamp(row + 2, rowTop, rowBottom);
  float3 b = (FetchRgbPrev(int2(x, rowUp2)) + FetchRgbAt(int2(x, rowUp2))) * 0.5;
  float3 f = (FetchRgbPrev(int2(x, rowDn2)) + FetchRgbAt(int2(x, rowDn2))) * 0.5;

  float3 hi = max(max(d - e, d - c), min(b - c, f - e));
  float3 lo = min(min(d - e, d - c), max(b - c, f - e));
  diff = max(max(diff, lo), -hi);

  return clamp(spatial, d - diff, d + diff);
}


float4 main(VSOut i) : SV_Target {
  int2 op = int2(i.pos.xy);

  // Undo the rotation first: everything below works in the picture's own
  // orientation, which is the only one in which "the line above" means anything.
  // The logical picture is the cropped area, twice as tall if lines are doubled;
  // a quarter turn swaps the two axes of the render target but not of this.
  int logicalW = gOutWidth;
  int logicalH = gOutHeight * (gLineDouble != 0 ? 2 : 1);
  int lx, ly;
  if (gRotation == 1) {
    lx = op.y;
    ly = logicalH - 1 - op.x;
  } else if (gRotation == 2) {
    lx = logicalW - 1 - op.x;
    ly = logicalH - 1 - op.y;
  } else if (gRotation == 3) {
    lx = logicalW - 1 - op.y;
    ly = op.x;
  } else {
    lx = op.x;
    ly = op.y;
  }

  int srcX = lx + gCropLeft;

  // Rows of the cropped area, in full-frame coordinates.
  int rowTop = gCropTop;
  int rowBottom = gCropTop + gOutHeight - 1;
  int row = gCropTop + (gLineDouble != 0 ? (ly >> 1) : ly);

  float3 rgb;
  if (gDeinterlaceMode == 0) {
    rgb = FetchRgbAt(int2(srcX, row));
  } else if (gCoSitedPhase >= 0) {
    // Both fields hold the same picture lines, half a field apart in time: a
    // 240p or 288p console the card packed into an interlaced frame. Nothing is
    // spatially missing here, so every mode collapses to the same and only
    // correct answer -- take the line belonging to the field being shown and use
    // it for both rows of its pair. No interpolation, no ghosting, and no
    // vertical step, because the two fields are not offset from each other.
    int base = ((row - gCoSitedPhase) & ~1) + gCoSitedPhase;
    rgb = FetchRgbAt(int2(srcX, clamp(base + gFieldIndex, rowTop, rowBottom)));
  } else if (gDeinterlaceMode <= 2) {
    // Reconstruct a full frame out of the selected field. `t` is the
    // continuous index of that field's own rows.
    float t = (float(row) - float(gFieldIndex)) * 0.5;
    if (gDeinterlaceMode == 1) {
      // floor, not round. Rounding lands on .5 for every second output row, and
      // which way it goes depends on the field -- so the whole picture shifts by
      // a line each time the field changes, sixty times a second. Flooring is
      // half a line low for both fields equally, which is invisible; a line of
      // difference between fields is not.
      int r = int(floor(t)) * 2 + gFieldIndex;
      rgb = FetchRgbAt(int2(srcX, clamp(r, rowTop, rowBottom)));
    } else {
      float fl = floor(t);
      float frac = t - fl;
      int row0 = int(fl) * 2 + gFieldIndex;
      int row1 = row0 + 2;
      float3 a = FetchRgbAt(int2(srcX, clamp(row0, rowTop, rowBottom)));
      float3 b = FetchRgbAt(int2(srcX, clamp(row1, rowTop, rowBottom)));
      rgb = lerp(a, b, frac);
    }
  } else {
    // Modes 3 and 4 keep the frame's own geometry rather than stretching one
    // field over it. A row belonging to the field being shown is used exactly as
    // captured; the rows between them come from the other field, half a field
    // time away, and are only trustworthy where nothing moved.
    if ((row & 1) == gFieldIndex) {
      rgb = FetchRgbAt(int2(srcX, row));
    } else {
      int rowAbove = clamp(row - 1, rowTop, rowBottom);
      int rowBelow = clamp(row + 1, rowTop, rowBottom);
      float3 above = FetchRgbAt(int2(srcX, rowAbove));
      float3 below = FetchRgbAt(int2(srcX, rowBelow));

      if (gDeinterlaceMode == 5) {
        rgb = Yadif(srcX, row, rowTop, rowBottom);
      } else if (gDeinterlaceMode == 4) {
        rgb = EdgeDirected(srcX, rowAbove, rowBelow);
      } else {
        float3 woven = FetchRgbAt(int2(srcX, row));

        // Combing is the woven line sitting outside the range its two
        // neighbours span: bright, dark, bright. The product of the two
        // differences is positive exactly then, and near zero on a smooth
)HLSL"
R"HLSL(        // gradient. Vertical detail that is genuinely in the picture raises the
        // bar, so a static but finely striped image is not read as motion.
        //
        // Measured across three columns rather than one. A composite signal
        // carries enough noise that a single column flickers between weave and
        // interpolate from frame to frame, and that flicker is more visible than
        // the combing it was trying to remove.
        // Measured so that it scales with contrast rather than with its
        // square. The obvious form multiplies the two deviations together, and
        // on soft, anti-aliased material that collapses: against this card the
        // typical comb value came out at 0.00008 with the threshold sitting at
        // 0.00422 -- fifty times too high, so the mode wove essentially
        // everywhere and left the picture interlaced wherever anything moved.
        // Hard pixel edges cleared it and 3D rendering never did.
        //
        // Taking the smaller of the two deviations is linear in contrast and
        // works on both. It also needs no allowance for genuine vertical
        // detail: on a real edge the middle line lies *between* its
        // neighbours, so the deviations have opposite signs and this reads zero
        // by construction. Sweeping the remaining two numbers against frames
        // from the card, with movement established by comparing the same field
        // across frames, 0.003 and 60 reach 47 % of what genuinely moves while
        // touching 4.7 % of what does not.
        float motion = 0.0;
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
          float lw = Luma(FetchRgbAt(int2(srcX + dx, row)));
          float la = Luma(FetchRgbAt(int2(srcX + dx, rowAbove)));
          float lb = Luma(FetchRgbAt(int2(srcX + dx, rowBelow)));
          float da = lw - la;
          float db = lw - lb;
          float comb = (da * db > 0.0) ? min(abs(da), abs(db)) : 0.0;
          motion += saturate((comb - 0.003) * 60.0);
        }
        motion = saturate(motion * 0.3333);

        // Where it does interpolate, follow the edge rather than averaging
        // straight down. That is what was leaving jagged steps on things moving
        // quickly across the picture.
        rgb = lerp(woven, EdgeDirected(srcX, rowAbove, rowBelow), motion);
      }
    }
  }
  return float4(saturate(rgb), 1.0);
}
)HLSL";

// Pass four, and only when the swapchain is scRGB: the interface, brought into
// linear light.
//
// ImGui writes ordinary sRGB bytes. Handing those to a linear target shows them
// far too bright -- sRGB 0.5 is linear 0.21, and every grey in the interface
// would be wrong by that much. So it draws into a buffer of its own and lands
// here, where it is converted once and scaled to whatever diffuse white is.
//
// What arrives is premultiplied: ImGui blends over a transparent buffer, which
// leaves colour already multiplied by coverage. It has to be undone before the
// curve and redone after, because a curve applied to a premultiplied colour is
// not the same thing at all -- half covered black text would come out grey.
// The picture as a file should keep it: PQ encoded, BT.2020, ten bits.
//
// Everything else that leaves this program is eight bit and tone mapped, which
// is right for a webcam and for a screenshot somebody will paste somewhere. A
// recording is the one place where throwing the range away is a decision rather
// than a convenience, so this path does not: it puts the linear light back on
// the PQ curve and hands it over whole.
//
// BT.709 goes back to BT.2020 first. The pipeline works in BT.709 because that
// is what a screen wants; a PQ file is expected to be BT.2020 and a player will
// assume so whatever the file says.
inline const char* kHdrRecordPS = R"HLSL(
Texture2D<float4> texSrc : register(t0);
SamplerState sampPoint : register(s0);

cbuffer RecordCB : register(b0) {
  float gPaperWhite;
  float3 gRecordPad;
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

static const float kPqM1 = 0.1593017578125;
static const float kPqM2 = 78.84375;
static const float kPqC1 = 0.8359375;
static const float kPqC2 = 18.8515625;
static const float kPqC3 = 18.6875;

float NitsToPq(float nits) {
  float y = saturate(nits / 10000.0);
  float p = pow(y, kPqM1);
  return pow((kPqC1 + kPqC2 * p) / (1.0 + kPqC3 * p), kPqM2);
}

// The inverse of the matrix the clean pass applies, so a picture that never
// left BT.2020 comes back to exactly where it started.
float3 Bt709ToBt2020(float3 c) {
  return float3(
      dot(c, float3(0.627404, 0.329283, 0.043313)),
      dot(c, float3(0.069097, 0.919540, 0.011362)),
      dot(c, float3(0.016391, 0.088013, 0.895595)));
}

float4 main(VSOut i) : SV_Target {
  float3 lin = texSrc.SampleLevel(sampPoint, i.uv, 0).rgb;
  lin = Bt709ToBt2020(max(lin, 0.0)) * gPaperWhite;   // now in nits
  return float4(NitsToPq(lin.r), NitsToPq(lin.g), NitsToPq(lin.b), 1.0);
}
)HLSL";

inline const char* kUiCompositePS = R"HLSL(
Texture2D<float4> texUi : register(t0);
SamplerState sampPoint : register(s0);

cbuffer UiCB : register(b0) {
  float gPaperWhite;   // nits the interface's white should come out at
  float3 gUiPad;
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

float3 SrgbToLinear(float3 c) {
  c = saturate(c);
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 main(VSOut i) : SV_Target {
  float4 c = texUi.SampleLevel(sampPoint, i.uv, 0);
  if (c.a <= 0.0001) return float4(0.0, 0.0, 0.0, 0.0);
  float3 straight = c.rgb / c.a;
  float3 lin = SrgbToLinear(straight) * (gPaperWhite / 80.0);
  return float4(lin * c.a, c.a);
}
)HLSL";

inline const char* kScalePS = R"HLSL(
Texture2D<float4> texSrc : register(t0);
SamplerState sampPoint  : register(s0);
SamplerState sampLinear : register(s1);

cbuffer ScaleCB : register(b0) {
  float2 gSrcSize;
  float2 gDstSize;

  int    gFilter;   // 0 nearest, 1 bilinear, 2 bicubic, 3 lanczos3, 4 sharp bilinear
  float  gSharpen;  // 0..1
  int    gTransfer;    // 0 the picture is display referred, otherwise it is linear light
  int    gOutputHdr;   // the swapchain is scRGB and wants linear light

  float  gPaperWhite;  // nits diffuse white should come out at
  float  gSourcePeak;  // nits the source is assumed to reach at its brightest
  float  gDisplayPeak; // nits this screen can manage
  float  gScanlines;   // 0..1, how dark the gaps between source lines go

  int    gMask;         // 0 off, 1 aperture grille, 2 shadow mask
  float  gMaskStrength; // 0..1
  int    gNativeWidth;  // pixels the source really has across, 0 = leave alone
  float  gLinePitch;    // output rows per real picture line; 0 disables scanlines

  int    gPassthrough;  // resample only: no display effects, no transfer, no clamp
  float  gBrightness;   // -1..1 added, or stops of exposure in linear light
  float  gContrast;     // 0..2 around a pivot; 1 neutral
  float  gSaturation;   // 0..2; 1 neutral

  float  gHue;          // radians, converted on the way in
  int    gProcAmp;      // apply the four above in this pass
  float  gCompareSplit; // as in the clean pass: negative = no comparison
  int    gCompareAxis;  // 0 upright, 1 across

  int    gRotation;     // the quarter turns the convert pass applied
  int3   gPad;
};

struct VSOut {
  float4 pos : SV_Position;
  float2 uv  : TEXCOORD0;
};

float3 Tap(float2 texel) {
  float2 uv = (texel + 0.5) / gSrcSize;
  return texSrc.SampleLevel(sampPoint, uv, 0).rgb;
}

float CatmullRom(float x) {
  x = abs(x);
  if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
  if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
  return 0.0;
}

float Sinc(float x) {
  if (abs(x) < 1e-5) return 1.0;
  float px = 3.14159265358979 * x;
  return sin(px) / px;
}

float Lanczos3(float x) {
  x = abs(x);
  if (x >= 3.0) return 0.0;
  return Sinc(x) * Sinc(x / 3.0);
}

float3 SampleBicubic(float2 pos) {
  float2 base = floor(pos - 0.5);
  float2 f = pos - 0.5 - base;
  float3 sum = 0.0;
  float wsum = 0.0;
  [unroll] for (int dy = -1; dy <= 2; ++dy) {
    float wy = CatmullRom(float(dy) - f.y);
    [unroll] for (int dx = -1; dx <= 2; ++dx) {
      float w = CatmullRom(float(dx) - f.x) * wy;
      sum += Tap(base + float2(dx, dy)) * w;
      wsum += w;
    }
  }
  return sum / max(wsum, 1e-5);
}

float3 SampleLanczos(float2 pos) {
  float2 base = floor(pos - 0.5);
  float2 f = pos - 0.5 - base;
  float3 sum = 0.0;
  float wsum = 0.0;
  [unroll] for (int dy = -2; dy <= 3; ++dy) {
    float wy = Lanczos3(float(dy) - f.y);
    [unroll] for (int dx = -2; dx <= 3; ++dx) {
      float w = Lanczos3(float(dx) - f.x) * wy;
      sum += Tap(base + float2(dx, dy)) * w;
      wsum += w;
    }
  }
  return sum / max(wsum, 1e-5);
}

// Nearest neighbour, but the transition between two source pixels is squeezed
// into the one output pixel that straddles the boundary. Crisp like nearest,
// without the unevenly wide pixels you get at non-integer scale factors.
float3 SampleSharpBilinear(float2 pos) {
  float2 scale = max(gDstSize / gSrcSize, 1.0);
  float2 texelFloor = floor(pos);
  float2 s = pos - texelFloor;
  float2 regionRange = 0.5 - 0.5 / scale;
  float2 centerDist = s - 0.5;
  float2 f = (centerDist - clamp(centerDist, -regionRange, regionRange)) * scale + 0.5;
  float2 uv = (texelFloor + f) / gSrcSize;
  return texSrc.SampleLevel(sampLinear, uv, 0).rgb;
}

// Light contrast adaptive sharpening: boost the centre against its cross
// neighbourhood one source texel away, clamped to the local min/max so flat
// areas stay clean and edges do not ring.
//
// One *source* texel, and that is the whole of the fix this line once needed.
// `uv` runs over the source texture, so an offset of 1/gDstSize -- which is what
// stood here -- steps srcSize/dstSize source texels, less than one whenever the
// picture is enlarged. Both terms then collapse together: the neighbours are
// sampled between texels so the unsharp sum shrinks, and the clamp is built from
// those same samples so its headroom shrinks with it.
//
// Measured against a bandwidth limited edge on a 720x576 source, mean change per
// pixel across the edge at full strength: 0.71 levels at 1:1, then 0.38 at
// double size, 0.12 at quadruple, 0.06 at 4K -- the filter faded out exactly
// where a 240p or 576i picture is normally watched. Counted in source texels it
// holds 0.7 to 1.9 levels across that whole range, and stops over-sharpening
// when the picture is shrunk instead.
//
// A perfect step edge is deliberately left alone at any setting: the centre is
// already the local extreme, and the clamp will not push it past its own
// neighbours. There is nothing to sharpen about an edge that is already hard.
float3 Sharpen(float3 c, float2 uv, float amount) {
  float2 d = 1.0 / gSrcSize;
  float3 n = texSrc.SampleLevel(sampLinear, uv + float2(0.0, -d.y), 0).rgb;
  float3 s = texSrc.SampleLevel(sampLinear, uv + float2(0.0, d.y), 0).rgb;
  float3 w = texSrc.SampleLevel(sampLinear, uv + float2(-d.x, 0.0), 0).rgb;
  float3 e = texSrc.SampleLevel(sampLinear, uv + float2(d.x, 0.0), 0).rgb;
  float3 lo = min(min(min(n, s), min(w, e)), c);
  float3 hi = max(max(max(n, s), max(w, e)), c);
  float3 sharp = c + (c * 4.0 - n - s - w - e) * (amount * 0.25);
  return clamp(sharp, lo, hi);
}

// ---------------------------------------------------------------------------
// Back onto the pixel grid the console actually drew.
//
// A capture card samples the active line at a fixed rate -- 720 samples for
// BT.601, whatever the source does. A SNES puts 256 pixels across that same
// line, so each of its pixels lands on about 2.8 samples: not a whole number,
// and already softened by the card's own filter. Scale that straight to a
// window and the pixel boundaries fall wherever the arithmetic puts them, which
// is why upscaled pixel art usually looks slightly wrong even when it is sharp.
//
// Told what the real horizontal count is, every output pixel can be resolved to
// the *console's* pixel instead: work out which one it belongs to, and average
// exactly the samples that pixel covers. The grid comes back, and with integer
// scaling every block is the same width again.
//
// This is not what an OSSC does and cannot be. An OSSC samples the analogue
// waveform at the console's own dot clock, so it recovers the pixels before
// they are ever mixed together. Here they have already been resampled once and
// low-pass filtered by the card; what this recovers is the grid, not the detail
// that grid used to carry. Worth having, worth not overselling.
//
// Horizontal only, deliberately: vertically the card already gives one sample
// per real line -- 240 or 288 of them -- so there is nothing to undo.
//
// Bounded loop, because HLSL wants one and because the ratio is small: 720 over
// 256 is under three samples, and the clamp on the setting keeps it under
// sixteen even for an absurdly low count.
)HLSL"
R"HLSL(float3 SampleNativeGrid(float2 uv) {
  float n = floor(uv.x * (float)gNativeWidth);
  float span = gSrcSize.x / (float)gNativeWidth;
  float x0 = n * span;
  float x1 = x0 + span;

  int row = clamp((int)(uv.y * gSrcSize.y), 0, (int)gSrcSize.y - 1);
  int first = (int)floor(x0);
  int maxX = (int)gSrcSize.x - 1;

  float3 sum = 0.0;
  float weight = 0.0;
  [loop]
  for (int k = 0; k < 16; ++k) {
    int x = first + k;
    if ((float)x >= x1) break;
    // How much of this source sample the console pixel actually covers. The
    // ends are partial, which is the whole reason for weighting rather than
    // simply averaging whole samples: at 2.8 samples per pixel the boundaries
    // land inside a sample two times out of three.
    float cover = min((float)x + 1.0, x1) - max((float)x, x0);
    if (cover <= 0.0) continue;
    sum += texSrc.Load(int3(clamp(x, 0, maxX), row, 0)).rgb * cover;
    weight += cover;
  }
  return sum / max(weight, 1e-4);
}

// ---------------------------------------------------------------------------
// Optional: what a cathode ray tube did to the picture.
//
// Off by default and meant to stay that way for anyone who wants the signal
// as clean as it arrived. It is here because 240p artwork was drawn for a
// display that had gaps between its lines and a coloured mask over its
// phosphors, and on a modern panel the absence of both is itself a distortion.
//
// Both effects darken, so both are compensated afterwards: without that,
// turning them on is mostly a brightness control.
// ---------------------------------------------------------------------------

// Scanlines follow the *source* line grid, not the output pixel grid -- the
// gaps belong to the signal, not to the monitor showing it.
//
// Below roughly twice the source height there is nowhere to put them: the gap
// and the line land inside the same output pixel and the result is a moire
// pattern rather than a scanline. So the effect fades in across 2x-3x and is
// simply absent below it, which is safer than letting somebody turn on
// something that can only alias.
float Scanline(float2 uv) {
  if (gLinePitch <= 0.0) return 1.0;

  // The *real* line count, which is not always the height of the picture.
  //
  // A 240p console packed into a 576i frame arrives with its fields co-sited:
  // two rows of the frame carry the same picture line. Line doubling does the
  // same thing on purpose. In both cases the frame is twice as tall as the
  // signal, and drawing a gap between two rows that hold one line would put
  // scanlines at double the density the console ever had.
  float lines = gSrcSize.y / gLinePitch;
  float scale = gDstSize.y / max(lines, 1.0);
  float room = saturate((scale - 2.0) * 1.0);
  if (room <= 0.0) return 1.0;

  // Distance from the centre of the picture line this pixel falls in, 0..1.
  float phase = frac(uv.y * lines);
  float d = abs(phase - 0.5) * 2.0;

  // A raised cosine rather than a hard bar: a real beam has a profile, and a
  // hard edge would alias again at every non-integer scale.
  float beam = 0.5 + 0.5 * cos(3.14159265 * d);
  float dark = 1.0 - gScanlines * room * (1.0 - beam);

  // Put back what the gaps took, so the control changes structure and not
  // brightness. The mean of the raised cosine over a line is 0.5.
  return dark / (1.0 - gScanlines * room * 0.5);
}

// A coloured mask, the way a grille or a shadow mask splits white into triads.
// Needs real output resolution to read as anything but a tint -- at 1080p over
// a 240p source each source line is four output pixels tall and a triad is
// three across, which is about the floor.
float3 Mask(float2 pos) {
  if (gMask == 0) return 1.0;

  int col = ((int)pos.x) % 3;
  float3 tint = col == 0 ? float3(1.0, 0.35, 0.35)
              : col == 1 ? float3(0.35, 1.0, 0.35)
                         : float3(0.35, 0.35, 1.0);

  if (gMask == 2) {
    // Shadow mask: the triads step sideways every other line, which is what
    // stops a grille's vertical stripes from being so obvious.
    int row = ((int)pos.y / 2) % 2;
    if (row == 1) tint = tint.zxy;
  }

  float3 m = lerp(float3(1.0, 1.0, 1.0), tint, gMaskStrength);
  // Same compensation as the scanlines: the mask's average is below one.
  float mean = (m.r + m.g + m.b) / 3.0;
  return m / max(mean, 1e-3);
}

// ---------------------------------------------------------------------------
// Getting linear light onto a screen.
//
// Two jobs, and which one runs depends on the screen rather than the source.
// An HDR screen is handed scRGB: still linear, with 1.0 meaning eighty nits by
// definition, so the only work is a scale. An ordinary screen cannot show a
// thousand nit highlight and has to be told a smaller story about it -- that is
// the tone mapping, and it is done in the PQ domain because PQ is roughly
// perceptually even, which is where a knee belongs.
//
// The curve is the one from BT.2390: linear below the knee so ordinary picture
// content passes through untouched, a Hermite spline above it so highlights
// compress smoothly instead of clipping to a flat white.
// ---------------------------------------------------------------------------
static const float kPqM1 = 0.1593017578125;
static const float kPqM2 = 78.84375;
static const float kPqC1 = 0.8359375;
static const float kPqC2 = 18.8515625;
static const float kPqC3 = 18.6875;

float NitsToPq(float nits) {
  float y = saturate(nits / 10000.0);
  float p = pow(y, kPqM1);
  return pow((kPqC1 + kPqC2 * p) / (1.0 + kPqC3 * p), kPqM2);
}

float PqToNits(float e) {
  float p = pow(saturate(e), 1.0 / kPqM2);
  float num = max(p - kPqC1, 0.0);
  float den = kPqC2 - kPqC3 * p;
  return 10000.0 * pow(num / max(den, 1e-6), 1.0 / kPqM1);
}

float Bt2390Knee(float e, float maxLum) {
  float ks = 1.5 * maxLum - 0.5;   // where the straight part gives way
  if (e < ks) return e;
  float t = (e - ks) / max(1.0 - ks, 1e-6);
  float t2 = t * t;
  float t3 = t2 * t;
  return (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +
         (-2.0 * t3 + 3.0 * t2) * maxLum;
}

float3 LinearToSrgb(float3 c) {
  c = saturate(c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float3 SrgbToLinear(float3 c) {
  c = saturate(c);
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 ToneMapToSdr(float3 rgb) {
  // Tone map the brightness and carry the colour along, rather than running the
  // curve on each channel: doing it per channel pulls saturated highlights
  // towards white, which is how a sunset turns into a smear.
  float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
  if (luma <= 1e-6) return rgb;

  float scale = NitsToPq(gSourcePeak);
  float e = NitsToPq(luma * gPaperWhite) / max(scale, 1e-6);
  float maxLum = NitsToPq(gDisplayPeak) / max(scale, 1e-6);
  float mapped = PqToNits(saturate(Bt2390Knee(saturate(e), saturate(maxLum)) * scale));

  return rgb * (mapped / gDisplayPeak) / luma;
}

// ---------------------------------------------------------------------------
// Brightness, contrast, saturation, hue.
//
// In the shader rather than on the card on purpose. The card's own set is put
// back to neutral when the graph is built, so the picture arriving here is the
// one the console sent; a decoder quietly lifting the black level would mean
// there was no clean version anywhere to go back to.
//
// Order is contrast, brightness, then colour, which is the order every other
// program does them in -- a number copied from one of them lands where it is
// expected to.
// ---------------------------------------------------------------------------
float3 ApplyProcAmp(float3 rgb) {
  const bool lin = gTransfer != 0;  // the picture is linear light, not display referred
  const float3 kLuma = float3(0.2126, 0.7152, 0.0722);  // BT.709; the convert pass got us here

  // Contrast pivots around mid grey, and which value that is depends on what is
  // being carried. Display referred, half scale looks like the middle of the
  // range. In linear light the same grey card sits near 0.18 -- undoing that is
  // most of what a gamma curve was for.
  const float pivot = lin ? 0.18 : 0.5;
  rgb = (rgb - pivot) * gContrast + pivot;

  // Brightness adds where the picture is display referred and multiplies where
  // it is linear. Adding a constant to linear light raises the darkest parts of
  // the picture out of all proportion to the rest and turns black into grey
  // fog; multiplying is exposure, which is what the knob is expected to feel
  // like.
  if (lin) rgb *= exp2(gBrightness * 2.0);
  else     rgb += gBrightness;

  // Saturation and hue are one operation on the colour difference from luma:
  // hue turns that vector, saturation changes its length. Doing both as a
  // single rotate-and-scale keeps luma exactly where it was, which is the whole
  // reason for going through the colour difference rather than touching RGB.
  float y = dot(rgb, kLuma);
  float cb = (rgb.b - y) / 1.8556;  // 2 * (1 - Kb)
  float cr = (rgb.r - y) / 1.5748;  // 2 * (1 - Kr)

  const float c = cos(gHue) * gSaturation;
  const float s = sin(gHue) * gSaturation;
  const float cb2 = cb * c - cr * s;
  const float cr2 = cb * s + cr * c;

  const float b = y + 1.8556 * cb2;
  const float r = y + 1.5748 * cr2;
  const float g = (y - kLuma.r * r - kLuma.b * b) / kLuma.g;
  rgb = float3(r, g, b);

  // Display referred, anything outside the range is a colour that does not
  // exist and clamping is the honest answer. In linear light the top end is the
  // highlight a recording still wants, so only the floor is held.
  return lin ? max(rgb, 0.0) : saturate(rgb);
}

// Which side of the A/B divider a pixel is on. The clean pass drew the divider
// and left the composite chain out on one side; everything this pass adds on
// top has to stay out there as well, or the unfiltered half would still be
// sharpened, graded and put behind a mask -- and a comparison that differs in
// five things answers none of them.
//
// The divider was measured across the cropped picture before any quarter turn,
// so the turn is undone here to find that same axis again. `uv` covers the
// intermediate exactly, which is the cropped picture turned.
bool CompareRaw(float2 uv) {
  if (gCompareSplit < 0.0) return false;
  float2 l;
  if (gRotation == 1)      l = float2(uv.y, 1.0 - uv.x);
  else if (gRotation == 2) l = float2(1.0 - uv.x, 1.0 - uv.y);
  else if (gRotation == 3) l = float2(1.0 - uv.y, uv.x);
  else                     l = uv;
  return (gCompareAxis != 0 ? l.y : l.x) < gCompareSplit;
}

float4 main(VSOut i) : SV_Target {
  float2 pos = i.uv * gSrcSize;  // position in source pixels
  const bool raw = CompareRaw(i.uv);

  float3 rgb;
  if (gNativeWidth > 0 && !raw) {
    // Deliberately ahead of the filter choice and instead of it. Resolving to
    // the console's own pixel *is* a nearest-neighbour decision -- that is the
    // point of it -- and running a smoothing filter afterwards would put back
    // exactly the boundary softness this just removed. The filter setting still
    // governs anything else on screen.
    rgb = SampleNativeGrid(i.uv);
  } else if (gFilter == 0) {
    rgb = texSrc.SampleLevel(sampPoint, i.uv, 0).rgb;
  } else if (gFilter == 1) {
    rgb = texSrc.SampleLevel(sampLinear, i.uv, 0).rgb;
  } else if (gFilter == 2) {
    rgb = SampleBicubic(pos);
  } else if (gFilter == 3) {
    rgb = SampleLanczos(pos);
  } else {
    rgb = SampleSharpBilinear(pos);
  }

  // Ahead of the line below, and that is the point of it. Everything past that
  // line is a display effect; these four are not, and whether they reach a file
  // is the user's decision rather than a property of the pass. gProcAmp is how
  // that decision arrives -- set for the window whenever a knob is off centre,
  // set on the way out only when the user asked for it.
  if (gProcAmp != 0 && !raw) rgb = ApplyProcAmp(rgb);

  // Resampling on the way out to something that is not a screen. Everything
  // below this line is either a display effect or a decision about which screen
  // the picture is headed for, and none of that belongs in a file. Not even the
  // clamp: in the half float case what is being carried is linear light, where
  // a value above one is the highlight the whole exercise is about.
  if (gPassthrough != 0) return float4(rgb, 1.0);

  if (gSharpen > 0.001 && !raw) {
    rgb = Sharpen(rgb, i.uv, gSharpen);
  }

  // After sharpening, before the transfer: these are display effects, so they
  // belong on the picture as it will be seen and not on the signal. Neither
  // reaches a recording, a screenshot or the virtual camera -- those take the
  // intermediate, which is upstream of this pass entirely.
  if (!raw && (gScanlines > 0.001 || (gMask != 0 && gMaskStrength > 0.001))) {
    float3 gain = 1.0;
    if (gScanlines > 0.001) gain *= Scanline(i.uv);
    if (gMask != 0 && gMaskStrength > 0.001) gain *= Mask(i.pos.xy);

    // Both effects darken and both put the brightness back, which means the
    // gain goes above one wherever a line or a phosphor sits. On an already
    // bright pixel that lands past full scale and clips -- and clipping happens
    // per channel, so a saturated yellow loses its red first and shifts hue.
    // That is what "the colour goes odd" was: not too much effect, but a boost
    // with nowhere left to go.
    //
    // So the boost is held to what the pixel can still take. Scaling all three
    // channels by one number keeps the hue exactly; only the amount of lift
    // changes, and it only changes where there was no room for it anyway.
    float peak = max(max(rgb.r, rgb.g), rgb.b);
    if (peak > 1e-4) {
      float headroom = 1.0 / peak;
      gain = min(gain, max(headroom, 1.0));
    }
    rgb *= gain;
  }

  if (gTransfer != 0) {
    // Linear light in, 1.0 being diffuse white.
    if (gOutputHdr != 0) {
      // scRGB: linear, BT.709 primaries, 1.0 is eighty nits by definition.
      return float4(rgb * (gPaperWhite / 80.0), 1.0);
    }
    return float4(saturate(LinearToSrgb(ToneMapToSdr(rgb))), 1.0);
  }
  if (gOutputHdr != 0) {
    // An ordinary picture on an HDR screen. It is encoded against sRGB and the
    // screen is being fed linear light, so leaving it alone would show it far
    // too bright -- this is the case that looks broken if it is forgotten.
    return float4(SrgbToLinear(rgb) * (gPaperWhite / 80.0), 1.0);
  }
  return float4(saturate(rgb), 1.0);
}
)HLSL";

}  // namespace cap
