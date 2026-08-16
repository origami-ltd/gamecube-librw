#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwgx.h"

#define PLUGIN_ID ID_DRIVER

#ifndef RW_GAMECUBE

namespace rw {
namespace gx {
void registerPlatformPlugins(void) { }
}
}

#else

#include <gccore.h>
#include <malloc.h>

// libfat is shared with the game's streaming worker and must be serialised;
// the lock lives in CdStream_gamecube.cpp. Declared rather than included
// because librw does not get to see the game's headers.
extern "C" {
void CdStreamFsLock(void);
void CdStreamFsUnlock(void);
}
namespace { struct DvdFsGuard {
	DvdFsGuard(void) { CdStreamFsLock(); }
	~DvdFsGuard(void) { CdStreamFsUnlock(); }
}; }
#define DVD_FS_CAT2(a, b) a##b
#define DVD_FS_CAT(a, b) DVD_FS_CAT2(a, b)
#define DVD_FS_GUARD DvdFsGuard DVD_FS_CAT(_dvdFsGuard_, __LINE__)


// Texture allocations the streamer had to fail. Global scope, next to the
// geometry counter it complements — rwGeoAllocFails alone reports "no OOM"
// while every character on screen is a black silhouette, because the failure
// is here and nothing was counting it.
unsigned rwTexAllocFails;

// Live bytes held by GX-tiled textures, tracked exactly.
//
// This is the hole in the streaming accounting. gResidentCost measures a heap
// delta across the load, but gxGetTexture allocates the tiled buffer at the
// FIRST DRAW — after that window has closed — so up to 512KB per texture is
// never charged to any stream entry. ms_memoryUsed is therefore systematically
// under the truth, the streamer believes it has room it does not have, keeps
// loading, and the texture allocation eventually fails. Silently, which is how
// it surfaces as black silhouettes rather than as an out-of-memory.
//
// Reported rather than folded into ms_memoryUsed for now: adding it changes
// what the streamer evicts, and that needs to be a measured change rather than
// another blind one.
unsigned gxTiledBytes;

extern unsigned gxTexBuilds; // global counter, defined in gx.cpp
extern unsigned gxTileUs;    // CPU time spent tiling textures this frame
#include <ogc/lwp_watchdog.h>

