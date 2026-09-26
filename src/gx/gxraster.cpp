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
#include <ogc/arqueue.h>
#include <malloc.h>
// Texture/vertex buffers are MEM1. MEM1 pressure gets solved in MEM1 or in
// ARAM, never here.
extern "C" void *gcBigAlloc(size_t); extern "C" void gcBigFree(void*); extern "C" int gcBigContains(const void*);   // B79 big-block heap (gamecube.cpp)
extern unsigned rwGeoAllocFails;   // geometry.cpp
#define gxTexAlloc(sz) memalign(32, (sz))
#define gxTexFree(p) free(p)

// libfat is shared with the game's streaming worker and must be serialised;
// the lock lives in CdStream_gamecube.cpp. Declared rather than included
// because librw does not get to see the game's headers.
extern "C" {
void CdStreamFsLock(void);
void CdStreamFsUnlock(void);
// Streaming accounting, defined in Streaming.cpp. A tiled texture allocated
// outside a load's measurement window is invisible to gResidentCost, so it is
// charged here instead; one allocated inside is already in the heap delta.
void CStreamingTexBytes(long delta);
int CStreamingMeasuring(void);
extern volatile int gcOptionalAlloc;   // gamecube.cpp, B177: fail instead of shedding
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
// ARAM tier counters (see gxAram below) and its LRU clock, which gx.cpp bumps
// once per presented frame. Global like gxTiledBytes: the skeleton's census
// reads them with a plain extern.
unsigned gxAramBytes, gxWsBytes, gxWsPeak, gxPageIns, gxWsStarved, gxShareBytes, gxWsShared;
unsigned gxAramStoreBytes;   // texel store capacity, for the streamer's ARAM pressure rule
unsigned gxColorBytes;   // colour-cache arrays resident (gx.cpp)
extern "C" { volatile unsigned gxDmaBusy; }   // MemoryWatcher: 1 while the main thread spins on an ARAM DMA
unsigned gxSpills, gxWsFrameBytes, gxWsFramePeak;
unsigned gxWsForced;   // B111: page-ins that had to GX_DrawDone and evict this frame's textures
extern "C" void *__real_memalign(size_t, size_t);
unsigned gxFrameNo;
// Set by CTxdStore around a dictionary read: its texels stay in MEM1 instead
// of the ARAM tier. fonts and hud — on screen every frame, drawn last, and
// the first to starve when the window is full of the world (B30: HUD text
// as solid blocks, 8325 starves).
int gxTierExempt;

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
	uint32 tiledSize;   // so the free path subtracts exactly what was added
	bool32 tiledCharged; // whether that size went onto ms_memoryUsed
	uint16 texFails; // consecutive tiling-alloc failures; gates the draw skip
	// ARAM tier (see gxAram below): where the texels live when they are not
	// in MEM1, the MEM1 window block they occupy while drawn, and the frame
	// that last drew them — the LRU key.
	uint32 aram;
	uint32 wsAddr;
	uint32 lastFrame;
	bool32 spill;      // wsAddr is a heap block (window was full), freed two frames on
	uint8 tlutSlot;      // B90: CI8 palette slot (GX_TLUT0+n)
	uint8 tlutDirty;
};

int32 nativeRasterOffset;
bool32 gxTexCacheDirty;
#define GETGXRASTEREXT(raster) PLUGINOFFSET(GxRaster, raster, nativeRasterOffset)

// ---------------------------------------------------------------------------
// ARAM texel tier.
//
// The GameCube has 24MB of MEM1 and 16MB of ARAM, and the GP samples textures
// from MEM1 only; ARAM is reachable by DMA alone. dca3 fits Vice City in 16MB
// of Dreamcast RAM because every texture lives in a separate 8MB of VRAM
// (vendor/librw/src/dc/alloc.cpp there); this is the same split on this
// hardware: a native texture's tiled texels go to ARAM at load — the MEM1
// copy is freed in the same call, so the streamer's heap delta never sees
// them — and come back through a fixed MEM1 window only while something
// draws them. The window is an LRU keyed by the frame that last bound the
// texture; nothing bound in the last two frames is ever evicted, because the
// GP may still be reading it. The ARAM budget leaves room for the audio bank
// that returns later (ADPCM, [[aram-is-full-of-audio]]).
//
// ponytail: first-fit spans with coalescing, one array each, no defrag. The
// ARAM side sees churn only through the streamer (a few textures a second);
// the MEM1 window churns per camera turn, and a full scan of ~2000 spans per
// page-in is well under the DMA it precedes.
#ifdef RW_GAMECUBE
#define GX_ARAM_TIER 1
#else
#define GX_ARAM_TIER 0
#endif


#if GX_ARAM_TIER
#include <ogc/aram.h>
#include <ogc/cache.h>

enum {
	GX_WS_BYTES     = 3072*1024,   // B85: 3.5MB -> 3MB; twins now share window blocks (B84)   // MEM1 window: what one frame can draw. 3MB starved the
	                               // cutscene characters (per-frame set 3.2MB+, spills need heap
	                               // the audio had taken) — B40 back to 3.5MB; audio pays instead.
	GX_ARAM_RESERVE = 5220*1024,   // audio: ADPCM bank 3835K + seven ped-comment slots 553K + three 256K stream rings (B68) = 5156K measured; B114: was 5600K, 380K of texel store given back
	GX_SPAN_GRAIN   = 32           // ARAM DMA and GX texture alignment
};

struct GxSpans
{
	struct Span { uint32 addr, size; bool32 used; };
	Span *spans;
	int32 count, cap;
	uint32 used;

