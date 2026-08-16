#ifndef RWGX_H
#define RWGX_H

namespace rw {

#ifdef RW_GAMECUBE
// The console owns the display; GX picks the mode from the video encoder, so
// there is no window to hand over. Kept as a struct for interface parity.
struct EngineOpenParams
{
	int width, height;
	const char *windowtitle;
};
#endif

namespace gx {

extern Device renderdevice;

// GX takes screen-space 2D verts directly; w carries camera z for fog/sorting.
struct Im2DVertex
{
	float32 x, y, z, w;
	uint8   r, g, b, a;
	float32 u, v;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	void setCameraZ(float32 z) { this->w = z; }
	void setRecipCameraZ(float32 recipz) { this->w = 1.0f/recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u, float recipz) { (void)recipz; this->u = u; }
	void setV(float32 v, float recipz) { (void)recipz; this->v = v; }

	float getScreenX(void) { return this->x; }
	float getScreenY(void) { return this->y; }
	float getScreenZ(void) { return this->z; }
	float getCameraZ(void) { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

struct Im3DVertex
{
	V3d     position;
	uint8   r, g, b, a;
	float32 u, v;

	void setX(float32 x) { this->position.x = x; }
	void setY(float32 y) { this->position.y = y; }
	void setZ(float32 z) { this->position.z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	V3d getPosition(void) { return this->position; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

// per-geometry cache of recorded draw commands, one list per mesh
struct GxGeoExt
{
	void **lists;
	uint32 *sizes;
	int32 numLists;
};
extern int32 gxGeoOffset;
extern uint32 gxDlBytes;

bool32 gxRasterHasAlpha(Raster *raster);
// Debug probe: tiled GX format and the first tiled word. Separates "the
// texture is black in RAM" from "the texture is fine and the colour channel
// zeroes it" without a second boot.
void gxRasterProbe(Raster *raster, uint32 *gxFmt, uint32 *firstWord);
void registerPlatformPlugins(void);
ObjPipeline *makeDefaultPipeline(void);

// Raster backend
Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster *raster, int32 level, int32 lockMode);
void rasterUnlock(Raster *raster, int32 level);
uint8 *rasterLockPalette(Raster *raster, int32 lockMode);
void rasterUnlockPalette(Raster *raster);
int32 rasterNumLevels(Raster *raster);
bool32 imageFindRasterFormat(Image *img, int32 type,
	int32 *width, int32 *height, int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);

}
}

#endif