namespace rw {
namespace gx {

// Native raster extension: the GX-tiled copy of the linear staging pixels.
struct GxRaster
{
	void *tiled;
	GXTexObj obj;
	bool32 hasTex;
	bool32 dirty;
	// Set when rasterLock had to fabricate a staging buffer because the real
	// one was freed after tiling. Anything written into a fabricated buffer is
	// partial at best (a mip level laid into a level-0-sized allocation) and
	// zero at worst, so rebuilding the texture from it destroys a good one.
	bool32 fabricated;
	uint8 gxFmt; // GX_TF_CMPR or GX_TF_RGB5A3, chosen at first build
	uint32 tiledSize; // so the free path can subtract exactly what it added
};

int32 nativeRasterOffset;
bool32 gxTexCacheDirty;

// Per-geometry display lists: one recorded FIFO block per mesh. Replaying
// costs the GP a DMA and the CPU nothing, which is the whole point — the
// immediate path spends the frame pushing ~54k vertices through the
// write-gather pipe. A geometry plugin gives us both the storage and a
// destructor, so lists die with the geometry the streamer evicts.
int32 gxGeoOffset;
uint32 gxDlBytes;

static void*
createGeoExt(void *object, int32 offset, int32)
{
	memset(PLUGINOFFSET(GxGeoExt, object, offset), 0, sizeof(GxGeoExt));
	return object;
}

static void*
destroyGeoExt(void *object, int32 offset, int32)
{
	GxGeoExt *g = PLUGINOFFSET(GxGeoExt, object, offset);
	for(int32 i = 0; i < g->numLists; i++)
		if(g->lists[i]){
			gxDlBytes -= g->sizes[i];
			free(g->lists[i]);
		}
	free(g->lists);
	free(g->sizes);
	rwFree(g->packBase);
	memset(g, 0, sizeof(*g));
	return object;
}

uint32 gxPackSaved;
uint32 gxPackGeoms, gxPackRefusedPos, gxPackRefusedUV;

// Largest binary shift that keeps maxAbs inside int16, or -1 when even the
// coarsest useful quantum would not hold it. Refusing is always safe: the
// geometry stays on the float path and costs only the bytes we hoped to save.
static int
gxShiftFor(float maxAbs, int floorShift)
{
	if(maxAbs <= 0.0f)
		return 15;
	int s = 0;
	while(s < 15 && maxAbs*(float)(1 << (s+1)) <= 32767.0f)
		s++;
	return s < floorShift ? -1 : s;
}

static inline int16
gxQuant(float v, int shift)
{
	float q = v*(float)(1 << shift);
	// The extents drove the shift, so this only clamps against rounding at
	// the very edge — but an int16 overflow wraps to the opposite corner of
	// the model, which draws as a spike across the map rather than as noise.
	if(q > 32767.0f) return 32767;
	if(q < -32768.0f) return -32768;
	return (int16)(q < 0.0f ? q - 0.5f : q + 0.5f);
}

// Quantise positions and texcoords to int16 and release the float arrays they
// came from.
//
// GX_SetVtxAttrFmt takes GX_S16 with a per-attribute binary shift, so nothing
// is decoded at draw time: the value the GP reads IS the stored one, scaled by
// a power of two the hardware applies for free. This is a reduction, not a
// trade of memory against CPU — the vertex stream gets smaller too.
//
// Why it earns the surgery: after dropTrianglesAfterInstancing the RW arrays
// cost about 26 bytes a vertex, of which positions are 12 and texcoords 8.
// Replacing those 20 with 10 takes 38% off every loaded model, and
// gResidentCost measures real heap bytes, so the streaming budget sees the
// whole reduction and fits correspondingly more world. That is the safe half of
// the trade the handoff draws: real bytes reclaimed at an UNCHANGED reserve,
// not budget bought out of the headroom the exterior needs.
//
// The shift is chosen per geometry from its own extents, so precision follows
// the model instead of a global guess. A model reaching 128 units lands on
// shift 7 — 1/128 unit, the same quantum COMPRESSED_COL_VECTORS already uses
// for every collision vertex in the game — and each doubling of the extent
// costs exactly one bit of that. tools/gamecube/packtest.c pins the contract.
//
// Only called on streamed world geometry. Anything the game mutates after load
// keeps its float arrays: CWaterLevel rewrites the wavy geometry's vertices
// every frame, and skinned vertices are rebuilt every frame by definition.
void
gxPackGeometry(Geometry *geo)
{
	if(geo == nil || geo->numVertices <= 0 || (geo->flags & Geometry::NATIVE))
		return;
	GxGeoExt *g = PLUGINOFFSET(GxGeoExt, geo, gxGeoOffset);
	if(g->packed & GXPACK_TRIED)
		return;
	g->packed |= GXPACK_TRIED;
	if(geo->numMorphTargets != 1 || geo->morphTargets == nil)
		return;
	if(Skin::get(geo))
		return;

	MorphTarget *mt = &geo->morphTargets[0];
	V3d *verts = mt->vertices;
	if(verts == nil)
		return;
	int32 n = geo->numVertices;
	TexCoords *uv = geo->numTexCoordSets == 1 ? geo->texCoords[0] : nil;

	// Both blocks must have the exact layout Geometry::create built, or the
	// pointer arithmetic below is writing into someone else's data. Check the
	// structure rather than trusting it: morph vertices sit immediately after
	// the MorphTarget array, texcoords immediately after the colours (the
	// triangles that used to precede them are already gone).
	uint8 *mbase = (uint8*)geo->morphTargets;
	if((uint8*)verts != mbase + sizeof(MorphTarget))
		return;
	uint8 *abase = (uint8*)geo->attribBase;
	size_t colSz = (geo->flags & Geometry::PRELIT) ? (size_t)n*sizeof(RGBA) : 0;
	if(uv && (abase == nil || (uint8*)uv != abase + colSz))
		uv = nil;

	// Extents decide the quantum. Positions floor at 1/8 unit and texcoords at
	// 1/512 uv — half a texel on a 256-wide texture, which is where quantised
	// UVs start to show as texture swim on large tiled surfaces.
	float maxP = 0.0f, maxT = 0.0f;
	for(int32 i = 0; i < n; i++){
		float a = fabsf(verts[i].x), b = fabsf(verts[i].y), c = fabsf(verts[i].z);
		if(a > maxP) maxP = a;
		if(b > maxP) maxP = b;
		if(c > maxP) maxP = c;
	}
	if(uv)
		for(int32 i = 0; i < n; i++){
			float a = fabsf(uv[i].u), b = fabsf(uv[i].v);
			if(a > maxT) maxT = a;
			if(b > maxT) maxT = b;
		}
	int ps = gxShiftFor(maxP, 3);
	int ts = uv ? gxShiftFor(maxT, 9) : -1;
	if(ps < 0) gxPackRefusedPos++;
	if(uv && ts < 0) gxPackRefusedUV++;
	if(ps < 0 && ts < 0)
		return;

	size_t posSz = ps >= 0 ? (size_t)n*3*sizeof(int16) : 0;
	size_t uvSz  = ts >= 0 ? (size_t)n*2*sizeof(int16) : 0;
	uint8 *blk = (uint8*)rwMalloc(posSz + uvSz, MEMDUR_EVENT | ID_GEOMETRY);
	if(blk == nil)
		return;              // non-must: staying on the float path is correct
	g->packBase = blk;

	if(ps >= 0){
		int16 *p = (int16*)blk;
		g->pos = p;
		g->posShift = (uint8)ps;
		for(int32 i = 0; i < n; i++){
			*p++ = gxQuant(verts[i].x, ps);
			*p++ = gxQuant(verts[i].y, ps);
			*p++ = gxQuant(verts[i].z, ps);
		}
		g->packed |= GXPACK_POS;
	}
	if(ts >= 0){
		int16 *t = (int16*)(blk + posSz);
		g->uv = t;
		g->uvShift = (uint8)ts;
		for(int32 i = 0; i < n; i++){
			*t++ = gxQuant(uv[i].u, ts);
			*t++ = gxQuant(uv[i].v, ts);
		}
		g->packed |= GXPACK_UV;
	}

	// Release what we just replaced. Shrinking reallocs only: the block can
	// never end up larger than it started, so this eases fragmentation rather
	// than adding to it — the same reasoning dropTrianglesAfterInstancing uses.
	if(g->packed & GXPACK_POS){
		size_t keep = sizeof(MorphTarget);
		size_t vertSz = (size_t)n*sizeof(V3d);
		if(mt->normals){
			memmove(mbase + keep, mbase + keep + vertSz, vertSz);
			keep += vertSz;
		}
		gxPackSaved += (uint32)vertSz;
		uint8 *shrunk = (uint8*)rwRealloc(mbase, keep, MEMDUR_EVENT | ID_GEOMETRY);
		if(shrunk)
			mbase = shrunk;
		geo->morphTargets = (MorphTarget*)mbase;
		mt = &geo->morphTargets[0];
		mt->vertices = nil;
		mt->normals = mt->normals ? (V3d*)(mbase + sizeof(MorphTarget)) : nil;
	}
	if(g->packed & GXPACK_UV){
		gxPackSaved += (uint32)((size_t)n*sizeof(TexCoords));
		uint8 *shrunk = (uint8*)rwRealloc(abase, colSz ? colSz : 1,
		                                  MEMDUR_EVENT | ID_GEOMETRY);
		if(shrunk)
			abase = shrunk;
		geo->attribBase = abase;
		if(colSz)
			geo->colors = (RGBA*)abase;
		// Say it out loud rather than leaving a dangling array that still
		// looks valid to anything that reads it later.
		geo->texCoords[0] = nil;
	}
	gxPackGeoms++;
}

static void*
copyGeoExt(void *dst, void *, int32 offset, int32)
{
	memset(PLUGINOFFSET(GxGeoExt, dst, offset), 0, sizeof(GxGeoExt));
	return dst;
}
#define GETGXRASTEREXT(raster) PLUGINOFFSET(GxRaster, raster, nativeRasterOffset)

static void*
createNativeRaster(void *object, int32 offset, int32)
{
	GxRaster *ext = PLUGINOFFSET(GxRaster, object, offset);
	memset(ext, 0, sizeof(*ext));
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	GxRaster *ext = PLUGINOFFSET(GxRaster, object, offset);
	if(ext->tiled){
		::gxTiledBytes -= ext->tiledSize;
		ext->tiledSize = 0;
		free(ext->tiled);
		ext->tiled = nil;
	}
	ext->hasTex = 0;
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	GxRaster *ext = PLUGINOFFSET(GxRaster, dst, offset);
	memset(ext, 0, sizeof(*ext));
	return dst;
}

void
registerPlatformPlugins(void)
{
	gxGeoOffset = Geometry::registerPlugin(sizeof(GxGeoExt), ID_DRIVER,
	                                       createGeoExt, destroyGeoExt,
	                                       copyGeoExt);
	nativeRasterOffset = Raster::registerPlugin(sizeof(GxRaster),
	                                            ID_DRIVER,
	                                            createNativeRaster,
	                                            destroyNativeRaster,
	                                            copyNativeRaster);
}

// Convert the linear staging pixels to GX_TF_RGB5A3: 4x4 texel tiles of 32
// bytes, one big-endian uint16 per texel. 16-bit halves resident texture
// memory vs RGBA8 — the difference between fitting the arena and exit(1).
// point-sample one texel of the (possibly downscaled) GX copy
static inline void
sampleTexel(Raster *raster, int32 dx, int32 dy, int32 tw, int32 th,
	uint8 *pr, uint8 *pg, uint8 *pb, uint8 *pa)
{
	int32 w = raster->width, h = raster->height;
	uint8 r = 0, g = 0, b = 0, a = 0;
	int32 sx = dx * w / tw, sy = dy * h / th;
	if(dx < tw && dy < th && sx < w && sy < h){
		uint8 *p = raster->pixels + sy*raster->stride;
		int32 base = raster->format & 0xF00;
		// C888/C565/C555 carry no alpha channel — byte 3 (or the top bit)
		// is undefined padding. Sampling it anyway produced alpha 0, and
		// the alpha test then threw the texel away: that is why ped bodies
		// vanished while their C8888 hair and clothes drew fine.
		bool noAlpha = base == Raster::C888 || base == Raster::C565 ||
		    base == Raster::C555;
		if(raster->depth == 32){
			p += sx*4;
			r = p[0]; g = p[1]; b = p[2];
			a = noAlpha ? 255 : p[3];
		}else if(raster->depth == 8 || raster->depth == 4){
			// Palettised raster. Reading these as 16-bit produced garbage
			// texels with garbage alpha, and the alpha test then discarded
			// them — which is why ped skin vanished while their 32-bit
			// hair and clothing textures drew fine.
			uint8 *pal = raster->palette;
			uint32 i = raster->depth == 8 ? p[sx] :
			    (p[sx>>1] >> ((sx & 1) ? 4 : 0)) & 0xF;
			if(pal){
				pal += i*4;
				r = pal[0]; g = pal[1]; b = pal[2]; a = pal[3];
			}else{
				r = g = b = (uint8)i; a = 255;
			}
		}else{
			uint16 v = *(uint16*)(p + sx*2);
			if(base == Raster::C565){
				r = ((v>>11)&0x1F)<<3;
				g = ((v>>5)&0x3F)<<2;
				b = (v&0x1F)<<3;
				a = 255;
			}else{
				r = ((v>>10)&0x1F)<<3;
				g = ((v>>5)&0x1F)<<3;
				b = (v&0x1F)<<3;
				a = noAlpha ? 255 : ((v&0x8000) ? 255 : 0);
			}
		}
	}
	*pr = r; *pg = g; *pb = b; *pa = a;
}

static void
tileRGB5A3(uint8 *dst, Raster *raster, int32 tw, int32 th)
{
	for(int32 ty = 0; ty < th; ty += 4)
	for(int32 tx = 0; tx < tw; tx += 4){
		uint16 *out = (uint16*)dst;
		for(int32 y = 0; y < 4; y++)
		for(int32 x = 0; x < 4; x++){
			uint8 r, g, b, a;
			sampleTexel(raster, tx + x, ty + y, tw, th, &r, &g, &b, &a);
			// >=0xE0 rounds to full 3-bit alpha anyway; use opaque 5:5:5.
			if(a >= 0xE0)
				*out++ = 0x8000 | ((r>>3)<<10) | ((g>>3)<<5) | (b>>3);
			else
				*out++ = ((a>>5)<<12) | ((r>>4)<<8) | ((g>>4)<<4) | (b>>4);
		}
		dst += 32;
	}
}

// GX_TF_CMPR: DXT1-style, 4bpp — a quarter of RGB5A3's footprint, which is
// the difference between streaming churn and actually holding a scene in
// the arena. Layout: 8x8 tiles of four 4x4 blocks (TL,TR,BL,BR); block =
// two 565 colors (native big-endian store) + 4 index bytes, MSB-first pairs.
// ponytail: box-fit encoder (min/max endpoints, projection indices) — fast
// and fine for VC-era textures; upgrade path is a PCA/iterative refit.
static inline uint16
to565(uint8 r, uint8 g, uint8 b)
{
	return ((r>>3)<<11) | ((g>>2)<<5) | (b>>3);
}

static void
tileCMPR(uint8 *dst, Raster *raster, int32 tw, int32 th)
{
	for(int32 ty = 0; ty < th; ty += 8)
	for(int32 tx = 0; tx < tw; tx += 8)
	for(int32 sub = 0; sub < 4; sub++){
		int32 bx = tx + (sub & 1)*4, by = ty + (sub >> 1)*4;
		uint8 px[16][4];
		bool trans = false;
		uint8 mn[3] = {255,255,255}, mx[3] = {0,0,0};
		for(int32 i = 0; i < 16; i++){
			sampleTexel(raster, bx + (i&3), by + (i>>2), tw, th,
			    &px[i][0], &px[i][1], &px[i][2], &px[i][3]);
			if(px[i][3] < 128){ trans = true; continue; }
			for(int32 c = 0; c < 3; c++){
				if(px[i][c] < mn[c]) mn[c] = px[i][c];
				if(px[i][c] > mx[c]) mx[c] = px[i][c];
			}
		}
		uint16 lo = to565(mn[0], mn[1], mn[2]);
		uint16 hi = to565(mx[0], mx[1], mx[2]);
		int32 dir[3] = { mx[0]-mn[0], mx[1]-mn[1], mx[2]-mn[2] };
		int32 len2 = dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2];
		uint16 *out16 = (uint16*)dst;
		uint8 *idx = dst + 4;
		if(trans){
			// alpha mode: c0 <= c1; palette lo, hi, mid, transparent
			out16[0] = lo;
			out16[1] = hi >= lo ? hi : lo;
			for(int32 row = 0; row < 4; row++){
				uint8 byte = 0;
				for(int32 x = 0; x < 4; x++){
					uint8 *p = px[row*4 + x];
					uint8 v;
					if(p[3] < 128)
						v = 3;
					else if(len2 == 0)
						v = 0;
					else{
						int32 t = ((p[0]-mn[0])*dir[0] +
						    (p[1]-mn[1])*dir[1] +
						    (p[2]-mn[2])*dir[2]) * 4 / len2;
						v = t <= 0 ? 0 : t >= 3 ? 1 : 2;
					}
					byte |= v << (6 - 2*x);
				}
				idx[row] = byte;
			}
		}else{
			// opaque mode: c0 > c1; palette hi, lo, 2/3, 1/3
			if(hi == lo){
				out16[0] = hi | 1;
				out16[1] = lo & ~1;
				idx[0] = idx[1] = idx[2] = idx[3] = 0x55; // all c1 = lo
			}else{
				out16[0] = hi > lo ? hi : lo;
				out16[1] = hi > lo ? lo : hi;
				bool flip = hi < lo; // endpoints swapped vs min/max
				for(int32 row = 0; row < 4; row++){
					uint8 byte = 0;
					for(int32 x = 0; x < 4; x++){
						uint8 *p = px[row*4 + x];
						int32 t = len2 ? ((p[0]-mn[0])*dir[0] +
						    (p[1]-mn[1])*dir[1] +
						    (p[2]-mn[2])*dir[2]) * 6 / len2 : 0;
						// t in 0..6 along lo->hi
						uint8 v = t >= 5 ? 0 : t <= 1 ? 1 : t >= 3 ? 2 : 3;
						if(flip) v = v == 0 ? 1 : v == 1 ? 0 :
						    v == 2 ? 3 : 2;
						byte |= v << (6 - 2*x);
					}
					idx[row] = byte;
				}
			}
		}
		dst += 8;
	}
}

// CMPR carries 1-bit alpha at best; textures with gradient alpha keep
// RGB5A3. Scan the staging pixels once at build time.
#define GX_USE_CMPR 1 // A/B: 0 = all textures RGB5A3, bypass the CMPR encoder

static bool
cmprEligible(Raster *raster)
{
	if(!GX_USE_CMPR)
		return false;
	if(raster->depth != 32)
		return true; // 16-bit staging is already 1-bit alpha
	for(int32 y = 0; y < raster->height; y++){
		uint8 *p = raster->pixels + y*raster->stride;
		for(int32 x = 0; x < raster->width; x++){
			uint8 a = p[x*4 + 3];
			if(a > 16 && a < 240)
				return false;
		}
	}
	return true;
}

void
gxRasterProbe(Raster *raster, uint32 *gxFmt, uint32 *firstWord)
{
	*gxFmt = 0xFF;
	*firstWord = 0;
	if(raster == nil)
		return;
	GxRaster *ext = GETGXRASTEREXT(raster);
	if(ext->hasTex)
		*gxFmt = ext->gxFmt;
	if(ext->tiled)
		*firstWord = *(uint32*)ext->tiled;
}

// Does sampling this raster ever yield alpha < 255? Formats without an
// alpha channel never can, which lets the draw path keep early-Z on.
bool32
gxRasterHasAlpha(Raster *raster)
{
	if(raster == nil)
		return 0;
	int32 base = raster->format & 0xF00;
	return !(base == Raster::C888 || base == Raster::C565 ||
	         base == Raster::C555 || base == Raster::LUM8);
}

// Lazily (re)build the GX texture for a raster; returns nil if it has no pixels.
GXTexObj*
gxGetTexture(Raster *raster)
{
	if(raster == nil || raster->width == 0 || raster->height == 0)
		return nil;

	GxRaster *ext = GETGXRASTEREXT(raster);
	// staging pixels are freed after tiling, so the cached-texture check must
	// come before the pixels check
	if(ext->hasTex && !ext->dirty)
		return &ext->obj;
	if(raster->pixels == nil)
		return ext->hasTex ? &ext->obj : nil;

	// Format: CMPR (4bpp) unless the texture needs gradient alpha, then
	// RGB5A3 (16bpp). Decided once per raster; rebuilds keep the format
	// so the tiled buffer size stays valid.
	if(!ext->hasTex)
		ext->gxFmt = cmprEligible(raster) ? GX_TF_CMPR : GX_TF_RGB5A3;
	bool cmpr = ext->gxFmt == GX_TF_CMPR;

	// Round dims up to the tile size (CMPR 8, RGB5A3 4). Cap at 512px per
	// axis — enough for VC's big textures at 640x480 output; 1024s are
	// what blow the console heap. UVs are normalized so sampling is safe.
	// (Tried 256 to buy arena headroom: free-at-crash moved 2289K -> 2198K,
	// i.e. no effect. The OOM is fragmentation on one large contiguous
	// request, not resident texture bytes — don't re-try this.)
	int32 align = cmpr ? 7 : 3;
	int32 tw = raster->width > 512 ? 512 : (raster->width + align) & ~align;
	int32 th = raster->height > 512 ? 512 : (raster->height + align) & ~align;
	if(tw < align+1) tw = align+1;
	if(th < align+1) th = align+1;
	int32 size = cmpr ? tw*th/2 : tw*th*2;
	if(ext->tiled == nil){
		ext->tiled = memalign(32, size);
		if(ext->tiled)
			ext->tiledSize = (uint32)size, ::gxTiledBytes += (uint32)size;
		if(ext->tiled == nil){
			// Counted, because this failing silently is what "black
			// silhouettes with oom 0" actually is. The geometry loaded, this
			// allocation did not, gxGetTexture hands back nil, and the mesh
			// draws untextured — which reads as a lighting or a model bug and
			// is neither. oom only ever counted geometry, so the one failure
			// the budget was being tuned against was invisible to the readout
			// used to tune it.
			::rwTexAllocFails++;
			return nil;
		}
	}
	{
		unsigned long long t0 = gettime();
		if(cmpr)
			tileCMPR((uint8*)ext->tiled, raster, tw, th);
		else
			tileRGB5A3((uint8*)ext->tiled, raster, tw, th);
		::gxTileUs += (unsigned)ticks_to_microsecs(gettime() - t0);
	}
	DCFlushRange(ext->tiled, size);

	GX_InitTexObj(&ext->obj, ext->tiled, tw, th,
	    ext->gxFmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&ext->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0,
	    GX_DISABLE, GX_DISABLE, GX_ANISO_1);
	// no GX_InvalidateTexAll here: textures build at stream time with no
	// frame draining the FIFO; beginUpdate invalidates once per frame

	ext->hasTex = 1;
	ext->dirty = 0;
	// A texture built mid-frame leaves a stale TMEM copy: the GP keeps
	// sampling whatever was cached at that address, so a freshly streamed
	// ped/vehicle mesh draws with another model's texels. beginUpdate's
	// once-per-frame invalidate is too late for anything built after it.
	// Deferred so the invalidate lands between draws, not at stream time
	// (per-texture invalidation there floods the FIFO with no frame
	// draining it).
	gxTexCacheDirty = 1;
	::gxTexBuilds++;

	// ponytail: plain textures never re-lock once drawn, and keeping both the
	// linear staging and the tiled copy doubles texture cost against a 16MB
	// arena. Free the staging; camera textures keep theirs (grabbed/redrawn).
	if((raster->type & 0xF) == Raster::TEXTURE && raster->pixels){
		rwFree(raster->pixels);
		raster->pixels = nil;
		raster->originalPixels = nil;
	}
	return &ext->obj;
}

// ponytail: textures are kept as linear RGBA8 staging here. GX wants 4x4
// tiled blocks, so the tiling/upload step belongs in the texture pipeline
// when 3D rendering lands; nothing samples these yet.
Raster*
rasterCreate(Raster *raster)
{
	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		return raster;
	}

