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
	// Quantised attributes, and the float arrays they replaced are freed.
	// One block for both so a geometry costs one allocation, not two: the
	// arena here fragments badly enough that the combined attribute block in
	// Geometry::create is already a documented failure point.
	void *packBase;
	int16 *pos;               // 3 per vertex, stored value = float<<posShift
	int16 *uv;                // 2 per vertex, stored value = float<<uvShift
	uint8 posShift, uvShift;
	uint8 packed;             // GXPACK_* bits
	// Vertex colours, precomputed so the indexed path can exist at all.
	//
	// The immediate path computes clamp(prelight + ambient) * material per
	// vertex per frame, and because that result lives nowhere there is no
	// array to index — which is why GX_USE_INDEXED was switched off. But for
	// static world geometry every input except prelight is constant per mesh,
	// so the answer only changes when the ambient or the material does, i.e.
	// with the time of day. Cache it and all three attributes become arrays:
	// the CPU pushes indices and the GP DMAs the data itself.
	//
	// Costs 4 bytes a vertex against the 10 gxPackGeometry freed.
	RGBA  *colors;
	uint32 colorKey;          // ambient and material this was built for
	int32  colorCount;
};
enum { GXPACK_POS = 1, GXPACK_UV = 2, GXPACK_TRIED = 4 };
// Quantise a streamed geometry in place. Call once, after the DFF has loaded
// and before it is first drawn.
void gxPackGeometry(Geometry *geo);
extern uint32 gxPackSaved;    // running total of bytes reclaimed
extern uint32 gxPackGeoms, gxPackRefusedPos, gxPackRefusedUV;
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

// Native textures: a TXD converted ahead of time arrives already tiled, so
// reading one is a read into a correctly sized buffer and nothing else.
Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);

}
}

#endif