	bool init(uint32 base, uint32 size, int32 capacity){
		spans = (Span*)malloc(sizeof(Span)*capacity);
		if(spans == nil) return false;
		spans[0].addr = base; spans[0].size = size; spans[0].used = 0;
		count = 1; cap = capacity; used = 0;
		return true;
	}
	uint32 alloc(uint32 size){
		size = (size + GX_SPAN_GRAIN-1) & ~(GX_SPAN_GRAIN-1);
		for(int32 i = 0; i < count; i++){
			Span *sp = &spans[i];
			if(sp->used || sp->size < size) continue;
			// Table full: take the whole span rather than refuse, which pushed the
			// texture onto the MEM1 heap.
			if(sp->size > size && count < cap){
				memmove(sp+2, sp+1, sizeof(Span)*(count-i-1));
				sp[1].addr = sp->addr + size; sp[1].size = sp->size - size; sp[1].used = 0;
				sp->size = size; count++;
			}
			sp->used = 1; used += sp->size;
			return sp->addr;
		}
		return 0;
	}
	void release(uint32 addr){
		for(int32 i = 0; i < count; i++){
			if(spans[i].addr != addr || !spans[i].used) continue;
			spans[i].used = 0; used -= spans[i].size;
			if(i+1 < count && !spans[i+1].used){
				spans[i].size += spans[i+1].size;
				memmove(&spans[i+1], &spans[i+2], sizeof(Span)*(count-i-2)); count--;
			}
			if(i > 0 && !spans[i-1].used){
				spans[i-1].size += spans[i].size;
				memmove(&spans[i], &spans[i+1], sizeof(Span)*(count-i-1)); count--;
			}
			return;
		}
	}
};

static GxSpans gxAram, gxWs;
static int32 gxTierState;          // 0 untried, 1 ready, -1 refused
static Raster **gxWsList;          // rasters currently holding a window block
static int32 gxWsCount, gxWsCap;

static bool
gxTierInit(void)
{
	if(gxTierState) return gxTierState > 0;
	gxTierState = -1;
	if(!AR_CheckInit()){
		// Same array size as sampman's for the same reason: whoever runs
		// first sizes AR_Alloc's block table for every user.
		static u32 aramBlocks[300];
		AR_Init(aramBlocks, 300);
	}
	ARQ_Init();
	uint32 avail = AR_GetSize() - AR_GetBaseAddress();
	printf("ARAM tier: AR size %u base %08X\n", AR_GetSize(), AR_GetBaseAddress());
	if(avail <= GX_ARAM_RESERVE + 1024*1024){ printf("ARAM tier: refused, avail %uK\n", avail/1024); return false; }
	uint32 size = avail - GX_ARAM_RESERVE;
	uint32 base = AR_Alloc(size);
	if(base == 0){ printf("ARAM tier: AR_Alloc failed\n"); return false; }
	void *win = memalign(GX_SPAN_GRAIN, GX_WS_BYTES);
	if(win == nil){ printf("ARAM tier: no MEM1 for the window\n"); return false; }
	gxWsCap = 2048;
	gxWsList = (Raster**)malloc(sizeof(Raster*)*gxWsCap);
	gxAramStoreBytes = size;
	if(gxWsList == nil || !gxAram.init(base, size, 6144) ||
	   !gxWs.init((uint32)win, GX_WS_BYTES, 2048)){
		printf("ARAM tier: span tables failed\n");
		return false;
	}
	printf("ARAM tier: %uK texels at %08X, MEM1 window %uK\n",
	    size/1024, base, GX_WS_BYTES/1024);
	gxTierState = 1;
	return true;
}

static void
gxAramTransfer(uint32 direction, void *mram, uint32 aram, uint32 size)
{
	ARQRequest request;
	gxDmaBusy = 1;
	// AESND shares this queue; direct DMA bypasses its completion tracking.
	ARQ_PostRequest(&request, 0x47585458, direction, ARQ_PRIO_LO,
	    aram, (u32)MEM_VIRTUAL_TO_PHYSICAL(mram), size);
	gxDmaBusy = 0;
}

// Move a freshly read tiled blob to ARAM and drop the MEM1 copy.
static bool
gxAramStore(GxRaster *ext, uint32 size)
{
	static int said;
	if(said++ == 0) printf("ARAM tier: first native texture, %u bytes\n", size);
	if(!gxTierInit()) return false;
	uint32 a = gxAram.alloc(size);
	if(a == 0) return false;
	DCFlushRange(ext->tiled, size);
	gxAramTransfer(ARQ_MRAMTOARAM, ext->tiled, a, size);
	free(ext->tiled);
	ext->tiled = nil;
	ext->aram = a;
	::gxAramBytes += size;
	return true;
}

static void
gxWsDrop(Raster *raster)
{
	GxRaster *ext = GETGXRASTEREXT(raster);
	if(ext->wsAddr == 0) return;
	if(ext->spill){
		if(gcBigContains((void*)ext->wsAddr)) gcBigFree((void*)ext->wsAddr); else free((void*)ext->wsAddr);
		ext->spill = 0;
	}else{
		// B84: rasters that share ARAM texels (B74) share the window block too;
		// the block goes back only with its last user.
		bool32 shared = 0;
		for(int32 i = 0; i < gxWsCount && !shared; i++)
			if(gxWsList[i] != raster && GETGXRASTEREXT(gxWsList[i])->wsAddr == ext->wsAddr) shared = 1;
		if(!shared){
			gxWs.release(ext->wsAddr);
			::gxWsBytes -= ext->tiledSize;
		}
	}
	ext->wsAddr = 0;
	for(int32 i = 0; i < gxWsCount; i++)
		if(gxWsList[i] == raster){
			gxWsList[i] = gxWsList[--gxWsCount];
			break;
		}
}