	if(raster->flags & Raster::DONTALLOCATE)
		return raster;

	switch(raster->type){
	// The EFB is the camera target; there is no pixel buffer to allocate.
	case Raster::CAMERA:
	case Raster::ZBUFFER:
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		raster->pixels = nil;
		break;

	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
	default: {
		int32 depth = raster->depth ? raster->depth : 32;
		raster->depth = depth;
		raster->stride = raster->width*(depth/8);
		int32 size = raster->stride*raster->height;
		// non-must alloc: a failed texture must fail the stream load (which
		// evicts and retries), not exit(1) the whole game via mustmalloc
		raster->pixels = (uint8*)engine->memfuncs.rwmalloc(size,
		    MEMDUR_EVENT | ID_DRIVER);
		if(raster->pixels == nil){
			RWERROR((ERR_ALLOC, size));
			return nil;
		}
		memset(raster->pixels, 0, size);
		break;
	}
	}

	raster->originalPixels = raster->pixels;
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = raster->stride;
	return raster;
}

uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	(void)level;
	(void)lockMode;
	// staging may have been freed after tiling; re-materialize for writers
	// (mip uploads in readAsImage lock again after level 0 was tiled)
	if(raster->pixels == nil && raster->stride && raster->height){
		raster->pixels = (uint8*)engine->memfuncs.rwmalloc(
		    raster->stride*raster->height, MEMDUR_EVENT | ID_DRIVER);
		if(raster->pixels)
			memset(raster->pixels, 0, raster->stride*raster->height);
		raster->originalPixels = raster->pixels;
		// Remember this buffer is not the real staging data, so unlock does
		// not mark a already-built texture dirty and rebuild it from zeros.
		GETGXRASTEREXT(raster)->fabricated = 1;
	}
	raster->privateFlags |= Raster::PRIVATELOCK_WRITE;
	return raster->pixels;
}

