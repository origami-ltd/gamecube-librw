#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "../rwrender.h"
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

namespace rw {
namespace gx {

// Native raster extension: the GX-tiled copy of the linear staging pixels.
struct GxRaster
{
	void *tiled;
	GXTexObj obj;
	bool32 hasTex;
	bool32 dirty;
};

int32 nativeRasterOffset;
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
	nativeRasterOffset = Raster::registerPlugin(sizeof(GxRaster),
	                                            ID_DRIVER,
	                                            createNativeRaster,
	                                            destroyNativeRaster,
	                                            copyNativeRaster);
}

// Convert the linear staging pixels to GX_TF_RGB5A3: 4x4 texel tiles of 32
// bytes, one big-endian uint16 per texel. 16-bit halves resident texture
// memory vs RGBA8 — the difference between fitting the arena and exit(1).
static void
tileRGB5A3(uint8 *dst, Raster *raster)
{
	int32 w = raster->width, h = raster->height;
	for(int32 ty = 0; ty < h; ty += 4)
	for(int32 tx = 0; tx < w; tx += 4){
		uint16 *out = (uint16*)dst;
		for(int32 y = 0; y < 4; y++)
		for(int32 x = 0; x < 4; x++){
			uint8 r = 0, g = 0, b = 0, a = 0;
			int32 sx = tx + x, sy = ty + y;
			if(sx < w && sy < h){
				uint8 *p = raster->pixels + sy*raster->stride;
				if(raster->depth == 32){
					p += sx*4;
					r = p[0]; g = p[1]; b = p[2]; a = p[3];
				}else{
					uint16 v = *(uint16*)(p + sx*2);
					r = ((v>>10)&0x1F)<<3;
					g = ((v>>5)&0x1F)<<3;
					b = (v&0x1F)<<3;
					a = (v&0x8000) ? 255 : 0;
				}
			}
			// >=0xE0 rounds to full 3-bit alpha anyway; use opaque 5:5:5.
			if(a >= 0xE0)
				*out++ = 0x8000 | ((r>>3)<<10) | ((g>>3)<<5) | (b>>3);
			else
				*out++ = ((a>>5)<<12) | ((r>>4)<<8) | ((g>>4)<<4) | (b>>4);
		}
		dst += 32;
	}
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

	// GX tiles are 4x4; round the allocation up.
	int32 tw = (raster->width + 3) & ~3;
	int32 th = (raster->height + 3) & ~3;
	int32 size = tw*th*2;
	if(ext->tiled == nil){
		ext->tiled = memalign(32, size);
		if(ext->tiled == nil)
			return nil;
	}
	tileRGB5A3((uint8*)ext->tiled, raster);
	DCFlushRange(ext->tiled, size);

	GX_InitTexObj(&ext->obj, ext->tiled, raster->width, raster->height,
	    GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&ext->obj, GX_LINEAR, GX_LINEAR, 0, 0, 0,
	    GX_DISABLE, GX_DISABLE, GX_ANISO_1);
	// no GX_InvalidateTexAll here: textures build at stream time with no
	// frame draining the FIFO; beginUpdate invalidates once per frame

	ext->hasTex = 1;
	ext->dirty = 0;

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
		raster->pixels = (uint8*)rwNew(size, MEMDUR_EVENT | ID_DRIVER);
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
		raster->pixels = (uint8*)rwNew(raster->stride*raster->height,
		    MEMDUR_EVENT | ID_DRIVER);
		if(raster->pixels)
			memset(raster->pixels, 0, raster->stride*raster->height);
		raster->originalPixels = raster->pixels;
	}
	raster->privateFlags |= Raster::PRIVATELOCK_WRITE;
	return raster->pixels;
}

void
rasterUnlock(Raster *raster, int32)
{
	raster->privateFlags &= ~Raster::PRIVATELOCK_WRITE;
	GETGXRASTEREXT(raster)->dirty = 1;
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

	Image *truecolor = nil;
	if(image->depth <= 8){
		truecolor = Image::create(image->width, image->height, 32);
		truecolor->allocate();
		image->unpalettize(image->hasAlpha());
		image = image;
	}

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

	if(truecolor)
		truecolor->destroy();
	return 1;
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