// Oldest window occupant not bound this frame, or nil.
static Raster*
gxWsVictim(bool32 force)
{
	Raster *best = nil; uint32 bestFrame = 0;
	for(int32 i = 0; i < gxWsCount; i++){
		GxRaster *e = GETGXRASTEREXT(gxWsList[i]);
		// Only this frame's textures are pinned (B64): showRaster syncs the GP
		// before the next frame starts, so last frame's blocks are free to reuse.
		// Pinning two frames left no victims once a frame's set filled the window
		// (starve 180/s, textures drawn black).
		// B111: after a GX_DrawDone the pin is void — everything issued so far
		// has been drawn — so the caller may force this frame's textures too.
		if(e->spill || (!force && e->lastFrame + 1 > ::gxFrameNo)) continue;
		if(best == nil || e->lastFrame < bestFrame){ best = gxWsList[i]; bestFrame = e->lastFrame; }
	}
	return best;
}

// B90: CI8 textures (ci8pack.py, lossless RGB5A3 palettes) bind through a
// TLUT. 16 slots of 256 entries in TMEM, handed out round-robin; the palette
// rides at the end of the window block, so a page-in brings it along.
static Raster *gxTlutOwner[16];
static uint32 gxTlutNext;
static void
gxInitWindowTexObj(Raster *raster, GxRaster *ext, uint32 blk)
{
	if(ext->gxFmt == GX_TF_CI8){
		if(ext->tlutSlot == 0xFF || gxTlutOwner[ext->tlutSlot] != raster){
			ext->tlutSlot = (uint8)(gxTlutNext++ & 15);
			gxTlutOwner[ext->tlutSlot] = raster;
		}
		GX_InitTexObjCI(&ext->obj, (void*)blk, raster->width, raster->height,
		    GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0 + ext->tlutSlot);
		ext->tlutDirty = 1;
	}else
		GX_InitTexObj(&ext->obj, (void*)blk, raster->width, raster->height,
		    ext->gxFmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&ext->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0,
	    GX_DISABLE, GX_DISABLE, GX_ANISO_1);
}
// Called at every bind of a CI8 raster: load its palette into its slot unless
// the slot still holds it.
static void
gxBindTlut(Raster *raster, GxRaster *ext)
{
	if(ext->gxFmt != GX_TF_CI8 || ext->wsAddr == 0) return;
	if(gxTlutOwner[ext->tlutSlot] != raster){
		ext->tlutSlot = (uint8)(gxTlutNext++ & 15);
		gxTlutOwner[ext->tlutSlot] = raster;
		GX_InitTexObjTlut(&ext->obj, GX_TLUT0 + ext->tlutSlot);
		ext->tlutDirty = 1;
	}
	if(ext->tlutDirty){
		GXTlutObj tlut;
		GX_InitTlutObj(&tlut, (void*)(ext->wsAddr + (uint32)raster->width*raster->height), GX_TL_RGB5A3, 256);
		GX_LoadTlut(&tlut, GX_TLUT0 + ext->tlutSlot);
		ext->tlutDirty = 0;
	}
}

// Bring an ARAM-resident texture into the MEM1 window for drawing.
extern "C" { extern volatile const char *gMainWhere; }
static bool
gxPageIn(Raster *raster, GxRaster *ext)
{
	gMainWhere = "page-in";
	uint32 size = ext->tiledSize;
	// B84: a twin raster (same ARAM span, B74 dedupe) already in the window
	// means no DMA and no new block: point at its texels.
	for(int32 i = 0; i < gxWsCount && gxWsCount < gxWsCap; i++){
		GxRaster *o = GETGXRASTEREXT(gxWsList[i]);
		if(o->aram == ext->aram && o->wsAddr && !o->spill && o->tiledSize == size){
			gxInitWindowTexObj(raster, ext, o->wsAddr);
			ext->wsAddr = o->wsAddr;
			ext->spill = 0;
			gxWsList[gxWsCount++] = raster;
			::gxWsShared++;
			return true;
		}
	}
	uint32 blk = gxWs.alloc(size);
	bool32 spill = 0, forced = 0;
	while(blk == 0){
		Raster *victim = gxWsVictim(forced);
		if(victim == nil && !forced && gxWsCount > 0){
			// B111: the window holds only this frame's textures (or is too
			// fragmented for this block). Let the GPU finish everything issued
			// so far; then this frame's earlier textures are victims too, and
			// the draw stays textured instead of starving. One stall per
			// starvation, counted as `forced`; b110 had 1352 untextured draws
			// and 2700 heap emergencies from the spill path below.
			GX_DrawDone();
			forced = 1;
			::gxWsForced++;
			continue;
		}
		if(victim == nil || gxWsCount >= gxWsCap){
			// Window full of this frame's textures: borrow from the heap for
			// two frames rather than draw untextured. Counted, so a window
			// that is too small shows up as `spill` instead of as a glitch.
			blk = (uint32)gcBigAlloc(size);   // B79: spills come from the big-block chunks, not the general heap
			// B111: __real_memalign, not the wrapped one — a failed spill used
			// to enter gcHeapFail (emergency shed + arena carve) every time.
			if(blk == 0) blk = (uint32)__real_memalign(GX_SPAN_GRAIN, size);
			if(blk == 0){
				::gxWsStarved++;
				return false;
			}
			spill = 1;
			::gxSpills++;
			break;
		}
		gxWsDrop(victim);
		blk = gxWs.alloc(size);
	}
	// Any cache line of the previous occupant must go before the DMA lands,
	// or a later write-back would put stale bytes over the new texels.
	DCInvalidateRange((void*)blk, size);
	gxAramTransfer(ARQ_ARAMTOMRAM, (void*)blk, ext->aram, size);
	gxInitWindowTexObj(raster, ext, blk);
	// The block may have held another texture two frames ago and TMEM may
	// still cache it by address: invalidate now, mid-frame, not at the next
	// beginUpdate.
	GX_InvalidateTexAll();
	ext->wsAddr = blk;
	ext->spill = spill;
	gxWsList[gxWsCount++] = raster;
	if(!spill){
		::gxWsBytes += size;
		if(::gxWsBytes > ::gxWsPeak) ::gxWsPeak = ::gxWsBytes;
	}
	::gxPageIns++;
	return true;
}