void
rasterUnlock(Raster *raster, int32)
{
	raster->privateFlags &= ~Raster::PRIVATELOCK_WRITE;
	GxRaster *ext = GETGXRASTEREXT(raster);
	// Only a lock over the real staging data may invalidate a built texture.
	// A fabricated buffer (see rasterLock) holds zeros or a single mip level
	// in a level-0-sized allocation; rebuilding from it is what turned already
	// correct textures black or mapped the wrong part of an atlas onto a mesh.
	if(ext->fabricated){
		ext->fabricated = 0;
		if(ext->tiled){
			rwFree(raster->pixels);
			raster->pixels = nil;
			raster->originalPixels = nil;
		}
		return;
	}
	ext->dirty = 1;
}

uint8*
rasterLockPalette(Raster *raster, int32)
{
	return raster->palette;
}

void
rasterUnlockPalette(Raster*)
{
}

int32
rasterNumLevels(Raster*)
{
	return 1;
}

bool32
imageFindRasterFormat(Image *img, int32 type,
	int32 *pwidth, int32 *pheight, int32 *pdepth, int32 *pformat)
{
	assert(img->width != 0 && img->height != 0);

	*pwidth = img->width;
	*pheight = img->height;

	switch(img->depth){
	case 32:
		*pdepth = 32;
		*pformat = img->hasAlpha() ? Raster::C8888 : Raster::C888;
		break;
	case 24:
		*pdepth = 32;
		*pformat = Raster::C888;
		break;
	case 16:
		*pdepth = 16;
		*pformat = Raster::C1555;
		break;
	case 8:
	case 4:
		// expand palettised images; GX palettes are a later concern
		*pdepth = 32;
		*pformat = Raster::C8888;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	*pformat |= type;
	return 1;
}

static bool32
rasterFromImageBody(Raster *raster, Image *image);

bool32
rasterFromImage(Raster *raster, Image *image)
{
	bool32 r = rasterFromImageBody(raster, image);
	if(r){
		GETGXRASTEREXT(raster)->dirty = 1;
		// Tile eagerly so streamed textures never sit resident as 32bpp
		// staging — but not while locked: readAsImage locks around each mip
		// level and would find its pixels freed on the next lock.
		if((raster->privateFlags & Raster::PRIVATELOCK_WRITE) == 0)
			gxGetTexture(raster);
	}
	return r;
}

static bool32
rasterFromImageBody(Raster *raster, Image *image)
{
	if((raster->type & 0xF) != Raster::TEXTURE){
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	// unpalettize converts in place to depth 24 or 32, both of which the
	// loop below already handles. This used to also create and allocate a
	// full-size 32bpp Image that was never written to and destroyed at the
	// end of the function: a transient width*height*4 spike for every
	// palettised texture streamed, feeding the arena fragmentation that
	// makes the large Geometry::create realloc fail.
	if(image->depth <= 8)
		image->unpalettize(image->hasAlpha());

	uint8 *dst = raster->pixels;
	if(dst == nil)
		return 0;

	int32 depth = raster->depth;
	for(int32 y = 0; y < raster->height && y < image->height; y++){
		uint8 *src = image->pixels + y*image->stride;
		uint8 *out = dst + y*raster->stride;
		for(int32 x = 0; x < raster->width && x < image->width; x++){
			uint8 r = 0, g = 0, b = 0, a = 255;
			switch(image->depth){
			case 32: r = src[0]; g = src[1]; b = src[2]; a = src[3]; src += 4; break;
			case 24: r = src[0]; g = src[1]; b = src[2]; src += 3; break;
			default: src += image->bpp; break;
			}
			if(depth == 32){
				out[0] = r; out[1] = g; out[2] = b; out[3] = a;
				out += 4;
			}else{
				uint16 v = ((r>>3)<<10) | ((g>>3)<<5) | (b>>3) | (a ? 0x8000 : 0);
				*(uint16*)out = v;
				out += 2;
			}
		}
	}

	return 1;
}

// GX texture format ids as plain constants. The offline converter has to build
// these same blobs on a host with no gccore.h, so nothing in the tiling or the
// native-texture format may depend on the console headers.
enum { GXFMT_RGB5A3 = 0x5, GXFMT_CMPR = 0xE };

static void*
gxAllocTiled(uint32 size)
{
	return memalign(32, size);
}

// Publish a tiled blob as a usable texture: flush it out of the CPU cache so
// the GP sees it, then describe it to GX. Nothing here converts anything.
static void
gxFinishNativeRaster(Raster *raster, int32 tw, int32 th, uint32 size)
{
	GxRaster *ext = GETGXRASTEREXT(raster);
	DCFlushRange(ext->tiled, size);
	GX_InitTexObj(&ext->obj, ext->tiled, tw, th, ext->gxFmt,
	    GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&ext->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0,
	    GX_DISABLE, GX_DISABLE, GX_ANISO_1);
	ext->hasTex = 1;
	ext->dirty = 0;
	gxTexCacheDirty = 1;
}

// ---------------------------------------------------------------------------
// Native GX textures.
//
// The point of these is that a TXD converted ahead of time arrives already
// tiled, so loading one is a read into a correctly sized buffer and nothing
// else. The runtime path this replaces had a D3D raster, a full RGBA8 Image
// and an RGBA8 staging buffer live at the same time for every texture, which
// is what shattered the heap: measured 12.3MB live across ~16300 allocations
// with 4.4MB free split into ~9200 chunks. dca3 reaches the same conclusion
// for the Dreamcast — convert offline, do no conversion at runtime.
//
// Header is RenderWare's usual 88-byte native texture struct. gxFmt rides in
// the compression byte, and width/height are the *tiled* dimensions, so the
// reader needs no knowledge of the source image at all.
enum { GXNATIVE_HEADER = 88 };

// Bounded log of why a native read gave up. A nil return here fails the whole
// dictionary, and the streamer answers a failed load by requesting it again —
// so one silent rejection becomes an endless retry that drains the heap while
// streaming never advances. Worth naming the reason out loud.
static int32 gxNativeLogLeft = 40;
static void
gxNativeFail(const char *why, uint32 a, uint32 b)
{
	if(gxNativeLogLeft-- <= 0)
		return;
	char line[128];
	snprintf(line, sizeof(line), "NATIVE fail %s a=%u b=%u", why, a, b);
	DVD_FS_GUARD;
	FILE *f = fopen("dvd:/native.log", "a");
	if(f){ fprintf(f, "%s\n", line); fclose(f); }
}

Texture*
readNativeTexture(Stream *stream)
{
	uint32 structSize;
	uint8 header[GXNATIVE_HEADER];
	if(stream == nil || !findChunk(stream, ID_STRUCT, &structSize, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	if(structSize < sizeof(header)){
		gxNativeFail("structSize", structSize, sizeof(header));
		return nil;
	}
	stream->read8(header, sizeof(header));
	uint32 platform = readLE32(&header[0]);
	if(platform != PLATFORM_GAMECUBE){
		RWERROR((ERR_PLATFORM, platform));
		gxNativeFail("platform", platform, PLATFORM_GAMECUBE);
		return nil;
	}
	uint32 filterAddressing = readLE32(&header[4]);
	if(memchr(&header[8], '\0', 32) == nil || memchr(&header[40], '\0', 32) == nil){
		gxNativeFail("name", 0, 0);
		return nil;
	}
	uint32 format = readLE32(&header[72]);
	int32 tw = readLE16(&header[80]);
	int32 th = readLE16(&header[82]);
	uint8 gxFmt = header[87];
	if(tw <= 0 || th <= 0 || tw > 1024 || th > 1024){
		gxNativeFail("dims", tw, th);
		return nil;
	}

	Texture *tex = Texture::create(nil);
	if(tex == nil)
		return nil;
	tex->filterAddressing = filterAddressing;
	strncpy(tex->name, (char*)&header[8], 32);
	strncpy(tex->mask, (char*)&header[40], 32);

	uint32 size = stream->readU32();
	uint32 expect = gxFmt == GXFMT_CMPR ? (uint32)tw*th/2 : (uint32)tw*th*2;
	if(size != expect){
		gxNativeFail("size", size, expect);
		tex->destroy();
		return nil;
	}

	Raster *raster = Raster::create(tw, th, 16, format | Raster::TEXTURE |
	    Raster::DONTALLOCATE, PLATFORM_GAMECUBE);
	if(raster == nil){
		gxNativeFail("rastercreate", tw, th);
		tex->destroy();
		return nil;
	}
	GxRaster *ext = GETGXRASTEREXT(raster);
	ext->tiled = gxAllocTiled(size);
	if(ext->tiled == nil){
		gxNativeFail("alloc", size, 0);
		raster->destroy();
		tex->destroy();
		return nil;
	}
	// Straight into its final home. No staging, no Image, no conversion.
	stream->read8(ext->tiled, size);
	ext->gxFmt = gxFmt;
	gxFinishNativeRaster(raster, tw, th, size);
	tex->raster = raster;
	return tex;
}

void
writeNativeTexture(Texture *tex, Stream *stream)
{
	Raster *raster = tex->raster;
	GxRaster *ext = GETGXRASTEREXT(raster);
	int32 tw = raster->width, th = raster->height;
	uint32 size = ext->gxFmt == GXFMT_CMPR ? (uint32)tw*th/2 : (uint32)tw*th*2;

	writeChunkHeader(stream, ID_STRUCT, GXNATIVE_HEADER + 4 + size);
	uint8 header[GXNATIVE_HEADER];
	memset(header, 0, sizeof(header));
	writeLE32(&header[0], PLATFORM_GAMECUBE);
	writeLE32(&header[4], tex->filterAddressing);
	strncpy((char*)&header[8], tex->name, 32);
	strncpy((char*)&header[40], tex->mask, 32);
	writeLE32(&header[72], raster->format);
	writeLE32(&header[76], ext->gxFmt != GXFMT_CMPR);
	writeLE16(&header[80], tw);
	writeLE16(&header[82], th);
	header[84] = 16;                 // depth of the tiled copy
	header[85] = 1;                  // one level; mips are a later concern
	header[86] = Raster::TEXTURE;
	header[87] = ext->gxFmt;
	stream->write8(header, sizeof(header));
	stream->writeU32(size);
	stream->write8(ext->tiled, size);
}

uint32
getSizeNativeTexture(Texture *tex)
{
	Raster *raster = tex->raster;
	GxRaster *ext = GETGXRASTEREXT(raster);
	uint32 size = ext->gxFmt == GXFMT_CMPR ?
	    (uint32)raster->width*raster->height/2 :
	    (uint32)raster->width*raster->height*2;
	return 12 + GXNATIVE_HEADER + 4 + size;
}

Image*
rasterToImage(Raster *raster)
{
	if(raster->pixels == nil)
		return nil;

	Image *image = Image::create(raster->width, raster->height, 32);
	image->allocate();
	for(int32 y = 0; y < raster->height; y++){
		uint8 *src = raster->pixels + y*raster->stride;
		uint8 *dst = image->pixels + y*image->stride;
		for(int32 x = 0; x < raster->width; x++){
			if(raster->depth == 32){
				dst[0] = src[0]; dst[1] = src[1];
				dst[2] = src[2]; dst[3] = src[3];
				src += 4;
			}else{
				uint16 v = *(uint16*)src;
				dst[0] = ((v>>10)&0x1F)<<3;
				dst[1] = ((v>>5)&0x1F)<<3;
				dst[2] = (v&0x1F)<<3;
				dst[3] = (v&0x8000) ? 255 : 0;
				src += 2;
			}
			dst += 4;
		}
	}
	return image;
}

}
}

#endif