// Once per presented frame (gx.cpp showRaster): measure the frame's texture
// set — what the window must hold — and return spill blocks the GP is done
// with (two frames old).
void
gxTierFrameEnd(void)
{
	uint32 done = ::gxFrameNo - 1;
	uint32 bytes = 0;
	for(int32 i = 0; i < gxWsCount; i++){
		GxRaster *e = GETGXRASTEREXT(gxWsList[i]);
		if(e->lastFrame == done) bytes += e->tiledSize;
	}
	::gxWsFrameBytes = bytes;
	if(bytes > ::gxWsFramePeak) ::gxWsFramePeak = bytes;
	for(int32 i = 0; i < gxWsCount; ){
		GxRaster *e = GETGXRASTEREXT(gxWsList[i]);
		if(e->spill && e->lastFrame + 2 <= ::gxFrameNo)
			gxWsDrop(gxWsList[i]);   // swaps the tail in; re-check this slot
		else
			i++;
	}
}

// ARAM texel sharing (B74). 52% of the archive's texel bytes are exact
// duplicates across TXDs (white128a x56, road2_256 x38, ...). Every native
// texture is hashed as it streams to ARAM; a duplicate of a resident blob gives
// its span back and points at the resident one, refcounted. ARAM effectively
// doubles; the disc bytes are still read (a shared TXD on disc is the next step).
struct GxShare { uint64 hash; uint32 aram, size; uint16 refs; uint16 fmt; };
enum { GX_SHARE_N = 4096 };
static GxShare gxShare[GX_SHARE_N];
static inline uint64 gxHashInit(void) { return 14695981039346656037ull; }
static inline uint64 gxHashBytes(uint64 h, const uint8 *p, uint32 n)
{
	for(uint32 i = 0; i < n; i += 4){   // every 4th byte: 8x cheaper, still content-defined with the full size in the key
		h ^= p[i]; h *= 1099511628211ull;
	}
	return h;
}
static GxShare *gxShareFind(uint64 hash, uint32 size, uint16 fmt)
{
	uint32 i = (uint32)(hash ^ (hash >> 32)) % GX_SHARE_N;
	for(uint32 k = 0; k < 64; k++, i = (i + 1) % GX_SHARE_N){
		GxShare *e = &gxShare[i];
		if(e->refs == 0) return nil;
		if(e->hash == hash && e->size == size && e->fmt == fmt) return e;
	}
	return nil;
}
static GxShare *gxShareInsert(uint64 hash, uint32 aram, uint32 size, uint16 fmt)
{
	uint32 i = (uint32)(hash ^ (hash >> 32)) % GX_SHARE_N;
	for(uint32 k = 0; k < 64; k++, i = (i + 1) % GX_SHARE_N){
		GxShare *e = &gxShare[i];
		if(e->refs == 0){ e->hash = hash; e->aram = aram; e->size = size; e->fmt = fmt; e->refs = 1; return e; }
	}
	return nil;   // table region full: this texture is simply not shared
}
// true = the span is still in use by another raster (do not free it)
static bool gxShareRelease(uint32 aram)
{
	for(uint32 i = 0; i < GX_SHARE_N; i++){
		GxShare *e = &gxShare[i];
		if(e->refs && e->aram == aram){
			if(--e->refs == 0){ e->hash = 0; e->aram = 0; return false; }
			gxShareBytes -= e->size;
			return true;
		}
	}
	return false;
}

static void
gxAramRelease(Raster *raster, GxRaster *ext)
{
	if(ext->wsAddr) gxWsDrop(raster);
	if(ext->aram){
		if(gxShareRelease(ext->aram)){   // still referenced by another raster
			ext->aram = 0; ext->tiledSize = 0;
			return;
		}
		gxAram.release(ext->aram);
		::gxAramBytes -= ext->tiledSize;
		ext->aram = 0;
		ext->tiledSize = 0;
	}
}
#else
static inline bool gxAramStore(GxRaster*, uint32) { return false; }
static inline bool gxPageIn(Raster*, GxRaster*) { return false; }
static inline void gxAramRelease(Raster*, GxRaster*) {}
static inline void gxBindTlut(Raster*, GxRaster*) {}
void gxTierFrameEnd(void) {}
#endif

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
	if(g->colors) ::gxColorBytes -= g->colorCount*sizeof(RGBA);
	rwFree(g->colors);
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
// GX_S8 normals carry a fixed 6 fraction bits: 1.0 is stored as 64.
static inline int8
gxQuantS8(float v)
{
	float q = v*64.0f + (v < 0.0f ? -0.5f : 0.5f);
	return (int8)(q > 127.0f ? 127 : q < -127.0f ? -127 : (int)q);
}

void
gxPackGeometry(Geometry *geo)
{
	if(geo == nil || geo->numVertices <= 0 || (geo->flags & Geometry::NATIVE))
		return;
	GxGeoExt *g = PLUGINOFFSET(GxGeoExt, geo, gxGeoOffset);
	if(g->packed & GXPACK_TRIED)
		return;
	g->arraysFlushed = 0;
	g->packed |= GXPACK_TRIED;
	if(geo->numMorphTargets != 1 || geo->morphTargets == nil)
		return;
	if(Skin::get(geo))
		return;
	// Env-mapped meshes pack too. The refusal that used to sit here existed
	// only because the env stage in atomicRenderCB was gated on unpacked
	// positions; packing keeps the float normals the texgen needs (the
	// memmove below preserves them), so that gate is now just `normals`.
	// Vehicles are all env-mapped, and at ~25 models resident they were the
	// largest unpacked geometry left — 20 float bytes a vertex down to 10.

	MorphTarget *mt = &geo->morphTargets[0];
	V3d *verts = mt->vertices;
	if(verts == nil)
		return;
	int32 n = geo->numVertices;
	TexCoords *uv = geo->numTexCoordSets == 1 ? geo->texCoords[0] : nil;
	V3d *nrm = mt->normals;

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
	// Normals go int8 alongside (dca3 packs them the same way): 3 bytes a
	// vertex against 12, and the morph block then shrinks to its header.
	// ponytail: only when positions pack too — a geometry that keeps float
	// positions keeps float normals, so the morph block has one shape each way.
	size_t nrmSz = (ps >= 0 && nrm) ? (size_t)n*3 : 0;
	uint8 *blk = (uint8*)rwMalloc(posSz + uvSz + nrmSz, MEMDUR_EVENT | ID_GEOMETRY);
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
	if(nrmSz){
		int8 *q = (int8*)(blk + posSz + uvSz);
		g->nrm = q;
		for(int32 i = 0; i < n; i++){
			*q++ = gxQuantS8(nrm[i].x);
			*q++ = gxQuantS8(nrm[i].y);
			*q++ = gxQuantS8(nrm[i].z);
		}
		g->packed |= GXPACK_NRM;
	}

	// Release what we just replaced. Shrinking reallocs only: the block can
	// never end up larger than it started, so this eases fragmentation rather
	// than adding to it — the same reasoning dropTrianglesAfterInstancing uses.
	if(g->packed & GXPACK_POS){
		size_t keep = sizeof(MorphTarget);
		size_t vertSz = (size_t)n*sizeof(V3d);
		bool keepNrm = mt->normals != nil && !(g->packed & GXPACK_NRM);
		if(keepNrm){
			memmove(mbase + keep, mbase + keep + vertSz, vertSz);
			keep += vertSz;
		}
		gxPackSaved += (uint32)vertSz;
		if(g->packed & GXPACK_NRM)
			gxPackSaved += (uint32)(vertSz - (size_t)n*3);
		uint8 *shrunk = (uint8*)rwRealloc(mbase, keep, MEMDUR_EVENT | ID_GEOMETRY);
		if(shrunk)
			mbase = shrunk;
		geo->morphTargets = (MorphTarget*)mbase;
		mt = &geo->morphTargets[0];
		mt->vertices = nil;
		mt->normals = keepNrm ? (V3d*)(mbase + sizeof(MorphTarget)) : nil;
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

// B178: GameCube native geometry, written offline by tools/gamecube/dffnative.py:
// gxPackGeometry's output computed on the host. Positions and uv0 are the
// packer's int16 arrays, read straight into the packed block; normals turn
// int8 on the way in, colours land in the attribute block. No float arrays
// ever exist, so the 100-180K transient blocks Geometry::create asked for —
// the allocation that failed on a fragmented heap and left big buildings at
// LOD for good — are gone, and the DFF on the disc is about half the size.
// The geometry leaves here exactly as a packed generic one, NATIVE cleared.
enum { GXNAT_POS = 1, GXNAT_NORMAL = 2, GXNAT_COLOR = 4, GXNAT_UV0 = 8, GXNAT_UV2 = 16 };
uint32 gxNativeGeoms;
static void gxNativeFail(const char *why, uint32 a, uint32 b);

Stream*
gxReadNativeGeometry(Stream *stream, int32 len, void *object, int32, int32)
{
	Geometry *geo = (Geometry*)object;
	uint32 size, version;
	if(!findChunk(stream, ID_STRUCT, &size, &version) || size < 24){
		gxNativeFail("geo-struct", size, 0);
		return nil;
	}
	uint32 meta[6];   // platform, version, vertices, attributes, shifts, data bytes
	stream->read32(meta, sizeof(meta));
	uint32 n = meta[2], attr = meta[3], dataLen = meta[5];
	size_t posSz = (size_t)n*6, pad = (4 - (posSz & 3)) & 3;
	size_t expect = posSz + ((attr & (GXNAT_NORMAL|GXNAT_COLOR|GXNAT_UV0)) ? pad : 0) +
	    ((attr & GXNAT_NORMAL) ? (size_t)n*12 : 0) + ((attr & GXNAT_COLOR) ? (size_t)n*4 : 0) +
	    ((attr & GXNAT_UV0) ? (size_t)n*4 : 0);
	if(meta[0] != PLATFORM_GAMECUBE || meta[1] != 1 || n != (uint32)geo->numVertices ||
	   !(attr & GXNAT_POS) || (attr & GXNAT_UV2) || geo->numMorphTargets != 1 ||
	   dataLen != expect || size != 24 + dataLen){
		gxNativeFail("geo-meta", n, attr);
		return nil;
	}
	GxGeoExt *g = PLUGINOFFSET(GxGeoExt, geo, gxGeoOffset);
	size_t uvSz = (attr & GXNAT_UV0) ? (size_t)n*4 : 0, nrmSz = (attr & GXNAT_NORMAL) ? (size_t)n*3 : 0;
	uint8 *blk = (uint8*)rwMalloc(posSz + uvSz + nrmSz, MEMDUR_EVENT | ID_GEOMETRY);
	uint8 *col = (attr & GXNAT_COLOR) ? (uint8*)rwMalloc((size_t)n*4, MEMDUR_EVENT | ID_GEOMETRY) : nil;
	if(blk == nil || ((attr & GXNAT_COLOR) && col == nil)){
		rwFree(blk); rwFree(col);
		rwGeoAllocFails++;
		return nil;
	}
	stream->read16(blk, posSz);
	if(pad && expect > posSz)
		stream->seek(pad);
	if(attr & GXNAT_NORMAL){
		int8 *q = (int8*)(blk + posSz + uvSz);
		float buf[3*128];
		for(uint32 done = 0; done < n; ){
			uint32 k = n - done < 128 ? n - done : 128;
			stream->read32(buf, k*12);
			for(uint32 i = 0; i < 3*k; i++)
				*q++ = gxQuantS8(buf[i]);
			done += k;
		}
	}
	if(col)
		stream->read8(col, (size_t)n*4);
	if(uvSz)
		stream->read16(blk + posSz, uvSz);

	g->packBase = blk;
	g->pos = (int16*)blk;
	g->posShift = (uint8)(meta[4] & 0xFF);
	g->packed = GXPACK_TRIED | GXPACK_POS;
	if(uvSz){
		g->uv = (int16*)(blk + posSz);
		g->uvShift = (uint8)((meta[4] >> 8) & 0xFF);
		g->packed |= GXPACK_UV;
	}
	if(nrmSz){
		g->nrm = (int8*)(blk + posSz + uvSz);
		g->packed |= GXPACK_NRM;
	}
	g->arraysFlushed = 0;
	geo->attribBase = col;
	geo->colors = (RGBA*)col;
	for(int32 i = 0; i < 8; i++)
		geo->texCoords[i] = nil;
	geo->triangles = nil;
	geo->numTriangles = 0;
	geo->flags &= ~Geometry::NATIVE;
	gxNativeGeoms++;
	(void)len; (void)version;
	return stream;
}

bool32
gxMoveGeometryMemory(Geometry *geo, void *(*move)(void*), bool32 onlyOne)
{
	if(geo == nil || (geo->flags & Geometry::NATIVE) || Skin::get(geo)) return false;
	GxGeoExt *g = PLUGINOFFSET(GxGeoExt, geo, gxGeoOffset);
	bool32 changed = false;
	uintptr old = (uintptr)geo->attribBase;
	uint8 *p = (uint8*)move(geo->attribBase);
	if((uintptr)p != old){
		uintptr delta = (uintptr)p - old;
		geo->attribBase = p;
		if(geo->triangles) geo->triangles = (Triangle*)((uintptr)geo->triangles + delta);
		if(geo->colors) geo->colors = (RGBA*)((uintptr)geo->colors + delta);
		for(int i = 0; i < geo->numTexCoordSets; i++)
			if(geo->texCoords[i]) geo->texCoords[i] = (TexCoords*)((uintptr)geo->texCoords[i] + delta);
		g->arraysFlushed = 0; changed = true;
		if(onlyOne) return true;
	}
	old = (uintptr)geo->morphTargets;
	p = (uint8*)move(geo->morphTargets);
	if((uintptr)p != old){
		uintptr delta = (uintptr)p - old;
		geo->morphTargets = (MorphTarget*)p;
		for(int i = 0; i < geo->numMorphTargets; i++){
			MorphTarget *m = &geo->morphTargets[i];
			if(m->vertices) m->vertices = (V3d*)((uintptr)m->vertices + delta);
			if(m->normals) m->normals = (V3d*)((uintptr)m->normals + delta);
		}
		g->arraysFlushed = 0; changed = true;
		if(onlyOne) return true;
	}
	old = (uintptr)g->packBase;
	p = (uint8*)move(g->packBase);
	if((uintptr)p != old){
		uintptr delta = (uintptr)p - old;
		g->packBase = p;
		if(g->pos) g->pos = (int16*)((uintptr)g->pos + delta);
		if(g->uv) g->uv = (int16*)((uintptr)g->uv + delta);
		if(g->nrm) g->nrm = (int8*)((uintptr)g->nrm + delta);
		g->arraysFlushed = 0; changed = true;
		if(onlyOne) return true;
	}
	old = (uintptr)geo->meshHeader;
	if(g->colors){
		RGBA *colors = (RGBA*)move(g->colors);
		if(colors != g->colors){
			g->colors = colors;
			DCFlushRange(colors, g->colorCount*sizeof(RGBA));
			changed = true;
			if(onlyOne) return true;
		}
	}
	p = (uint8*)move(geo->meshHeader);
	if((uintptr)p != old){
		uintptr delta = (uintptr)p - old;
		geo->meshHeader = (MeshHeader*)p;
		Mesh *meshes = geo->meshHeader->getMeshes();
		for(int i = 0; i < geo->meshHeader->numMeshes; i++)
			if(meshes[i].indices) meshes[i].indices = (uint16*)((uintptr)meshes[i].indices + delta);
		changed = true;
	}
	return changed;
}

static void*
copyGeoExt(void *dst, void *, int32 offset, int32)
{
	memset(PLUGINOFFSET(GxGeoExt, dst, offset), 0, sizeof(GxGeoExt));
	return dst;
}

// Tiled bytes one raster holds in MEM1, for the skeleton's OOM bill. C
// linkage and a void* so the caller needs none of this file's types.
extern "C" unsigned
gcRasterTiledBytes(void *raster)
{
	if(raster == nil)
		return 0;
	return GETGXRASTEREXT((Raster*)raster)->tiledSize;
}

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
	gxAramRelease((Raster*)object, ext);
	if(ext->tiled){
		::gxTiledBytes -= ext->tiledSize;
		if(ext->tiledCharged)
			CStreamingTexBytes(-(long)ext->tiledSize);
		ext->tiledSize = 0;
		ext->tiledCharged = 0;
		gxTexFree(ext->tiled);
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
// EFB -> this raster's tiled buffer, by GP copy. The raster half of
// rasterRenderFast (gx.cpp), here because GxRaster is private to this file.
// Caller owns the copy-filter state around this.
bool32
gxGrabEFB(Raster *dst, int32 w, int32 h)
{
	GxRaster *ext = GETGXRASTEREXT(dst);
	// RGB565: no alpha to preserve in a frame grab, half the bytes of RGBA8.
	uint32 need = GX_GetTexBufferSize(w, h, GX_TF_RGB565, GX_FALSE, 0);
	if(ext->tiled == nil || ext->tiledSize < need){
		// B177: the grab is optional (rain drops, trails, scope). It no longer
		// happens at boot for the colour filter, so it lands on a busy heap:
		// take it only if it fits, never by shedding models, and after a miss
		// wait a second before asking again.
		static uint32 missFrame;
		if(missFrame && ::gxFrameNo - missFrame < 60)
			return 0;
		if(ext->tiled)
			gxTexFree(ext->tiled);
		ext->hasTex = 0;
		gcOptionalAlloc = 1;
		ext->tiled = (void*)gxTexAlloc(need);
		gcOptionalAlloc = 0;
		if(ext->tiled == nil){
			missFrame = ::gxFrameNo ? ::gxFrameNo : 1;
			return 0;
		}
		missFrame = 0;
		ext->tiledSize = need;
	}
	// The CPU cache may hold lines over this buffer; a writeback after the
	// GP's DMA would corrupt the copy.
	DCInvalidateRange(ext->tiled, need);
	GX_SetTexCopySrc(0, 0, w, h);
	GX_SetTexCopyDst(w, h, GX_TF_RGB565, GX_FALSE);
	GX_CopyTex(ext->tiled, GX_FALSE);
	// Bounded by hardware: waits for the copy pipeline to drain, so a draw
	// that samples this texture next cannot race the GP writing it.
	GX_PixModeSync();
	GX_InitTexObj(&ext->obj, ext->tiled, w, h, GX_TF_RGB565,
	    GX_CLAMP, GX_CLAMP, GX_FALSE);
	ext->hasTex = 1;
	ext->dirty = 0;
	ext->fabricated = 0;
	ext->gxFmt = GX_TF_RGB565;
	gxTexCacheDirty = 1;   // TMEM may cache the previous frame at this address
	return 1;
}

GXTexObj*
gxGetTexture(Raster *raster)
{
	if(raster == nil || raster->width == 0 || raster->height == 0)
		return nil;

	GxRaster *ext = GETGXRASTEREXT(raster);
	if(ext->aram){
		if(ext->wsAddr == 0 && !gxPageIn(raster, ext))
			return nil;          // window full of this frame's textures: untextured once
		ext->lastFrame = ::gxFrameNo;
		gxBindTlut(raster, ext);
		return &ext->obj;
	}
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
	if(ext->tiled == nil && ext->texFails && ext->lastFrame + 120 > ::gxFrameNo)
		return nil;   // B91: a 600K motion-blur raster failing every frame dragged the emergency shed with it
	if(ext->tiled == nil){
		ext->tiled = gxTexAlloc(size);
		if(ext->tiled){
			ext->tiledSize = (uint32)size;
			::gxTiledBytes += (uint32)size;
			// Only when the streamer is not already measuring this load: it
			// tiles eagerly from rasterFromImage, and those bytes are inside
			// the heap delta gResidentCost takes. Charging both would count
			// them twice.
			ext->tiledCharged = !CStreamingMeasuring();
			if(ext->tiledCharged)
				CStreamingTexBytes((long)size);
		}
		if(ext->tiled == nil){
			// Counted, because this failing silently is what "black
			// silhouettes with oom 0" actually is. The geometry loaded, this
			// allocation did not, gxGetTexture hands back nil, and the mesh
			// draws untextured — which reads as a lighting or a model bug and
			// is neither. oom only ever counted geometry, so the one failure
			// the budget was being tuned against was invisible to the readout
			// used to tune it.
			::rwTexAllocFails++;
			ext->lastFrame = ::gxFrameNo;
			if(ext->texFails < 1000)
				ext->texFails++;
			return nil;
		}
	}
	if(cmpr)
		tileCMPR((uint8*)ext->tiled, raster, tw, th);
	else
		tileRGB5A3((uint8*)ext->tiled, raster, tw, th);
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
	ext->texFails = 0;

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

// TRUE while this texture's tiled copy failed to allocate but a retry can
// still succeed (staging pixels alive): the caller skips the mesh for those
// frames instead of drawing it untextured — prelight-dark, the "textures
// flashing dark" while the streamer makes room. Bounded: a texture stuck
// failing must eventually draw SOMETHING; the dark silhouette is then the
// diagnostic, never permanently missing geometry.
// ponytail: 60 failed draw-attempts ~ a second; a real eviction ladder in
// the allocator is the upgrade path if sustained failures ever show up.
bool32
gxTexturePending(Raster *raster)
{
	if(raster == nil || raster->pixels == nil)
		return 0;
	GxRaster *ext = GETGXRASTEREXT(raster);
	return !ext->hasTex && ext->tiled == nil &&
	    ext->texFails > 0 && ext->texFails < 60;
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
	// CAMERATEXTURE the same: its content arrives by GP copy
	// (rasterRenderFast), never from the CPU, so the linear pixel buffer
	// would be dead weight — 600KB per postfx buffer. gxGetTexture already
	// tolerates pixels == nil.
	case Raster::CAMERATEXTURE:
	case Raster::CAMERA:
	case Raster::ZBUFFER:
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		raster->pixels = nil;
		break;

	case Raster::TEXTURE:
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
enum { GXFMT_IA4 = 0x2, GXFMT_RGB5A3 = 0x5, GXFMT_CMPR = 0xE };
// Bytes of a tiled level-0 image in each native format: CMPR 4bpp, IA4 8bpp,
// RGB5A3 16bpp. The offline converter (txdconv) sizes with the same rule.
static inline uint32
gxNativeSize(uint8 fmt, int32 tw, int32 th)
{
	return fmt == GXFMT_CMPR ? (uint32)tw*th/2 :
	       fmt == GXFMT_IA4  ? (uint32)tw*th   :
	       fmt == GX_TF_CI8   ? (uint32)tw*th + 512 : (uint32)tw*th*2;   // B90: CI8 carries its palette (ci8pack.py)
}

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

// Why a native read gave up: a nil return fails the whole dictionary, and the
// streamer re-requests a failed load, so the reason is worth one line.
static void
gxNativeFail(const char *why, uint32 a, uint32 b)
{
	printf("NATIVE fail %s a=%u b=%u\n", why, (unsigned)a, (unsigned)b);
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
	uint32 expect = gxNativeSize(gxFmt, tw, th);
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
	ext->tiledSize = size;
	ext->gxFmt = gxFmt;
#if GX_ARAM_TIER
	if(filterAddressing & 0x80000000u){
		// B89: shared-pool reference (tools/gamecube/sharedpool.py): no texels
		// here, just the content hash of a texture models/shared.txd keeps
		// resident in ARAM. Same table the B74 dedupe fills.
		uint32 lo = stream->readU32(), hi = stream->readU32();
		uint64 h = ((uint64)hi << 32) | lo;
		GxShare *sh = gxShareFind(h, size, (uint16)gxFmt);
		if(sh == nil){
			gxNativeFail("shared-miss", hi, lo);
			raster->destroy(); tex->destroy();
			return nil;
		}
		tex->filterAddressing = filterAddressing & 0x7fffffffu;
		ext->aram = sh->aram; sh->refs++; gxShareBytes += size;
		GX_InitTexObj(&ext->obj, nil, tw, th, gxFmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjLOD(&ext->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0,
		    GX_DISABLE, GX_DISABLE, GX_ANISO_1);
		ext->hasTex = 1;
		ext->dirty = 0;
		tex->raster = raster;
		return tex;
	}
	// ARAM-bound texels never touch the heap: 32K pieces through one static
	// staging buffer, each DMA'd as it lands. B42 lost a cutscene's dictionary
	// to "NATIVE fail alloc 131072" — the old path wanted the whole texture in
	// MEM1 first, on a heap at the floor.
	uint32 a = (!::gxTierExempt && gxTierInit()) ? gxAram.alloc(size) : 0;
	if(a){
		gMainWhere = "tex-load";
		static uint8 stage[32*1024] __attribute__((aligned(32)));
		uint64 hash = gxHashInit();
		for(uint32 done = 0; done < size; ){
			uint32 chunk = size - done > sizeof(stage) ? (uint32)sizeof(stage) : size - done;
			stream->read8(stage, chunk);
			hash = gxHashBytes(hash, stage, chunk);
			DCFlushRange(stage, chunk);
			gxAramTransfer(ARQ_MRAMTOARAM, stage, a + done, chunk);
			done += chunk;
		}
		hash ^= (uint64)size << 40 ^ (uint64)gxFmt << 56 ^ (uint64)tw << 16 ^ th;
		GxShare *sh = gxShareFind(hash, size, (uint16)gxFmt);
		if(sh){   // resident twin: give this span back, share the texels
			gxAram.release(a);
			a = sh->aram; sh->refs++; gxShareBytes += size;
		}else{
			gxShareInsert(hash, a, size, (uint16)gxFmt);
			::gxAramBytes += size;
		}
		ext->aram = a;
		GX_InitTexObj(&ext->obj, nil, tw, th, gxFmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
		GX_InitTexObjLOD(&ext->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0,
		    GX_DISABLE, GX_DISABLE, GX_ANISO_1);
		ext->hasTex = 1;
		ext->dirty = 0;
		tex->raster = raster;
		return tex;
	}
#endif
	if(gxFmt == GX_TF_CI8){ gxNativeFail("ci8-mem1", tw, th); raster->destroy(); tex->destroy(); return nil; }   // B90: palettes bind only through the window
	ext->tiled = gxAllocTiled(size);
	if(ext->tiled == nil){
		gxNativeFail("alloc", size, 0);
		raster->destroy();
		tex->destroy();
		return nil;
	}
	// Straight into its final home. No staging, no Image, no conversion.
	stream->read8(ext->tiled, size);
	// Count it. This path used to set neither tiledSize nor gxTiledBytes, so
	// every ahead-of-time texture was invisible to the resident-texel figure
	// the HUD and the death screen report ("tex 62K" on a heap holding the
	// whole world's TXDs). Not charged to the streamer: streamed TXD loads are
	// already measured as a heap delta (gResidentCost), and TXDs loaded
	// outside streaming are the fixed set, which is exactly what this number
	// must expose.
	::gxTiledBytes += size;
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
	uint32 size = gxNativeSize(ext->gxFmt, tw, th);

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
	uint32 size = gxNativeSize(ext->gxFmt, raster->width, raster->height);
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
