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
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwgx.h"

#define PLUGIN_ID ID_DRIVER

#ifdef RW_GAMECUBE

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

#include <ogc/lwp_watchdog.h>

extern "C" void __console_init(void *fb, int xstart, int ystart, int xres,
    int yres, int stride);

void GeckoLog(const char*); // game-side USB Gecko logger (gamecube.cpp)
extern unsigned gxCopyUs;   // defined below at global scope
extern unsigned gxShowUs;   // whole showRaster duration
// What the VI actually came up in, latched at startGX and reported by the
// heartbeat — see startGX for why it cannot just be printed there.
unsigned gxViTVMode, gxHaveComponent, gxXfbHeight, gxEfbHeight;
// The boot console's framebuffer, handed down by the game so startGX can adopt
// it rather than allocating a third one. See startGX.
void *gxAdoptXfb;
unsigned gxAdoptXfbSize;
// Second GX-owned XFB, exported for the boot FMV's present queue.
void *gxMovieXfb;
// Whether showRaster waits for the retrace. The game pushes m_PrefsVsync down
// into this; defaults on so a build that never sets it behaves as before.
unsigned gxWaitRetrace = 1;
// Camera raster size latched at present time — the real render target.
unsigned gxCamW, gxCamH;
// Which submitter last handed work to the GP. Read by the freeze watchdog in
// the game skel, which cannot see librw's statics.
const char *gxLastPath = "-";
// per-frame draw counters, read by the HUD overlay
unsigned gxMeshCount, gxVertCount, gxDlMeshCount;
unsigned gxSimUs, gxRenderUs, gxStreamUs;
unsigned gxGpUs, gxVsyncUs, gxHudUs, gxTexBuilds, gxIdleUs, gxAudioUs, gxFxUs, gxListUs, gxPreUs, gxTileUs, gxPostUs, gxSkyUs, gxTailUs, gxLightsUs, gxFrameUs, gxFadeUs, gxAfterUs, gxEndUs, gxCopyUs, gxShowUs; // GP drain vs retrace wait, split // frame split, filled by the game loop

namespace rw {
namespace gx {

#define FIFO_SIZE (256*1024)
#define GX_WIREFRAME 0 // debug: draw world geometry as edges, untextured
#define GX_SKIN_LOCALSPACE 0 // A/B: treat hier matrices as already atomic-relative
#define GX_MARK_SKINNED 0 // debug: skinned atomics drawn flat red
#define GX_NO_SKIN 0 // A/B: 1 = draw bind pose, skip CPU skinning
// Display-list caching is OFF: profiling put the entire render pipeline at
// ~2ms/frame, so it optimises something that was never the bottleneck, and
// the cache costs 2MB out of a 15MB arena — which is what pushed streaming
// back into mustmalloc/exit(1). Raise this only on hardware, where freeing
// the CPU actually matters and memory has been re-budgeted for it.
#define GX_FORCE_PROGRESSIVE 1 // 480p even when the cable check says no; see startGX
#define GX_DL_BUDGET 0
// ON, with the measurement that flipped it: the same night alley read
// 29fps/work29/oth18.7 direct and 59fps/work11/oth1.8 indexed — the "oth"
// residual was CPU vertex submission the rnd timer never covered, and the
// GP's DMA fetch eats it. Full-run: 123 samples at 16.6ms vs 22 at 33.3ms.
#define GX_USE_INDEXED 1 // 1 = GP DMA vertex arrays
// debug: dump every input to the skinned (ped) draw over USB Gecko, bounded
// so it cannot flood the FIFO or the frame. Peds are the only geometry on
// the GX hardware-lighting path, so this is where "black characters" lives.
// 1 = dump every input to the skinned (ped) draw to dvd:/skin.log, one sample
// per second so the log spans the intro and gameplay. Read it back without
// mounting: LC_ALL=C grep -a -o "SKIN [^|]*" WiiSD.raw
// debug: draw every mesh with its texture coordinate as colour (r=u, g=v),
// untextured. Separates "the GP got the wrong UVs" from "the GP sampled the
// texture wrongly" in one boot.
#define GX_UV_DEBUG 0
#define GX_PROBE_SKIN 0
#if GX_PROBE_SKIN
static int32 gxProbeLeft = 300; // every mesh of one atomic, ~1 atomic/second
#endif

extern bool32 gxTexCacheDirty; // set by gxraster when a texture is (re)built
// Tracked render state, applied by the 3D paths before drawing.
static uint32 stZTest = 1, stZWrite = 1;
static uint32 stVertexAlpha = 0;
// Whether the bound texture carries an alpha channel. librw's other backends
// blend when vertexAlpha OR textureAlpha is set (gl3device.cpp setRenderState
// VERTEXALPHA / TEXTURERASTER, and d3ddevice.cpp does the same); this backend
// only ever looked at vertexAlpha, so anything whose transparency comes from
// its texture rather than from vertex or material alpha drew fully opaque.
// That is the missing transparency on the pink mission markers and the missing
// water spray from a hydrant — both are alpha-textured, and neither sets
// rwRENDERSTATEVERTEXALPHAENABLE because on every other platform it does not
// have to.
static uint32 stTextureAlpha = 0;
// Whether the bound MATERIAL carries alpha. The sibling of stTextureAlpha, and
// the half that was still missing: gl3render, gl3skin, gl3matfx, d3d9render,
// d3d9skin and d3d9matfx all do
//     SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 255)
// once per mesh, and this backend has no equivalent line anywhere — grep
// VERTEXALPHA here and the only hits are the setRenderState handler and its
// getter. So an atomic whose transparency comes from RpMaterialSetColor drew
// fully opaque, and because stVertexAlpha is a sticky global it would blend
// correctly only when some unrelated draw happened to leave it set. That is
// the 3d marker (models/generic/zonecylb.dff, C3dMarker::Render) losing its
// glow depending on what else is on screen.
static uint32 stMaterialAlpha = 0;
static uint32 stSrcBlend = BLENDSRCALPHA, stDstBlend = BLENDINVSRCALPHA;
static uint32 stAlphaFunc = ALPHAGREATEREQUAL, stAlphaRef = 10;
static uint32 stCullMode = CULLNONE;
// Stencil, emulated with EFB destination alpha (GX has none of its own).
// Two shapes cover everything the game asks for (MBlur.cpp):
//   ALWAYS + REPLACE  -> the draw also writes its alpha into the EFB: the mask.
//   EQUAL  + KEEP     -> the draw blends src*dstAlpha + dst*(1-dstAlpha):
//                        confined to where the mask wrote.
// Everything else (ref/mask/writemask, zfail) is accepted and ignored — the
// game only ever uses ref=1 with full masks.
// ---- extra additive TEV stage: matfx env-maps and ped rim light ----------
//
// One mechanism serves both: an armed GXTexObj sampled through a TG_NRM
// texgen (spheremap UVs from the vertex normal via GX_TEXMTX0) and added to
// PREV scaled by a konst colour. For vehicles the texture is the material's
// matfx environment map; for peds it is a generated radial ramp whose value
// rises toward edge-on normals — which IS a rim light, computed by the TEV.
// The vertex-normal plumbing (descriptor + both emission paths + the bound
// array) already existed behind hwLights; the stage just turns it back on for
// armed meshes.
bool32 gxRimEnable;          // <- CustomPipes::RimlightEnable, copied by the skel
// Menu resolution pref, written through an int8 by the frontend (the CFO
// (int8*) rule) and applied once in startGX. 0=480p, 1=528p, 2=720p label.
int8 gxEfbResPref;
// The one reader for the boot-time pref: the skel (RsGlobal sizing) and
// startGX (EFB growth) both call it after the fs mounts. Absent = 480p.
int8
gxReadEfbPref(void)
{
	FILE *prefF = fopen("dvd:/reVC.INI", "r");
	if(prefF){
		char prefLn[128];
		while(fgets(prefLn, sizeof(prefLn), prefF))
			if(strncmp(prefLn, "EfbHeight", 9) == 0 && strchr(prefLn, '='))
				gxEfbResPref = (int8)atoi(strchr(prefLn, '=') + 1);
		fclose(prefF);
	}
	return gxEfbResPref;
}
uint32 gxCopyFilterLevel;    // <- m_nPrefsMSAALevel: 0 sharp, >0 smoothing filter
// Road gloss and world lightmaps, same bridge pattern. The lookup is a
// function pointer installed by the game (re3.cpp) because the gloss table
// lives in CustomPipes; librw stays linkable without it.
bool32 gxGlossEnable;
float gxGlossMult = 1.0f;
bool32 gxLightmapEnable;
float gxLightmapBlend;       // WorldLightmapBlend.Get()*LightmapMult, per frame
Texture *(*gxGlossLookup)(Material*);
static GXTexObj *gxEnvTex;   // armed per mesh, nil = stage off
static bool32 gxEnvUV2;      // stage samples UV set 1 instead of the normal
static GXColor gxEnvK = {255,255,255,255};
static GXTexObj gxRimObj;
static bool32 gxRimReady;

// 64x64 I8 radial ramp, bright at the rim. I8 tiles are 8x4 texels.
static GXTexObj*
gxRimRamp(void)
{
	if(gxRimReady)
		return &gxRimObj;
	enum { W = 64, H = 64 };
	static u8 *buf;
	if(buf == nil)
		buf = (u8*)memalign(32, W*H);
	if(buf == nil)
		return nil;
	u8 *d = buf;
	for(int ty = 0; ty < H; ty += 4)
	for(int tx = 0; tx < W; tx += 8)
	for(int y = ty; y < ty+4; y++)
	for(int x = tx; x < tx+8; x++){
		// spheremap uv: a screen-facing normal lands at the centre,
		// edge-on at radius 0.5 — distance from centre IS facing ratio.
		float u = (x + 0.5f)/W - 0.5f, v = (y + 0.5f)/H - 0.5f;
		float r = sqrtf(u*u + v*v)*2.0f;      // 0 centre .. 1 rim
		float k = (r - 0.55f)/0.45f;
		if(k < 0.0f) k = 0.0f;
		if(k > 1.0f) k = 1.0f;
		*d++ = (u8)(k*k*255.0f);
	}
	DCFlushRange(buf, W*H);
	GX_InitTexObj(&gxRimObj, buf, W, H, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	gxRimReady = 1;
	gxTexCacheDirty = 1;
	return &gxRimObj;
}

static uint32 stStencilEnable = 0;
static uint32 stStencilFunc = 0;    // rw::StencilFunc, 0 = unset
static uint32 stStencilPass = STENCILKEEP;

static void gx3DMemoInvalidate(void); // 2D draws stomp the 3D GP state
static void gx2DMemoInvalidate(void);
// tracked-render-state appliers (defined with the state block below)
static void applyBlend(void);
static void applyCull(void);
static void applyZMode(void);
static void applyAlphaTest(void);

static void *gpFifo;
static void *xfb[2];
static int currentXfb;
static GXRModeObj *rmode;
static bool32 gxStarted;
static GXColor clearColor = { 0, 0, 0, 255 };

static void
startGX(void)
{
	if(gxStarted)
		return;

	VIDEO_Init();
	rmode = VIDEO_GetPreferredMode(nil);

	// 480p. VIDEO_GetPreferredMode hands back a progressive mode only when the
	// console's own setting already asks for one, and for a port whose whole
	// point is 60fps that setting is not optional: interlaced scans out 240
	// lines per field, so half the image on screen is always one field stale.
	// PAL50 is left alone — 576p is a different resolution, not a flag.
	//
	// GX_FORCE_PROGRESSIVE exists because VIDEO_HaveComponentCable() alone is
	// not a usable gate here. Measured: Dolphin reports cbl=0 regardless of its
	// progressive-scan setting (the SYSCONF byte is rewritten back to 0 on
	// launch, and the ProgressiveScan override is a per-game INI key that a
	// loose .dol has no game ID to match), so on the emulator the cable check
	// can never pass and the port can never be seen in 480p.
	//
	// Forcing is safe there — an emulator has no composite cable to be
	// incompatible with — and on real hardware component cables and the GCVideo
	// / GCHD digital adapters all set the DTV bit, so the forced path matches
	// what the check would have allowed anyway. Set this to 0 for a console
	// wired over composite, where progressive output is no picture at all.
	if(GX_FORCE_PROGRESSIVE || VIDEO_HaveComponentCable())
		switch(rmode->viTVMode >> 2){
		case VI_NTSC:    rmode = &TVNtsc480Prog; break;
		case VI_EURGB60: rmode = &TVEurgb60Hz480Prog; break;
		}

	// RESOLUTION pref (menu Graphics row, persisted in dvd:/reVC.INI):
	// 0 = 480p EFB. 1 and 2 = 640x528 EFB — the hardware maximum for the
	// RGBA6_Z24 format — downsampled to the fixed 480-line output by the
	// display copy's YScale below, through the copy filter: real vertical
	// supersampling on the console's own wire. Value 2 is labelled "720p
	// (Dolphin IR)": the GP cannot raster more than 528 lines, so anything
	// higher is Dolphin's internal-resolution scaler, not this code — the
	// EFB still runs at its 528 maximum so Dolphin has the best base.
	// Read here, not from the settings loader: the video mode is committed
	// long before LoadSettings runs, and the fs is already mounted (the
	// skeleton mounts before RW starts). Absent file or key = 480p.
	//
	// MEASURED DEAD END, do not re-try naively: growing efbHeight to 528 and
	// letting the existing DispCopyYScale (480/528 = 0.909) "downsample"
	// produced a solid magenta frame — GX_SetDispCopyYScale only scales UP
	// (it exists for interlace line-doubling); the EFB->XFB copy cannot
	// vertically downsample. Until a real path exists (PAL 576 modes, or a
	// texture-copy resample pass), the 528/720 menu entries change nothing.
	gxReadEfbPref();

	// Reported by the heartbeat rather than printed here: this runs before the
	// SD is mounted and before Dolphin's Gecko listener attaches, so a printf
	// at this point goes nowhere and reads as "no output" instead of "not seen".
	// viTVMode&3: 0 interlaced, 1 double-strike, 2 progressive.
	::gxViTVMode = rmode->viTVMode;
	::gxHaveComponent = VIDEO_HaveComponentCable();
	::gxXfbHeight = rmode->xfbHeight;
	::gxEfbHeight = rmode->efbHeight;

	// Reuse the boot console XFB when the skeleton offers it: linking Theora
	// leaves the cold-boot texture heap too tight for FONTS.TXD if this 614KB
	// block is allocated a third time. The earlier attempt showed console
	// leftovers as red blocks through the picture; clearing every pixel at
	// hand-over fixes that. Both buffers are cleared — SYS_AllocateFramebuffer
	// memory is uninitialised, and 0xFF patterns read as bright pink in YUY2
	// on the first flips before an EFB copy lands.
	unsigned xfbSize = VIDEO_GetFrameBufferSize(rmode);
	if(::gxAdoptXfb && ::gxAdoptXfbSize >= xfbSize){
		xfb[0] = ::gxAdoptXfb;
		VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
	}else
		xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
	// Second GX-owned XFB, exposed only so the boot FMV can queue one frame
	// ahead without allocating a third 614KB buffer.
	::gxMovieXfb = xfb[1];
	currentXfb = 0;

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb[currentXfb]);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();

	gpFifo = memalign(32, FIFO_SIZE);
	memset(gpFifo, 0, FIFO_SIZE);
	GX_Init(gpFifo, FIFO_SIZE);

	GX_SetCopyClear(clearColor, GX_MAX_Z24);
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_SetDispCopyYScale((f32)rmode->xfbHeight/(f32)rmode->efbHeight);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
	// Deflicker filter off: it costs EFB->XFB copy bandwidth every frame
	// and mostly matters on interlaced CRTs. Progressive output looks
	// sharper without it.
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_FALSE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,
	    ((rmode->viHeight == 2*rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

	// RGBA6 keeps a destination-alpha channel in the EFB — VC masks the
	// round radar (and other stencil-ish effects) with dest-alpha blends.
	// Cost: 6-bit color components (hardware dithers), same Z24.
	// RGB8_Z24: full colour and 24-bit Z. Halving the EFB to RGB565_Z16
	// was measured and changed nothing (the EFB->XFB copy is not where the
	// frame goes), so there is no reason to pay for it in Z precision.
	// The radar's round mask is a Z trick, so no destination alpha needed.
	// RGBA6, not RGB8: the two bits per channel buy a destination-alpha
	// channel, which is this hardware's stencil. MBlur's screen effects
	// (heat haze, water drops on the lens) mask their refraction pass with
	// STENCILEQUAL; with no stencil on GX those either draw unmasked over
	// the whole rectangle or not at all. Dest-alpha blending
	// (GX_BL_DSTALPHA) gives the same masking, and RGBA6 is what most
	// retail GC titles shipped in for exactly this reason.
	GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
	// EFB alpha must start each frame at 0 and stay 0 outside mask writes —
	// see the STENCIL emulation in setRenderState.
	GX_SetAlphaUpdate(GX_FALSE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_CopyDisp(xfb[currentXfb], GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	GX_ClearVtxDesc();
	GX_InvVtxCache();
	GX_InvalidateTexAll();

	gxStarted = TRUE;
}

static void
stopGX(void)
{
	if(!gxStarted)
		return;
	GX_AbortFrame();
	GX_Flush();
	gxStarted = FALSE;
}

// Vertex setup shared by the 2D paths: position, colour, one texcoord.
// Same memo as the 3D path. The 2D path is called once per sprite —
// coronas, particles, HUD glyphs, radar blips — and each call rewrote the
// whole vertex descriptor, TEV, channel and blend state plus a projection
// matrix load. Hundreds of those per frame is real time.
static bool32 gx2DProjLoaded;
static bool32 gx2DMemoDirty = 1;
static bool32 m2Tex, m2VA, m2Src, m2Dst, m2ZT, m2ZW, m2AF, m2AR;

// One-shot TEV override for the next textured im2D draws:
//   out = tex*mult + add   (per channel)
// This is the contrast curve of the MOBILE colour filter, which is a pixel
// shader everywhere else. Armed by CPostFX through setIm2DConstMulAdd, cleared
// by clearIm2DOverride; while armed the textured 2D path swaps its MODULATE
// stage for a two-stage constant multiply-add:
//   stage0: PREV = TEXC * K0            (K0 = mult/2, scale x2 -> mult<=2)
//   stage1: PREV = PREV + K1 - 0.5      (K1 = add+0.5, so add may be negative)
static bool32 gxIm2DMulAdd;
static GXColor gxIm2DK0, gxIm2DK1;

static u8
gxQuant255(float v)
{
	int i = (int)(v*255.0f + 0.5f);
	return (u8)(i < 0 ? 0 : i > 255 ? 255 : i);
}

void
setIm2DConstMulAdd(const float *mult, const float *add)
{
	gxIm2DK0.r = gxQuant255(mult[0]*0.5f);
	gxIm2DK0.g = gxQuant255(mult[1]*0.5f);
	gxIm2DK0.b = gxQuant255(mult[2]*0.5f);
	gxIm2DK0.a = 255;
	gxIm2DK1.r = gxQuant255(add[0]+0.5f);
	gxIm2DK1.g = gxQuant255(add[1]+0.5f);
	gxIm2DK1.b = gxQuant255(add[2]+0.5f);
	gxIm2DK1.a = 255;
	gxIm2DMulAdd = 1;
	gx2DMemoInvalidate();
}

void
clearIm2DOverride(void)
{
	gxIm2DMulAdd = 0;
	gx2DMemoInvalidate();
}

static void
setupIm2DVtxDesc(bool32 textured)
{
	gx3DMemoInvalidate();
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	if(textured){
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
		GX_SetNumTexGens(1);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
		if(gxIm2DMulAdd){
			GX_SetTevKColor(GX_KCOLOR0, gxIm2DK0);
			GX_SetTevKColor(GX_KCOLOR1, gxIm2DK1);
			// stage0: lerp(0, tex, K0) x2 = tex*mult
			GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
			GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC,
			    GX_CC_KONST, GX_CC_ZERO);
			GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO,
			    GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
			GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO,
			    GX_CA_ZERO, GX_CA_TEXA);
			GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO,
			    GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
			// stage1: PREV + K1 - 0.5 = PREV + add
			GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);
			GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL,
			    GX_COLOR0A0);
			GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_KONST,
			    GX_CC_ONE, GX_CC_CPREV);
			GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_SUBHALF,
			    GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
			GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO,
			    GX_CA_ZERO, GX_CA_APREV);
			GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO,
			    GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
		}else{
			GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
			GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
		}
	}else{
		GX_SetNumTexGens(0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
		GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL,
		    GX_COLOR0A0);
	}
	// the 3D path runs up to 3 TEV stages and reorders them — a stale
	// stage count turns every full-screen 2D overlay white (the flashbang).
	// Two stages while the constant mult-add override is armed, one otherwise
	// — and no early return above, so the blend/Z/alpha appliers at the end
	// of this function always run.
	GX_SetNumTevStages(textured && gxIm2DMulAdd ? 2 : 1);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
	    GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	// Blend/alpha follow the tracked state (hardcoding srcalpha here made
	// every additive overlay — coronas, streetlight pools — draw as opaque
	// black boxes). Z stays force-disabled: the sky and other full-screen
	// 2D passes inherit whatever Z state the last 3D draw left, and a sky
	// quad that writes depth beats every object drawn after it (world
	// occlusion inverts — sky punches through walls and peds).
	// ponytail: costs the round radar mask (draws square); revisit with a
	// per-draw Z override once the 2D passes are audited.
	stMaterialAlpha = 0;   // no material in the 2D path; do not inherit one
	applyBlend();
	applyZMode();
	applyAlphaTest();
}

static void gx2DMemoInvalidate(void) { gx2DMemoDirty = 1; }

// Set an orthographic projection matching the 2D screen space librw hands us.
// The ortho projection never changes, so load it once and only reload after
// a 3D draw has swapped in the camera frustum.
static void
setIm2DMatrices(void)
{
	Mtx44 proj;
	Mtx mv;

	// n=0,f=1 with negated vertex z: RW im2D z (0=near, 1=far) maps
	// monotonically onto GX depth 0..1 — required for the radar z-mask
	// and depth-tested sprites (coronas).
	guOrtho(proj, 0, rmode->efbHeight, 0, rmode->fbWidth, 0.0f, 1.0f);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	guMtxIdentity(mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
}

// Camera matrices in GX form, rebuilt each beginUpdate. The im2D path loads
// its own ortho projection, so every 3D draw reloads gxProj first.
static Mtx gxView;
static Mtx44 gxProj;
static u8 gxProjType = GX_PERSPECTIVE;
static bool32 gxHaveCamera;

// rw::Matrix rows (row-vector convention) become columns for GX (column
// vectors); the view additionally negates Z so the camera looks down -Z as
// GX's projection expects (RW looks down +Z).
static void
rwToGxMtx(Mtx out, Matrix *m)
{
	out[0][0] = m->right.x; out[0][1] = m->up.x; out[0][2] = m->at.x; out[0][3] = m->pos.x;
	out[1][0] = m->right.y; out[1][1] = m->up.y; out[1][2] = m->at.y; out[1][3] = m->pos.y;
	out[2][0] = m->right.z; out[2][1] = m->up.z; out[2][2] = m->at.z; out[2][3] = m->pos.z;
}

static void
loadWorldMtx(Matrix *world, bool32 withNormals = 0)
{
	Mtx w, mv;
	if(world){
		rwToGxMtx(w, world);
		guMtxConcat(gxView, w, mv);
	}else
		guMtxCopy(gxView, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	if(withNormals){
		Mtx nrm;
		guMtxInvXpose(mv, nrm);
		GX_LoadNrmMtxImm(nrm, GX_PNMTX0);
	}
	// Spheremap matrix for the env/rim texgen: model-space normal through the
	// modelview rotation, scaled and biased into 0..1 UVs. Eight floats per
	// draw, loaded unconditionally so an armed mesh can never miss it.
	{
		Mtx tm;
		for(int j = 0; j < 4; j++){
			tm[0][j] = 0.5f*mv[0][j];
			tm[1][j] = -0.5f*mv[1][j];
			tm[2][j] = 0.0f;
		}
		tm[0][3] = 0.5f;
		tm[1][3] = 0.5f;
		GX_LoadTexMtxImm(tm, GX_TEXMTX0, GX_MTX2x4);
	}
	GX_SetCurrentMtx(GX_PNMTX0);
}

// True while the game is rendering into a camera whose target is a texture
// rather than the screen (CShadowCamera). The GX backend has no
// render-to-texture yet — there is only the EFB — so such a pass would
// otherwise land straight in the scene framebuffer. That is exactly what
// broke the characters: CShadowCamera::Update strips PRELIT/LIGHT/TEXTURED/
// NORMALS from the ped geometry to draw a silhouette, and clears its target
// to white first. With no render target of our own, that silhouette was
// painted over the world (black, untextured, unlit characters and "the ped
// reads as a wall") and the white clear became the frame's copy-clear colour
// (the white world). Skip the pass until GX_CopyTex lands.
static bool32 gxOffscreenPass;
static bool32 gxHaveCameraSaved;

static bool32
targetsTexture(Camera *cam)
{
	Raster *fb = cam ? cam->frameBuffer : nil;
	return fb && (fb->type & 0xF) == Raster::CAMERATEXTURE;
}

static void
beginUpdate(Camera *cam)
{
	startGX();
	gxOffscreenPass = targetsTexture(cam);
	if(gxOffscreenPass){
		// nothing may draw: the 3D paths all gate on gxHaveCamera. Saved and
		// restored rather than just cleared, so an offscreen pass that ever
		// happens inside the main one cannot silently kill the rest of the
		// scene. (Today they run in PreRender, before the main beginUpdate.)
		gxHaveCameraSaved = gxHaveCamera;
		gxHaveCamera = FALSE;
		return;
	}
	// Texture-cache invalidation happens HERE, at frame start, and nowhere
	// else. The draw-site version ("invalidate exactly when a texture was
	// rebuilt") ran in the MIDDLE of the frame — and on Dolphin an
	// InvalidateTexAll also drops the virtualised EFB copies, so everything
	// sampling the captured frame after that point (the colour-filter
	// overlay, the screen droplets) read black for the rest of that frame.
	// With rain on and textures streaming in, that was most frames: the
	// whole world strobing dark, dark "cut" drops — one cause, reported
	// three ways. A texture rebuilt mid-frame now draws through possibly
	// stale TMEM until next frame; one frame of stale texels on a brand-new
	// texture is invisible, a dropped EFB copy is not.
	if(gxTexCacheDirty){ GX_InvalidateTexAll(); gxTexCacheDirty = 0; }
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);

	// View: inverse of the camera frame, transposed into GX layout.
	// X negated: RW camera space is X-mirrored vs GL/GX (librw's own
	// camera sync negates the X column) — without it the whole world
	// renders mirrored and stick left/right feels swapped.
	// Z negated: GX projections look down -Z, RW down +Z.
	// Two negations = a proper rotation, so no net mirror.
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	gxView[0][0] = -inv.right.x; gxView[0][1] = -inv.up.x; gxView[0][2] = -inv.at.x; gxView[0][3] = -inv.pos.x;
	gxView[1][0] = inv.right.y; gxView[1][1] = inv.up.y; gxView[1][2] = inv.at.y; gxView[1][3] = inv.pos.y;
	gxView[2][0] = -inv.right.z; gxView[2][1] = -inv.up.z; gxView[2][2] = -inv.at.z; gxView[2][3] = -inv.pos.z;

	if(cam->projection == Camera::PERSPECTIVE){
		guFrustum(gxProj,
		    cam->viewWindow.y*cam->nearPlane, -cam->viewWindow.y*cam->nearPlane,
		    -cam->viewWindow.x*cam->nearPlane, cam->viewWindow.x*cam->nearPlane,
		    cam->nearPlane, cam->farPlane);
		gxProjType = GX_PERSPECTIVE;
	}else{
		guOrtho(gxProj,
		    cam->viewWindow.y, -cam->viewWindow.y,
		    -cam->viewWindow.x, cam->viewWindow.x,
		    cam->nearPlane, cam->farPlane);
		gxProjType = GX_ORTHOGRAPHIC;
	}
	GX_LoadProjectionMtx(gxProj, gxProjType);
	gxHaveCamera = TRUE;
}

static void
endUpdate(Camera*)
{
	if(gxOffscreenPass){
		gxHaveCamera = gxHaveCameraSaved;
		gxOffscreenPass = 0;
	}
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	startGX();
	// An offscreen camera's clear must not touch the screen. GX_SetCopyClear
	// is global state, so CShadowCamera's white background was becoming the
	// whole frame's clear colour.
	if(targetsTexture(cam))
		return;
	if(mode & Camera::CLEARIMAGE){
		clearColor.r = col->red;
		clearColor.g = col->green;
		clearColor.b = col->blue;
		// Alpha 0 regardless of the requested clear: EFB alpha is the
		// stencil substitute, and a frame that starts with it at 255 makes
		// every STENCILEQUAL-masked pass cover its whole rectangle.
		clearColor.a = 0;
		GX_SetCopyClear(clearColor, GX_MAX_Z24);
	}
}

// Last state handed to the GP, so a stall can name what it choked on rather
// than leaving a frozen screen and nothing else. Updated once per draw call,
// which is a handful of stores — not per vertex.
struct GxLastDraw {
	const char *path;
	const char *texName;
	Raster *texRaster;
	uint32 count;
	uint16 texW, texH;
	uint8 hasTex, prim, vtxfmt, posFrac, uvFrac;
};
static GxLastDraw gxLast;

static void
gxReportGpStall(void)
{
	// Bounded: a stall normally repeats on every frame after the first, and a
	// log that fills the SD helps nobody.
	static int left = 8;
	if(left-- <= 0)
		return;
	char line[256];
	snprintf(line, sizeof(line),
	    "GPSTALL path=%s tex=%s raster=%p %ux%u has=%d prim=%d fmt=%d "
	    "count=%u posFrac=%d uvFrac=%d",
	    gxLast.path ? gxLast.path : "-",
	    gxLast.texName ? gxLast.texName : "-",
	    (void*)gxLast.texRaster, (unsigned)gxLast.texW, (unsigned)gxLast.texH,
	    (int)gxLast.hasTex, (int)gxLast.prim, (int)gxLast.vtxfmt,
	    (unsigned)gxLast.count, (int)gxLast.posFrac, (int)gxLast.uvFrac);
	// Gecko truncates past a couple of dozen characters, so the transport
	// that carries the detail is the SD; the Gecko line is only the flag that
	// tells you to go read it.
	GeckoLog("GPSTALL");
	DVD_FS_GUARD;
	FILE *f = fopen("dvd:/gpstall.log", "a");
	if(f){
		fprintf(f, "%s\n", line);
		fclose(f);
	}
	// Best effort: drop whatever the GP is chewing on so the next frame has a
	// chance. If it stalls again the log says so, which is still progress on a
	// freeze that previously produced no evidence at all.
	GX_AbortFrame();
}

static void
showRaster(Raster *raster, uint32 flags)
{
	// Whole-call timer. Its internals (copy, DrawDone, retrace) already
	// measure ~6ms while DoRWStuffEndOfFrame measures ~23ms; this says
	// whether the missing 16ms is inside showRaster (in the untimed
	// GX_Flush / VIDEO_Flush statements) or before it.
	extern unsigned gxShowUs;
	unsigned long long tShow = gettime();
	struct ShowTimer {
		unsigned long long t;
		~ShowTimer(){ ::gxShowUs = (unsigned)ticks_to_microsecs(gettime() - t); }
	} showTimer = { tShow };
	// The camera raster IS the render target, so its size is the answer to
	// "what resolution is the game actually drawing" — measured, rather than
	// inferred from the video mode, which is 640x480 and says nothing about
	// what the game asked the camera for.
	if(raster){
		::gxCamW = raster->width;
		::gxCamH = raster->height;
	}
	startGX();

	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	// Pick a buffer the VI is not scanning out right now. Normally that is
	// simply the other one; when our previous flip has not been picked up yet
	// it is the one we just drew, so we overwrite a frame nobody ever saw
	// rather than stalling. That is a triple buffer's behaviour without paying
	// for a third 614KB XFB.
	//
	// This is what lets the retrace stall go. Presentation stays tear-free
	// either way — VIDEO_Flush hands the buffer to the VI, which only ever
	// switches at a retrace — so VIDEO_WaitVSync was never what made the image
	// clean, it only paced the CPU. And pacing the CPU is precisely what
	// quantised the frame rate: a 17ms frame missed one retrace and presented
	// at the next, so the game could only ever read 60 or 30 and nothing
	// between. Without the stall the rate floats and the frame limiter caps it.
	// ANTI ALIASING: vertical smoothing in the copy pipeline, on or off.
	GX_SetCopyFilter(GX_FALSE, nil, gxCopyFilterLevel > 0 ? GX_TRUE : GX_FALSE,
	    rmode->vfilter);
	{
		void *shown = VIDEO_GetCurrentFramebuffer();
		int next = currentXfb ^ 1;
		currentXfb = (xfb[next] == shown) ? currentXfb : next;
	}
	{
		// GX_CopyDisp only queues commands, so it should be ~free. If it
		// is not, the CPU is blocking on FIFO space — i.e. waiting for the
		// GP to drain, which would mean the GP is the real bottleneck and
		// the earlier GX_DrawDone reading of 0ms was just measuring an
		// already-empty queue.
		unsigned long long t0 = gettime();
		GX_CopyDisp(xfb[currentXfb], GX_TRUE);
		::gxCopyUs = (unsigned)ticks_to_microsecs(gettime() - t0);
	}
	// DrawDone AFTER queuing the copy (the libogc order). Calling it
	// first made the CPU wait for the GP to drain, then queue the copy,
	// then wait again — a whole extra round trip per frame.
	//
	// Bounded, though, because GX_DrawDone() waits on the GP with no way out
	// and a stalled GP is a real failure mode here: a vertex descriptor that
	// disagrees with the data pushed, or a TEV stage sampling a texmap that
	// was never loaded, both park the GP forever. From outside it is
	// indistinguishable from a hang anywhere else in the game — frozen image,
	// no exception, no crash.log, and near-zero CPU because the thread is
	// blocked rather than spinning. That is exactly how the pause-menu freeze
	// presented, and with an unbounded wait there is nothing to read
	// afterwards. A draw-sync token gives the same ordering guarantee and can
	// be polled against a deadline, so the GP tells on itself instead.
	{
		unsigned long long t0 = gettime();
		static u16 syncToken;
		if(++syncToken == 0)
			syncToken = 1;
		GX_SetDrawSync(syncToken);
		GX_Flush();
		// Far beyond any real frame: the heaviest measured GP time is single
		// -digit milliseconds, so this only fires on a genuine stall.
		unsigned long long deadline = t0 + millisecs_to_ticks(500);
		while(GX_GetDrawSync() != syncToken)
			if(gettime() > deadline){
				gxReportGpStall();
				break;
			}
		gxGpUs = (unsigned)ticks_to_microsecs(gettime() - t0);
	}
	GX_Flush();

	VIDEO_SetNextFramebuffer(xfb[currentXfb]);
	VIDEO_Flush();
	unsigned long long tv = gettime();
	// Single retrace. The two-retrace lock was written on the assumption
	// that a frame costs more than 33ms; instrumenting the loop showed
	// simulation at ~10ms and scene submission at ~1ms, so the lock was
	// the frame rate: any frame a hair over 33ms fell to the next pair
	// and presented at 66ms, which is the 17-20fps beat we measured.
	//
	// Optional now, driven by m_PrefsVsync through gxWaitRetrace (the game
	// pushes it down; librw has no business knowing about the menu manager).
	// Waiting here quantises the frame to a whole retrace, so a 20ms frame
	// presents at 33.3ms and reads as 30fps — true to what a TV shows, but it
	// hides how far over budget the frame actually is. With the wait off and
	// the skel's frame limiter pacing instead, that same frame reads 50fps,
	// which is the number you want when deciding what to optimise.
	if(::gxWaitRetrace)
		VIDEO_WaitVSync();
	gxVsyncUs = (unsigned)ticks_to_microsecs(gettime() - tv);
}

static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	// "Copy the camera raster into the raster on the context stack" — the one
	// shape the game ever uses this in (CPostFX::GetBackBuffer and the
	// front-buffer capture in postfx/MBlur). The stub that used to be here
	// returned 0, so every camera-texture in the game held zeros and the
	// COLOUR FILTER, MOTION BLUR and TRAILS options all did nothing at all.
	//
	// The EFB is copied out by the GP (GX_CopyTex), not read by the CPU:
	// same mechanism showRaster already uses for the XFB, and the copy
	// registers are re-written by GX_CopyDisp each present, so nothing here
	// leaks into the display copy.
	(void)x; (void)y;
	Raster *dst = Raster::getCurrentContext();
	if(raster == nil || dst == nil || dst == raster)
		return 0;
	if((dst->type & 0xF) != Raster::CAMERATEXTURE)
		return 0;
	int32 w = dst->width > rmode->fbWidth ? rmode->fbWidth : dst->width;
	int32 h = dst->height > rmode->efbHeight ? rmode->efbHeight : dst->height;
	if(w <= 0 || h <= 0)
		return 0;
	// The raster-side work lives in gxraster.cpp, where GxRaster is visible.
	// No deflicker on a texture grab; the display copy shares the filter
	// state, so the mode's own filter goes back right after.
	extern bool32 gxGrabEFB(Raster*, int32, int32);
	GX_SetCopyFilter(GX_FALSE, nil, GX_FALSE, nil);
	bool32 ok = gxGrabEFB(dst, w, h);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	return ok;
}

// im2D texture, set by the game through rwRENDERSTATETEXTURERASTER.
static Raster *currentTexRaster;
GXTexObj *gxGetTexture(Raster *raster);
bool32 gxTexturePending(Raster *raster);


static u8
gxBlend(uint32 blend)
{
	switch(blend){
	case BLENDZERO:         return GX_BL_ZERO;
	case BLENDONE:          return GX_BL_ONE;
	case BLENDSRCCOLOR:     return GX_BL_SRCCLR;
	case BLENDINVSRCCOLOR:  return GX_BL_INVSRCCLR;
	case BLENDSRCALPHA:     return GX_BL_SRCALPHA;
	case BLENDINVSRCALPHA:  return GX_BL_INVSRCALPHA;
	case BLENDDESTALPHA:    return GX_BL_DSTALPHA;
	case BLENDINVDESTALPHA: return GX_BL_INVDSTALPHA;
	case BLENDDESTCOLOR:    return GX_BL_DSTCLR;
	case BLENDINVDESTCOLOR: return GX_BL_INVDSTCLR;
	default:                return GX_BL_SRCALPHA;
	}
}

static void
applyBlend(void)
{
	// The stencil emulation outranks the plain blend state: a masked pass
	// must blend by destination alpha whatever src/dst the game set, and a
	// mask-writing pass must be allowed to write EFB alpha.
	if(stStencilEnable && stStencilFunc == STENCILEQUAL){
		GX_SetAlphaUpdate(GX_FALSE);
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA,
		    GX_LO_CLEAR);
		return;
	}
	GX_SetAlphaUpdate(stStencilEnable && stStencilPass == STENCILREPLACE ?
	    GX_TRUE : GX_FALSE);
	// vertexAlpha OR textureAlpha, matching gl3 and d3d9. Testing only
	// vertexAlpha here is what made alpha-textured effects draw opaque.
	if(stVertexAlpha || stTextureAlpha || stMaterialAlpha)
		GX_SetBlendMode(GX_BM_BLEND, gxBlend(stSrcBlend),
		    gxBlend(stDstBlend), GX_LO_CLEAR);
	else
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

// Set when the bound texture cannot produce alpha < 255: the alpha test
// then never rejects, so early-Z can stay on. VC turns alpha testing on
// globally, and late-Z forces the GP to texture every pixel before the
// depth test can throw it away — the overdraw cost of that is the frame.
static bool32 stTexOpaque;

static void
applyAlphaTest(void)
{
	u8 ref = (u8)stAlphaRef;
	// (An "opaque texture -> keep early-Z" fast path lived here. It leaked
	// state between draws and whited out the world, and measured no gain,
	// so it is gone. Re-add only with a per-draw parameter, never a static.)
	switch(stAlphaFunc){
	case ALPHAGREATEREQUAL:
		GX_SetAlphaCompare(GX_GEQUAL, ref, GX_AOP_AND, GX_ALWAYS, 0);
		// Z must resolve after texturing when texels can be rejected.
		GX_SetZCompLoc(GX_FALSE);
		break;
	case ALPHALESS:
		GX_SetAlphaCompare(GX_LESS, ref, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_FALSE);
		break;
	default:
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
		GX_SetZCompLoc(GX_TRUE);
		break;
	}
}

static void
applyZMode(void)
{
	// GX does not update the Z buffer while the depth compare is disabled, so
	// "don't test, but do write" cannot be expressed as compare=FALSE. It has
	// to be an always-passing compare instead. CRadar::DrawRadarMask relies on
	// exactly that combination to stamp the round cutout into Z before the map
	// tiles are drawn against it — without this the mask writes nothing and
	// the square map shows straight through the circular border.
	if(!stZTest && stZWrite)
		GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
	else
		GX_SetZMode(stZTest ? GX_TRUE : GX_FALSE, GX_LEQUAL,
		    stZWrite ? GX_TRUE : GX_FALSE);
}

static void
applyCull(void)
{
	// Flipped mapping. The view matrix negates X and Z: each negation
	// flips triangle winding, so the pair leaves winding as RW authored
	// it — but GX's front-face convention is the opposite of RW's, so
	// BACK maps to GX_CULL_FRONT. With the straight mapping the GP keeps
	// the back faces and discards the front ones: you see model interiors
	// (peds read as scattered pieces, rooms lose their walls).
	// Cull nothing. This is the only setting verified to render interiors
	// correctly — Marco's Bistro keeps its walls and ceiling. BOTH mappings
	// were tried and both drop interior walls: CULLBACK -> GX_CULL_FRONT (the
	// original) and CULLBACK -> GX_CULL_BACK. Since neither orientation saves
	// them, the winding of interior geometry is not simply inverted relative
	// to the rest of the scene, and picking either side is guesswork that
	// costs walls.
	//
	// ponytail: this rasterises back faces that could be discarded, so it is a
	// real GP cost, not a free win. It stays until the winding is understood
	// (strip parity through the chunked GX_Begin path is the first suspect).
	// Do not "fix" this back to a front/back mapping without an A/B in an
	// interior first.
	GX_SetCullMode(GX_CULL_NONE);
}

static void
setRenderState(int32 state, void *pvalue)
{
	uint32 value = (uint32)(uintptr)pvalue;

	switch(state){
	case TEXTURERASTER:
		currentTexRaster = (Raster*)pvalue;
		stTextureAlpha = currentTexRaster &&
		    gxRasterHasAlpha(currentTexRaster);
		if(gxStarted) applyBlend();
		break;
	case ZTESTENABLE:
		stZTest = value;
		if(gxStarted) applyZMode();
		break;
	case ZWRITEENABLE:
		stZWrite = value;
		if(gxStarted) applyZMode();
		break;
	case CULLMODE:
		stCullMode = value;
		if(gxStarted) applyCull();
		break;
	case VERTEXALPHA:
		stVertexAlpha = value;
		if(gxStarted) applyBlend();
		break;
	case STENCILENABLE:
		stStencilEnable = value;
		if(!value){
			stStencilFunc = 0;
			stStencilPass = STENCILKEEP;
		}
		if(gxStarted) applyBlend();
		break;
	case STENCILFUNCTION:
		stStencilFunc = value;
		if(gxStarted) applyBlend();
		break;
	case STENCILPASS:
		stStencilPass = value;
		if(gxStarted) applyBlend();
		break;
	case STENCILFAIL:
	case STENCILZFAIL:
	case STENCILFUNCTIONREF:
	case STENCILFUNCTIONMASK:
	case STENCILFUNCTIONWRITEMASK:
		// Accepted so the game's stencil setup is not an error; the
		// dest-alpha emulation has no use for them (ref is always 1,
		// masks always full).
		break;
	case SRCBLEND:
		stSrcBlend = value;
		if(gxStarted) applyBlend();
		break;
	case DESTBLEND:
		stDstBlend = value;
		if(gxStarted) applyBlend();
		break;
	case ALPHATESTFUNC:
		stAlphaFunc = value;
		if(gxStarted) applyAlphaTest();
		break;
	case ALPHATESTREF:
		stAlphaRef = value;
		if(gxStarted) applyAlphaTest();
		break;
	default:
		break;
	}
}

static void*
getRenderState(int32 state)
{
	switch(state){
	case TEXTURERASTER: return currentTexRaster;
	case ZTESTENABLE:   return (void*)(uintptr)stZTest;
	case ZWRITEENABLE:  return (void*)(uintptr)stZWrite;
	case CULLMODE:      return (void*)(uintptr)stCullMode;
	case VERTEXALPHA:   return (void*)(uintptr)stVertexAlpha;
	case SRCBLEND:      return (void*)(uintptr)stSrcBlend;
	case DESTBLEND:     return (void*)(uintptr)stDstBlend;
	case ALPHATESTFUNC: return (void*)(uintptr)stAlphaFunc;
	case ALPHATESTREF:  return (void*)(uintptr)stAlphaRef;
	}
	return 0;
}

// im2D: librw hands us screen-space vertices already transformed.
static void
drawIm2D(PrimitiveType primType, void *vertices, int32 numVertices,
	void *indices, int32 numIndices)
{
	if(!gxStarted || numVertices <= 0)
		return;

	Im2DVertex *verts = (Im2DVertex*)vertices;
	uint16 *idx = (uint16*)indices;
	int32 count = indices ? numIndices : numVertices;
	if(count <= 0)
		return;
	if(count > 65535) count = 65535; // u16 FIFO count

	u8 prim;
	switch(primType){
	case PRIMTYPETRILIST:   prim = GX_TRIANGLES; break;
	case PRIMTYPETRISTRIP:  prim = GX_TRIANGLESTRIP; break;
	case PRIMTYPETRIFAN:    prim = GX_TRIANGLEFAN; break;
	case PRIMTYPELINELIST:  prim = GX_LINES; break;
	case PRIMTYPEPOLYLINE:  prim = GX_LINESTRIP; break;
	default: return;
	}

	stTexOpaque = 0; // 2D never takes the early-Z fast path
	GXTexObj *tex = gxGetTexture(currentTexRaster);
	if(tex)
		GX_LoadTexObj(tex, GX_TEXMAP0);

	setupIm2DVtxDesc(tex != nil);
	setIm2DMatrices();

	// Blend truth at draw time, additive draws only (rain family). One line
	// per state change, bounded — gecko drops floods.
	{
		extern bool32 gxForceAddBlend;
		static uint32 lastKey = ~0u;
		static int32 left = 40;
		uint32 key = (stVertexAlpha<<16) | (stSrcBlend<<8) | stDstBlend;
		if(stSrcBlend == BLENDONE && stDstBlend == BLENDONE &&
		   key != lastKey && left-- > 0){
			lastKey = key;
			char bl[64];
			snprintf(bl, sizeof(bl), "IM2D va=%u src=%u dst=%u ta=%u",
			    (unsigned)stVertexAlpha, (unsigned)stSrcBlend,
			    (unsigned)stDstBlend, (unsigned)stTextureAlpha);
			GeckoLog(bl);
		}
		if(gxForceAddBlend && stSrcBlend == BLENDONE && stDstBlend == BLENDONE)
			GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
	}

	// Corner-artifact hunter: any small quad landing in the top-left region
	// names itself — texture, size, blend, alpha. Bounded; card-logged so
	// gecko drops cost nothing.
	if(count <= 8){
		float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
		for(int32 i = 0; i < count; i++){
			Im2DVertex *v = &verts[indices ? idx[i] : i];
			if(v->x < minx) minx = v->x;
			if(v->x > maxx) maxx = v->x;
			if(v->y < miny) miny = v->y;
			if(v->y > maxy) maxy = v->y;
		}
		if(minx < 200.0f && miny < 200.0f && maxx - minx > 32.0f &&
		   maxx - minx < 340.0f && maxy - miny > 32.0f && maxy - miny < 340.0f &&
		   maxx < 460.0f && maxy < 380.0f){
			static int32 left = 30;
			if(left-- > 0){
				char cl[128];
				snprintf(cl, sizeof(cl),
				    "CORNER %.0f,%.0f-%.0f,%.0f tex=%p %dx%d va=%u s%u d%u a=%u\n",
				    minx, miny, maxx, maxy, (void*)currentTexRaster,
				    currentTexRaster ? currentTexRaster->width : 0,
				    currentTexRaster ? currentTexRaster->height : 0,
				    (unsigned)stVertexAlpha, (unsigned)stSrcBlend,
				    (unsigned)stDstBlend,
				    (unsigned)verts[indices ? idx[0] : 0].a);
				DVD_FS_GUARD;
				FILE *cf = fopen("dvd:/automenu.log", "a");
				if(cf){ fputs(cl, cf); fclose(cf); }
			}
		}
	}

	gxLastPath = "2d";
	gxLast.path = "2d";
	gxLast.texName = nil;
	gxLast.texRaster = currentTexRaster;
	gxLast.texW = currentTexRaster ? currentTexRaster->width : 0;
	gxLast.texH = currentTexRaster ? currentTexRaster->height : 0;
	gxLast.hasTex = tex != nil;
	gxLast.prim = prim;
	gxLast.vtxfmt = 0;
	gxLast.count = count;
	gxLast.posFrac = gxLast.uvFrac = 0xFF;

	GX_Begin(prim, GX_VTXFMT0, count);
	for(int32 i = 0; i < count; i++){
		Im2DVertex *v = &verts[indices ? idx[i] : i];
		GX_Position3f32(v->x, v->y, -v->z);
		GX_Color4u8(v->r, v->g, v->b, v->a);
		if(tex)
			GX_TexCoord2f32(v->u, v->v);
	}
	GX_End();
}

static void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	drawIm2D(primType, vertices, numVertices, nil, 0);
}

static void
im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices,
	int32 numVertices, void *indices, int32 numIndices)
{
	drawIm2D(primType, vertices, numVertices, indices, numIndices);
}

static void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	uint16 idx[2] = { (uint16)vert1, (uint16)vert2 };
	drawIm2D(PRIMTYPELINELIST, vertices, numVertices, idx, 2);
}

static void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2,
	int32 vert3)
{
	uint16 idx[3] = { (uint16)vert1, (uint16)vert2, (uint16)vert3 };
	drawIm2D(PRIMTYPETRILIST, vertices, numVertices, idx, 3);
}

// 3D draw setup. Lighting runs on the GX color channel hardware:
//   chan = matReg * (ambSrc + sum(lights))
// - unlit:            chan from vertex color, lights off (RW: prelight*mat)
//   ...the matReg multiply for unlit prelight happens via GX too: matsrc REG,
//   ambsrc VTX, no lights -> chan = matReg * vtxColor.
// - lit, no prelight: ambsrc REG = ambLight*surfAmb, matReg = matColor,
//   lights scaled by surfDiffuse -> exactly RW's formula distributed.
// - lit + prelight:   ambsrc VTX (prelight), lights on; the global ambient
//   term can't join the channel (slot taken), so TEV stage 0 adds it as a
//   constant before the texture modulate. Native two-stage TEV.
// Alpha never sees lighting: GX_ALPHA0 is configured unlit with the same
// sources, and material alpha rides the TEV konst when prelight supplies
// the vertex alpha.
static bool32 gx3DMemoDirty = 1;
static void gx3DMemoInvalidate(void) { gx3DMemoDirty = 1; }
static void gx2DMemoInvalidate(void);

static inline int
clamp255i(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

// (a*b)/255 for a,b in 0..255, exact, without a divide.
static inline int
mul255(int a, int b)
{
	int t = a*b + 128;
	return (t + (t >> 8)) >> 8;
}

static void
setup3DDraw(bool32 textured, bool32 lit, bool32 prelit, bool32 haveNormals,
	RGBA matcol, float surfAmb, const RGBAf *ambLight, u8 lightMask,
	bool32 sendColor, bool32 indexed = 0, RGBA ambAdd = {0,0,0,0},
	u8 posFrac = 0xFF, u8 uvFrac = 0xFF)
{
	// stMaterialAlpha is set by the CALLER from the real material — the
	// atomic path bakes the material colour into the vertex stream and hands
	// this function white, which is how the first version of this flag
	// managed to never fire and left the mission markers opaque anyway.
	// posFrac/uvFrac: 0xFF means the attribute is float32, anything else is the
	// binary shift of a gxPackGeometry-quantised int16 array. The shift is part
	// of the vertex format, so it MUST be part of the memo key — two geometries
	// with different extents pick different shifts, and reusing the previous
	// format would scale one of them by the other's factor.
	// Memo: VC submits long runs of meshes with identical setup. Each call
	// is ~30 GP register writes + a projection reload; skipping the repeats
	// is the cheapest real win in the immediate-mode path.
	// ponytail: flat key compare, no hashing — the run lengths are what pay.
	static bool32 mT, mL, mP, mN;
	static RGBA mMat;
	static float mAmb;
	static RGBAf mAmbL;
	static u8 mMask;
	static bool32 mIdx;
	static RGBA mAmbAdd;
	static bool32 mTexOpaque;
	static bool32 mSend;
	static u8 mPosFrac = 0xFF, mUvFrac = 0xFF;
	static void *mEnvTex;
	static u32 mEnvK32;
	u32 envK32 = ((u32)gxEnvK.r<<24)|((u32)gxEnvK.g<<16)|((u32)gxEnvK.b<<8)|gxEnvK.a;
	RGBAf ambl = ambLight ? *ambLight : (RGBAf){0,0,0,0};
	if(!gx3DMemoDirty && mT == textured && mL == lit && mP == prelit &&
	   mN == haveNormals && mMask == lightMask && mAmb == surfAmb &&
	   mIdx == indexed && mTexOpaque == stTexOpaque && mSend == sendColor &&
	   mPosFrac == posFrac && mUvFrac == uvFrac &&
	   *(uint32*)&mAmbAdd == *(uint32*)&ambAdd &&
	   *(uint32*)&mMat == *(uint32*)&matcol &&
	   mAmbL.red == ambl.red && mAmbL.green == ambl.green &&
	   mAmbL.blue == ambl.blue &&
	   mEnvTex == (void*)gxEnvTex && mEnvK32 == envK32){
		applyBlend();
		applyZMode();
		applyAlphaTest();
		applyCull();
		return;
	}
	gx3DMemoDirty = 0;
	gx2DMemoInvalidate();
	mT = textured; mL = lit; mP = prelit; mN = haveNormals;
	mMask = lightMask; mAmb = surfAmb; mMat = matcol; mAmbL = ambl;
	mEnvTex = (void*)gxEnvTex; mEnvK32 = envK32;
	mIdx = indexed; mAmbAdd = ambAdd; mTexOpaque = stTexOpaque;
	mSend = sendColor;
	mPosFrac = posFrac; mUvFrac = uvFrac;

	u8 attrMode = indexed ? GX_INDEX16 : GX_DIRECT;
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, attrMode);
	if(posFrac == 0xFF)
		GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	else
		GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_S16, posFrac);
	bool32 hwLights = lit && haveNormals && lightMask != GX_LIGHTNULL;
	bool32 envOn = gxEnvTex != nil && !gxEnvUV2;
	bool32 dualOn = gxEnvTex != nil && gxEnvUV2;
	// Normals go to the GP for hardware lights OR for the env/rim texgen —
	// the emission loops in atomicRenderCB use the same predicate, and the
	// two disagreeing is a FIFO desync. The lightmap stage sends UV set 1
	// instead; the two are never armed together.
	if(hwLights || envOn){
		GX_SetVtxDesc(GX_VA_NRM, attrMode);
		GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	}
	if(dualOn){
		GX_SetVtxDesc(GX_VA_TEX1, attrMode);
		GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
	}
	// vertex colors whenever the channel reads them. Decided by the caller
	// (see atomicRenderCB) so the vertex descriptor here and the vertex
	// stream there can never disagree — a mismatch desyncs the FIFO.
	if(sendColor){
		GX_SetVtxDesc(GX_VA_CLR0, attrMode);
		GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	}

	// Channel layout (after Polyphase, a proven GX forward renderer):
	// COLOR0A0 carries the raw vertex color (prelight) with lighting off;
	// COLOR1A1 is the hardware-lit channel: matReg * (ambReg + lights).
	// The unused channel must still be configured or the GP misbehaves.
	GXColor mat = { matcol.red, matcol.green, matcol.blue, matcol.alpha };
	bool32 twoChans = sendColor && hwLights;
	GX_SetNumChans(twoChans ? 2 : 1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
	    GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	u8 litChan, litChanCol;
	if(twoChans){
		litChan = GX_COLOR1A1;
		litChanCol = GX_COLOR1;
	}else{
		litChan = GX_COLOR0A0;
		litChanCol = GX_COLOR0;
	}
	GX_SetChanMatColor(litChan, mat);
	if(hwLights){
		GXColor amb = { 0, 0, 0, 255 };
		if(ambLight){
			float r = ambLight->red   * surfAmb * 255.0f;
			float g = ambLight->green * surfAmb * 255.0f;
			float b = ambLight->blue  * surfAmb * 255.0f;
			amb.r = r > 255.0f ? 255 : (u8)r;
			amb.g = g > 255.0f ? 255 : (u8)g;
			amb.b = b > 255.0f ? 255 : (u8)b;
		}
		GX_SetChanAmbColor(litChan, amb);
		GX_SetChanCtrl(litChanCol, GX_ENABLE, GX_SRC_REG, GX_SRC_REG,
		    lightMask, GX_DF_CLAMP, GX_AF_NONE);
		// alpha stays unlit: material alpha from the register
		GX_SetChanCtrl(litChan == GX_COLOR1A1 ? GX_ALPHA1 : GX_ALPHA0,
		    GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		    GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	}else if(!sendColor){
		// flat material color, no lights
		GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG,
		    GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	}

	// TEV. Texel = texture color, or ONE when untextured.
	// - lit+prelit:  PREV = texel*litChan + texel*prelight  (REG0 scratch)
	// - lit:         PREV = texel*litChan
	// - unlit:       PREV = texel*vertexColor
	// Alpha: texel.a * (prelit ? prelight.a : matcol.a).
	u8 stage = GX_TEVSTAGE0;
	u8 texC = textured ? GX_CC_TEXC : GX_CC_ONE;
	u8 texA = textured ? GX_CA_TEXA : GX_CA_KONST; // KONST0 alpha = 255
	GXColor kone = { 255, 255, 255, 255 };
	GX_SetTevKColor(GX_KCOLOR0, kone);

	if(textured){
		GX_SetVtxDesc(GX_VA_TEX0, attrMode);
		if(uvFrac == 0xFF)
			GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
		else
			GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_S16, uvFrac);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	}
	{
		u8 ntg = textured ? 1 : 0;
		if(envOn)
			GX_SetTexCoordGen(textured ? GX_TEXCOORD1 : GX_TEXCOORD0,
			    GX_TG_MTX2x4, GX_TG_NRM, GX_TEXMTX0);
		else if(dualOn)
			GX_SetTexCoordGen(textured ? GX_TEXCOORD1 : GX_TEXCOORD0,
			    GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
		if(envOn || dualOn)
			ntg++;
		GX_SetNumTexGens(ntg);
	}

	bool32 prelitLit = hwLights && prelit;
	if(prelitLit){
		// REG0 = texel * prelight (color and alpha)
		GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_K0_A);
		GX_SetTevOrder(stage, textured ? GX_TEXCOORD0 : GX_TEXCOORDNULL,
		    textured ? GX_TEXMAP0 : GX_TEXMAP_NULL, GX_COLOR0A0);
		GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_RASC, texC, GX_CC_ZERO);
		GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_RASA, texA, GX_CA_ZERO);
		GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVREG0);
		GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVREG0);
		stage++;
	}

	// main stage: PREV = texel * (lit channel | vertex color | material)
	GX_SetTevKAlphaSel(stage, GX_TEV_KASEL_K0_A);
	GX_SetTevOrder(stage, textured ? GX_TEXCOORD0 : GX_TEXCOORDNULL,
	    textured ? GX_TEXMAP0 : GX_TEXMAP_NULL,
	    hwLights ? litChan : GX_COLOR0A0);
	GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_RASC, texC, GX_CC_ZERO);
	GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_RASA, texA, GX_CA_ZERO);
	GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
	    GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
	    GX_TRUE, GX_TEVPREV);
	stage++;

	if(prelitLit){
		// PREV += REG0 (dynamic + baked); alpha keeps the baked product,
		// which carries prelight.a * texel.a
		GX_SetTevOrder(stage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
		GX_SetTevColorIn(stage, GX_CC_CPREV, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
		GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
		GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVPREV);
		stage++;
	}

	// Ambient for meshes fed from static arrays: no CPU pass can touch
	// their colors, so add amb*surfAmb as a TEV constant (PREV += K*texel).
	if(ambAdd.red | ambAdd.green | ambAdd.blue){
		GXColor ka = { ambAdd.red, ambAdd.green, ambAdd.blue, 255 };
		GX_SetTevKColor(GX_KCOLOR1, ka);
		GX_SetTevKColorSel(stage, GX_TEV_KCSEL_K1);
		GX_SetTevOrder(stage, textured ? GX_TEXCOORD0 : GX_TEXCOORDNULL,
		    textured ? GX_TEXMAP0 : GX_TEXMAP_NULL, GX_COLORNULL);
		GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_KONST, texC, GX_CC_CPREV);
		GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
		GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVPREV);
		stage++;
	}
	// The armed extra stage: PREV += envTex * K2. The texture arrives
	// through the spheremap texgen, so this is the matfx environment
	// reflection on vehicles and the rim term on peds, both on the GP.
	if(envOn || dualOn){
		u8 envMap = textured ? GX_TEXMAP1 : GX_TEXMAP0;
		u8 envCoord = textured ? GX_TEXCOORD1 : GX_TEXCOORD0;
		GX_LoadTexObj(gxEnvTex, envMap);
		GX_SetTevKColor(GX_KCOLOR2, gxEnvK);
		GX_SetTevKColorSel(stage, GX_TEV_KCSEL_K2);
		GX_SetTevOrder(stage, envCoord, envMap, GX_COLORNULL);
		GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST,
		    GX_CC_CPREV);
		GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
		    GX_CA_APREV);
		GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVPREV);
		stage++;
	}
	GX_SetNumTevStages(stage - GX_TEVSTAGE0);

	applyBlend();
	applyZMode();
	applyAlphaTest();
	applyCull();
	GX_LoadProjectionMtx(gxProj, gxProjType);
}

// ---- screen droplets: the neo lens-drop shader, as two TEV stages --------
//
// The PC build combines a drop-shape mask (TEX0) with the captured frame
// sampled through a second UV set (TEX1) in a pixel shader. The TEV does the
// same combine in hardware: PREV.rgb = screen texel, PREV.a = mask alpha *
// vertex alpha, standard alpha blend. Called by screendroplets.cpp, which
// owns the drop simulation; this owns only the draw.
static GXTexObj *gxDropMask, *gxDropScreen;
// Global namespace on purpose: set from game code (screendroplets.cpp) via a
// plain extern, no rw::gx include dance. Red = mask shape/alpha isolated.
bool32 gxScreenDropDebugRed;
bool32 gxScreenDropDebugMask;
static uint32 gxDropSaved[6];
// Additive glint for the MBlur water/blood drops (0 = plain modulate, the
// rain lens-drops). Set before gxDropletBegin, cleared by gxDropletEnd.
uint8 gxDropletBrighten;
bool32 gxForceAddBlend;   // dvd:/autoblend.txt: force ONE/ONE on additive im2D
uint32 gxDropQuads;      // quads actually drawn this frame
int32 gxDropBeginState;  // 1 ok, -1 mask nil, -2 screen nil, 0 never ran

GXTexObj *gxGetTexture(Raster*);

void
gxDropletBegin(Raster *mask, Raster *screen)
{
	// ponytail: drawn through the exact im2D setup the HUD uses every frame,
	// with the mask as the one texture — the bespoke two-texture TEV path
	// missampled the mask (flat squares) and refraction is parked until that
	// is understood. Soft translucent drops, correct shape, no refraction.
	gxDropMask = mask ? gxGetTexture(mask) : nil;
	gxDropScreen = screen ? gxGetTexture(screen) : nil;
	gxDropBeginState = gxDropMask == nil ? -1 : gxDropScreen == nil ? -2 : 1;
	if(gxDropMask == nil)
		return;
	// The droplet pass owns its blend/Z; save the tracked state and restore
	// in End so the next draw's appliers see what the game actually set.
	gxDropSaved[0] = stVertexAlpha; gxDropSaved[1] = stSrcBlend;
	gxDropSaved[2] = stDstBlend;    gxDropSaved[3] = stZTest;
	gxDropSaved[4] = stZWrite;      gxDropSaved[5] = stStencilEnable;
	stVertexAlpha = 1;
	stSrcBlend = BLENDSRCALPHA;
	stDstBlend = BLENDINVSRCALPHA;
	stZTest = 0;
	stZWrite = 0;
	// The MBlur fx loop runs with the stencil emulation armed; left on, the
	// blend applier switches to the dest-alpha override and paints the whole
	// quad opaque — the corner square. Droplets blend by their own alpha.
	stStencilEnable = 0;
	GX_LoadTexObj(gxDropMask, GX_TEXMAP0);
	setupIm2DVtxDesc(1);
	setIm2DMatrices();
	GX_SetCullMode(GX_CULL_NONE);
	// The original screenDroplet shader is vtxColor x mask x SCREEN — the
	// refraction is the screen texture sampled over a magnified span. Added
	// on top of the proven im2D base one stage at a time: stage0 stays the
	// im2D modulate (vtx x mask), stage1 multiplies the grabbed frame in.
	if(gxDropScreen){
		GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
		GX_SetNumTexGens(2);
		GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
		GX_LoadTexObj(gxDropScreen, GX_TEXMAP1);
		GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
		GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_CPREV, GX_CC_ZERO);
		// screen alpha stays out of the product: the EFB copy is RGB565 —
		// its "alpha" is undefined, and the PC backbuffer reads 255 here.
		GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
		GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVPREV);
		GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1,
		    GX_TRUE, GX_TEVPREV);
		if(gxDropletBrighten){
			// PREV + K2: the pass-1 gray glint of the PC water drop, which
			// is what keeps the refracted patch from reading as a dark spot
			// over dark scenery.
			GXColor k = { gxDropletBrighten, gxDropletBrighten,
			              gxDropletBrighten, 255 };
			GX_SetTevKColor(GX_KCOLOR2, k);
			GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K2);
			GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL,
			    GX_COLORNULL);
			GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_KONST, GX_CC_ZERO,
			    GX_CC_ZERO, GX_CC_CPREV);
			GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO,
			    GX_CA_ZERO, GX_CA_APREV);
			GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO,
			    GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
			GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO,
			    GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
			GX_SetNumTevStages(3);
		}else
		GX_SetNumTevStages(2);
	}
}

void
gxDropletQuad(const float *px, const float *py, float u2l, float v2t,
    float u2r, float v2b, uint32 rgba)
{
	if(gxDropMask == nil)
		return;
	static const float mu[4]  = { 0.0f, 0.0f, 1.0f, 1.0f };
	static const float mv2[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
	gxDropQuads++;
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	for(int i = 0; i < 4; i++){
		GX_Position3f32(px[i], py[i], 0.0f);
		GX_Color4u8((u8)(rgba>>24), (u8)(rgba>>16), (u8)(rgba>>8), (u8)rgba);
		GX_TexCoord2f32(mu[i], mv2[i]);
		if(gxDropScreen)
			// Corner order: i0/i3 are the TOP of the quad. The GX grab is
			// top-down, so top maps to vt (the PC table's flip undoes GL's
			// bottom-up storage and must not be copied here).
			GX_TexCoord2f32(i < 2 ? u2l : u2r, (i == 0 || i == 3) ? v2t : v2b);
	}
	GX_End();
}

void
gxDropletEnd(void)
{
	if(gxDropMask){
		stVertexAlpha = gxDropSaved[0]; stSrcBlend = gxDropSaved[1];
		stDstBlend = gxDropSaved[2];    stZTest = gxDropSaved[3];
		stZWrite = gxDropSaved[4];      stStencilEnable = gxDropSaved[5];
	}
	gxDropMask = gxDropScreen = nil;
	gxDropletBrighten = 0;
	// Whatever draws next rebuilds its own state.
	gx3DMemoInvalidate();
	gx2DMemoInvalidate();
}

// Compatibility wrapper for the im3D path: untextured/unlit semantics.
static void
setup3DVtxDesc(bool32 textured)
{
	// im3D always streams a per-vertex color
	gxEnvTex = nil;
	setup3DDraw(textured, 0, 0, 0, makeRGBA(255,255,255,255), 1.0f, nil,
	    GX_LIGHTNULL, 1);
}

static u8
gxPrim(PrimitiveType primType)
{
	switch(primType){
	case PRIMTYPETRILIST:   return GX_TRIANGLES;
	case PRIMTYPETRISTRIP:  return GX_TRIANGLESTRIP;
	case PRIMTYPETRIFAN:    return GX_TRIANGLEFAN;
	case PRIMTYPELINELIST:  return GX_LINES;
	case PRIMTYPEPOLYLINE:  return GX_LINESTRIP;
	case PRIMTYPEPOINTLIST: return GX_POINTS;
	default: return 0xFF;
	}
}

// im3D: the game hands us world-space-ish vertices plus an optional transform.
static Im3DVertex *im3dVerts;
static int32 im3dNumVerts;
static Matrix im3dWorld;
static bool32 im3dHaveWorld;

static void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	(void)flags;
	im3dVerts = (Im3DVertex*)vertices;
	im3dNumVerts = numVertices;
	if(world){
		im3dWorld = *world;
		im3dHaveWorld = TRUE;
	}else
		im3dHaveWorld = FALSE;
}

static void
im3dDraw(PrimitiveType primType, void *indices, int32 numIndices)
{
	if(!gxStarted || !gxHaveCamera || im3dVerts == nil)
		return;
	u8 prim = gxPrim(primType);
	if(prim == 0xFF)
		return;

	GXTexObj *tex = gxGetTexture(currentTexRaster);
	if(tex)
		GX_LoadTexObj(tex, GX_TEXMAP0);

	setup3DVtxDesc(tex != nil);
	loadWorldMtx(im3dHaveWorld ? &im3dWorld : nil);

	uint16 *idx = (uint16*)indices;
	int32 count = idx ? numIndices : im3dNumVerts;
	if(count > 65535) count = 65535; // u16 FIFO count; im3D never legitimately exceeds it
	GX_Begin(prim, GX_VTXFMT1, count);
	for(int32 i = 0; i < count; i++){
		Im3DVertex *v = &im3dVerts[idx ? idx[i] : i];
		GX_Position3f32(v->position.x, v->position.y, v->position.z);
		GX_Color4u8(v->r, v->g, v->b, v->a);
		if(tex)
			GX_TexCoord2f32(v->u, v->v);
	}
	GX_End();
}

static void
im3DRenderPrimitive(PrimitiveType primType)
{
	im3dDraw(primType, nil, 0);
}

static void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	im3dDraw(primType, indices, numIndices);
}

static void
im3DEnd(void)
{
	im3dVerts = nil;
	im3dNumVerts = 0;
}

// Default atomic pipeline: immediate-mode walk of the generic geometry with
// the gl3 default-shader lighting model computed on the CPU:
//   color = (prelight + amb*surfAmb + dir*surfDiff*max(0,N.-L)) * matColor
// ponytail: CPU-fed vertices every frame; the upgrade path is cached GX
// display lists per mesh when this becomes the bottleneck.
// Precompute clamp(prelight + ambient) * material for a whole geometry.
//
// This is the array the indexed path needs and never had: the immediate path
// computes the same expression per vertex per frame and throws it away, so
// there was nothing to index. Every input except prelight is constant per
// mesh, so the result only changes when the ambient or the material does —
// with the time of day, not with the frame.
//
// Returns false when the geometry is not a candidate (skinned, lit by
// directionals, or no prelight), in which case the caller stays immediate.
static bool32
gxBuildColorCache(Geometry *geo, GxGeoExt *g, RGBA *prelit, int32 numDir,
	int ambR8, int ambG8, int ambB8, RGBA matcol)
{
	if(prelit == nil || numDir != 0)
		return 0;
	// A fading mesh changes matcol.alpha every frame, which is a key miss
	// and an IN-PLACE rebuild of an array the GP may still be DMA-reading
	// from the previous frame — the flush orders memory, it does not wait
	// for the GP. Big per-frame deltas made the tear visible: freshly
	// streamed models (exactly the distant ones, and interior floors)
	// flashed dark patches through their whole fade. Fades take the
	// immediate path instead; the cache serves the steady state, where a
	// rebuild only ever moves one timecycle step and a tear is invisible.
	if(matcol.alpha != 255)
		return 0;
	int32 n = geo->numVertices;
	if(n <= 0)
		return 0;

	// One key covering everything that is not per-vertex. A mismatch rebuilds;
	// a match is free.
	uint32 key = (uint32)ambR8 | ((uint32)ambG8<<8) | ((uint32)ambB8<<16) ^
	    (*(uint32*)&matcol * 2654435761u);
	if(g->colors && g->colorCount == n && g->colorKey == key)
		return 1;

	if(g->colors == nil || g->colorCount != n){
		rwFree(g->colors);
		g->colors = (RGBA*)rwMalloc(n*sizeof(RGBA), MEMDUR_EVENT | ID_GEOMETRY);
		if(g->colors == nil){ g->colorCount = 0; return 0; }
		g->colorCount = n;
	}
	for(int32 i = 0; i < n; i++){
		int r8 = clamp255i(ambR8 + prelit[i].red);
		int g8 = clamp255i(ambG8 + prelit[i].green);
		int b8 = clamp255i(ambB8 + prelit[i].blue);
		g->colors[i].red   = (uint8)mul255(r8, matcol.red);
		g->colors[i].green = (uint8)mul255(g8, matcol.green);
		g->colors[i].blue  = (uint8)mul255(b8, matcol.blue);
		g->colors[i].alpha = (uint8)mul255(prelit[i].alpha, matcol.alpha);
	}
	g->colorKey = key;
	DCFlushRange(g->colors, n*sizeof(RGBA));
	return 1;
}

static void
atomicRenderCB(ObjPipeline *pipe, Atomic *atomic)
{
	(void)pipe;
	Geometry *geo = atomic->geometry;
	if(geo == nil || geo->flags & Geometry::NATIVE ||
	   geo->meshHeader == nil || geo->morphTargets == nil ||
	   !gxStarted || !gxHaveCamera)
		return;

	// gxPackGeometry replaces the float positions and texcoords of streamed
	// world geometry with int16 arrays and frees the originals, so either side
	// can legitimately be nil — but not both.
	GxGeoExt *gpk = PLUGINOFFSET(GxGeoExt, geo, gxGeoOffset);
	const int16 *packPos = (gpk->packed & GXPACK_POS) ? gpk->pos : nil;
	const int16 *packUV = (gpk->packed & GXPACK_UV) ? gpk->uv : nil;
	u8 posFrac = packPos ? gpk->posShift : 0xFF;
	u8 uvFrac = packUV ? gpk->uvShift : 0xFF;
	// Both sides nil happens when a pack ran out of memory after freeing the
	// floats; feeding GX a null array crashed the GC build (nil+0x14 read in
	// this function). Skip the atomic and say so once.
	if(geo->morphTargets[0].vertices == nil && packPos == nil){
		static bool32 said;
		if(!said){ said = 1; GeckoLog("GXNILGEO"); }
		return;
	}

	V3d *verts = geo->morphTargets[0].vertices;
	if(verts == nil && packPos == nil)
		return;
	TexCoords *uv = geo->numTexCoordSets > 0 ? geo->texCoords[0] : nil;
	TexCoords *uv2 = geo->numTexCoordSets > 1 ? geo->texCoords[1] : nil;
	RGBA *prelit = (geo->flags & Geometry::PRELIT) ? geo->colors : nil;
	V3d *normals = geo->morphTargets[0].normals;

	// Lights, in the gl3 lightingCB way. Directions come in world space;
	// rotate them into object space once instead of rotating every normal.
	WorldLights lights;
	Light *directionals[4];
	memset(&lights, 0, sizeof(lights));
	lights.directionals = directionals;
	lights.numDirectionals = 4;
	World *rwworld = (World*)engine->currentWorld;
	bool lit = (geo->flags & Geometry::LIGHT) && rwworld != nil;
	if(lit)
		rwworld->enumerateLights(atomic, &lights);
	else
		lights.numDirectionals = 0;

	Matrix *world = atomic->getFrame()->getLTM();

	// CPU skinning: blend vertices with the same object-space bone
	// matrices gl3 uploads to its vertex shader (bind-pose inverse times
	// current node matrix), then draw through the normal path.
	Skin *skin = Skin::get(geo);
	HAnimHierarchy *hier = skin ? Skin::getHierarchy(atomic) : nil;
	bool32 skinnedAtomic = 0;
	if(!GX_NO_SKIN && skin && hier && skin->numBones == hier->numNodes &&
	   skin->weights && skin->indices){
		enum { MAXBONES = 128 };
		static Matrix boneMats[MAXBONES];
		static V3d *skinBuf;
		static int32 skinCap;
		int32 nb = skin->numBones > MAXBONES ? MAXBONES : skin->numBones;
		Matrix *invMats = (Matrix*)skin->inverseMatrices;
		// Matrix::mult short-circuits when EITHER operand carries the
		// IDENTITY flag — it returns the other operand untouched. librw
		// clears it on the inverse matrices for exactly that reason; the
		// hierarchy matrices and the inverted atomic LTM need the same
		// treatment or a stale flag silently drops a bone from the chain
		// (limbs collapse onto the parent while the rest animates).
		Matrix hm;
		if(GX_SKIN_LOCALSPACE || (hier->flags & HAnimHierarchy::LOCALSPACEMATRICES)){
			for(int32 i = 0; i < nb; i++){
				invMats[i].flags = 0;
				hm = hier->matrices[i];
				hm.flags = 0;
				Matrix::mult(&boneMats[i], &invMats[i], &hm);
			}
		}else{
			Matrix invAtm, tmp;
			Matrix::invert(&invAtm, world);
			invAtm.flags = 0;
			for(int32 i = 0; i < nb; i++){
				invMats[i].flags = 0;
				hm = hier->matrices[i];
				hm.flags = 0;
				Matrix::mult(&tmp, &hm, &invAtm);
				tmp.flags = 0;
				Matrix::mult(&boneMats[i], &invMats[i], &tmp);
			}
		}
		int32 need = geo->numVertices * (normals ? 2 : 1);
		if(need > skinCap){
			free(skinBuf);
			skinBuf = (V3d*)malloc(need*sizeof(V3d));
			skinCap = skinBuf ? need : 0;
		}
		if(skinBuf){
			skinnedAtomic = 1;
			V3d *outP = skinBuf;
			V3d *outN = normals ? skinBuf + geo->numVertices : nil;
			for(int32 i = 0; i < geo->numVertices; i++){
				float *wt = &skin->weights[i*4];
				uint8 *bi = &skin->indices[i*4];
				V3d v = verts[i];
				V3d nv = normals ? normals[i] : v;
				V3d p = { 0.0f, 0.0f, 0.0f };
				V3d n2 = { 0.0f, 0.0f, 0.0f };
				for(int32 j = 0; j < 4; j++){
					float w = wt[j];
					if(w == 0.0f)
						continue;
					Matrix *M = &boneMats[bi[j] < nb ? bi[j] : 0];
					p.x += w*(M->right.x*v.x + M->up.x*v.y + M->at.x*v.z + M->pos.x);
					p.y += w*(M->right.y*v.x + M->up.y*v.y + M->at.y*v.z + M->pos.y);
					p.z += w*(M->right.z*v.x + M->up.z*v.y + M->at.z*v.z + M->pos.z);
					if(outN){
						n2.x += w*(M->right.x*nv.x + M->up.x*nv.y + M->at.x*nv.z);
						n2.y += w*(M->right.y*nv.x + M->up.y*nv.y + M->at.y*nv.z);
						n2.z += w*(M->right.z*nv.x + M->up.z*nv.y + M->at.z*nv.z);
					}
				}
				outP[i] = p;
				if(outN)
					outN[i] = n2;
			}
			verts = outP;
			if(normals)
				normals = outN;
		}
	}

	// Lighting is computed per vertex on the CPU, exactly as librw's gl3
	// default.vert does — NOT on GX's fixed-function lighting channels.
	//
	// The two models are not interchangeable. gl3 (and therefore reVC on every
	// platform that works) computes
	//     color = prelight + amb*surfAmb + sum(max(0,N.-L))*lightCol*surfDiff
	//     color = clamp(color, 0, 1) * matColor
	// GX channels can only produce matReg * (ambReg + sum(lights)): the
	// prelight term cannot participate, and the clamp cannot happen before the
	// material multiply. Mapping one onto the other needed a two-stage TEV for
	// prelit+lit, a CPU ambient term for geometry without normals, and a TEV
	// konst for the rest — and every mesh that ended up sourcing its color from
	// GX_SRC_REG rendered black, which is every non-prelit mesh in the game:
	// peds, cutscene actors and props. Prelit world geometry was unaffected
	// because it reads GX_SRC_VTX, which is why buildings and roads looked fine
	// while characters were silhouettes.
	//
	// Doing the reference math on the CPU and streaming the result as a plain
	// vertex color removes the whole hardware-lighting path along with that
	// entire class of bug, and is less code than the mapping it replaces.
	bool lit2 = lit && normals != nil;
	int32 numDir = 0;
	V3d objDir[2];   // light directions rotated into atomic object space
	RGBAf dirCol[2];
	if(lit2){
		numDir = lights.numDirectionals > 2 ? 2 : lights.numDirectionals;
		for(int32 k = 0; k < numDir; k++){
			Light *l = lights.directionals[k];
			V3d at = l->getFrame()->getLTM()->at;
			// gl3 lights world-space normals; rotating the light into object
			// space instead is the same dot product for a rotation matrix and
			// costs 2 transforms per light instead of one per vertex.
			objDir[k].x = world->right.x*at.x + world->right.y*at.y + world->right.z*at.z;
			objDir[k].y = world->up.x*at.x    + world->up.y*at.y    + world->up.z*at.z;
			objDir[k].z = world->at.x*at.x    + world->at.y*at.y    + world->at.z*at.z;
			dirCol[k] = l->color;
		}
	}
	u8 lightMask = GX_LIGHTNULL; // GX hardware lights are never used now

	// Flush once per geometry, not per frame: the GP reads these arrays
	// straight from RAM, but the CPU only writes them at stream time.
	// Re-flushing every frame cost more than the indexed path saved.
	// ponytail: direct-mapped table, collisions just re-flush (harmless).
	bool32 needFlush = 1;
	{
		enum { FTAB = 512 };
		static void *ftab[FTAB];
		uint32 h = ((uintptr)geo >> 4) & (FTAB-1);
		if(ftab[h] == geo)
			needFlush = 0;
		else
			ftab[h] = geo;
	}
	// Indexed-attribute arrays: the GP DMA-fetches vertex data straight
	// from these buffers, so the CPU streams 2-byte indices instead of
	// full vertices. Only bind/flush when the indexed path is active —
	// doing it in direct mode costs a full cache flush per atomic per
	// frame for nothing (measured: 19fps -> 8fps).
	if(skinnedAtomic)
		GX_InvVtxCache();
	if(packPos){
		GX_SetArray(GX_VA_POS, (void*)packPos, 3*sizeof(int16));
		if(needFlush)
			DCFlushRange((void*)packPos, geo->numVertices*3*sizeof(int16));
	}else{
		GX_SetArray(GX_VA_POS, verts, sizeof(V3d));
		if(needFlush || skinnedAtomic)
			DCFlushRange(verts, geo->numVertices*sizeof(V3d));
	}
	if(normals){
		GX_SetArray(GX_VA_NRM, normals, sizeof(V3d));
		if(needFlush)
			DCFlushRange(normals, geo->numVertices*sizeof(V3d));
	}
	if(uv2){
		GX_SetArray(GX_VA_TEX1, uv2, sizeof(TexCoords));
		if(needFlush)
			DCFlushRange(uv2, geo->numVertices*sizeof(TexCoords));
	}
	if(prelit){
		GX_SetArray(GX_VA_CLR0, prelit, sizeof(RGBA));
		if(needFlush)
			DCFlushRange(prelit, geo->numVertices*sizeof(RGBA));
	}
	if(packUV){
		GX_SetArray(GX_VA_TEX0, (void*)packUV, 2*sizeof(int16));
		if(needFlush)
			DCFlushRange((void*)packUV, geo->numVertices*2*sizeof(int16));
	}else if(uv){
		GX_SetArray(GX_VA_TEX0, uv, sizeof(TexCoords));
		if(needFlush)
			DCFlushRange(uv, geo->numVertices*sizeof(TexCoords));
	}

	u8 prim = (geo->meshHeader->flags & MeshHeader::TRISTRIP) ?
	    GX_TRIANGLESTRIP : GX_TRIANGLES;
#if GX_WIREFRAME
	// Edges only: exposes exploded/degenerate vertices that a textured
	// fill hides. LINESTRIP over the same index stream, so a vertex with a
	// garbage position draws a long spike straight to it.
	prim = GX_LINESTRIP;
#endif

	// The atomic's matrix is the same for every mesh it owns, and with
	// normals loadWorldMtx also does a guMtxInvXpose — a full inverse per
	// call. Hoist it out of the mesh loop: one load per atomic instead of
	// one per mesh (~350 meshes/frame were paying for it).
	loadWorldMtx(world, lit2);

	Mesh *mesh = geo->meshHeader->getMeshes();
	for(uint16 m = 0; m < geo->meshHeader->numMeshes; m++, mesh++){
		if(mesh->numIndices == 0)
			continue;

		Material *mat = mesh->material;
		Texture *texture = mat ? mat->texture : nil;
		// A geometry with no texture coordinates cannot be textured. This
		// used to bind the material's texture anyway and stream UV (0,0) for
		// every vertex (see the uv ? : 0.0f fallback below), painting the
		// whole mesh with a single corner texel — black for most VC textures.
		// 165 of 235 sampled ped meshes carry gflags without TEXTURED
		// (0x11/0x13, i.e. no tex coord sets), which is what rendered
		// characters as flat black silhouettes while their textured meshes
		// (jeans, forearms) came out fine.
		// "Has texture coordinates" is the real condition, and after
		// gxPackGeometry they may live in the packed int16 array instead of
		// the float one. Testing only `uv` here drew every packed world
		// geometry untextured — a white city with correct silhouettes.
		GXTexObj *tex = (texture && (uv || packUV)) ?
		    gxGetTexture(texture->raster) : nil;
		// Tiling-alloc failed under memory pressure: untextured here means
		// vertex colours only — prelight-dark, the "textures flashing dark"
		// on stream-in while the streamer makes room. Skip the mesh for the
		// few frames the retry needs; gxTexturePending bounds the hide so a
		// texture stuck failing still draws its dark silhouette eventually.
		if(tex == nil && texture && (uv || packUV) &&
		   gxTexturePending(texture->raster))
			continue;
#if GX_UV_DEBUG
		tex = nil;  // show the raw UV colour, unmodulated by any texel
#endif
		// A texture built mid-frame leaves a stale TMEM copy, so the GP keeps
		// sampling whatever was cached at that address and the mesh draws with
		// another model's texels. drawIm2D has always honoured this flag; the
		// 3D path never did, which is why freshly streamed characters showed
		// the wrong part of their atlas (shirt texels on hands and feet) while
		// their UVs measured correct.
			if(tex){
			// world UVs repeat far past 0..1; the raster-level default is
			// CLAMP, which smears the edge texel across every near mesh.
			// (GX wrap needs pow2 dims — true for VC's textures.)
			u8 wu = texture->getAddressU() == Texture::MIRROR ? GX_MIRROR :
			    texture->getAddressU() == Texture::WRAP ? GX_REPEAT : GX_CLAMP;
			u8 wv = texture->getAddressV() == Texture::MIRROR ? GX_MIRROR :
			    texture->getAddressV() == Texture::WRAP ? GX_REPEAT : GX_CLAMP;
			GX_InitTexObjWrapMode(tex, wu, wv);
			GX_LoadTexObj(tex, GX_TEXMAP0);
		}

		RGBA matcol = mat ? mat->color : makeRGBA(255, 255, 255, 255);
		float surfAmb = mat ? mat->surfaceProps.ambient : 1.0f;
		float surfDiff = mat ? mat->surfaceProps.diffuse : 1.0f;

		// opaque-texture fast path needs the material fully opaque too,
		// or a translucent material would sneak past the alpha test
		stTexOpaque = tex != nil && texture != nil &&
		    !gxRasterHasAlpha(texture->raster) &&
		    matcol.alpha == 255 && !stVertexAlpha;

		bool32 prelitMesh = prelit != nil;
		bool32 hwLights = 0;      // CPU lighting only, see above
		bool32 sendColor = 1;     // every vertex now carries its lit color
#if GX_MARK_SKINNED
		// paint skinned atomics flat red, untextured: shows the exact
		// silhouette the skinned buffer occupies on screen
		// keep the texture, drop only the lighting: if the model comes
		// back whole and textured, the fault is the lit-channel setup for
		// skinned meshes, not texturing or the alpha test
		if(skinnedAtomic){
			prelitMesh = 0; lit2 = 0; hwLights = 0;
			matcol = makeRGBA(255, 255, 255, 255);
		}
#endif
#if GX_WIREFRAME
		tex = nil; prelitMesh = 0; lit2 = 0;
		matcol = makeRGBA(255,255,255,255);
#endif
		// Every vertex now carries a CPU-computed color, and there is no array
		// to index one out of, so the indexed path cannot apply here at all.
		// The TEV ambient konst is gone with it — ambient is in the vertex
		// color now, where gl3 puts it.
		// Indexed when the vertex colours can come from an array instead of
		// being recomputed per vertex. gxBuildColorCache below makes that
		// array for static, unskinned, unlit geometry — which is the city.
		// Behind GX_USE_INDEXED until it has been measured against the
		// immediate path on real scenes; the profiler's gp column is what
		// decides whether moving work to the GP helps or just moves the
		// bottleneck.
		bool32 useIdx = 0;
		RGBA ambK = { 0, 0, 0, 0 };
		(void)ambK;

		// Per-material light terms, precomputed once per mesh (gl3 scales the
		// ambient by surfAmbient and each directional by surfDiffuse).
		float ambR = 0.0f, ambG = 0.0f, ambB = 0.0f;
		if(lit){
			ambR = lights.ambient.red   * surfAmb;
			ambG = lights.ambient.green * surfAmb;
			ambB = lights.ambient.blue  * surfAmb;
		}
		float matR = matcol.red/255.0f, matG = matcol.green/255.0f;
		float matB = matcol.blue/255.0f, matA = matcol.alpha/255.0f;
		// 8-bit forms of the same per-mesh terms, for the integer vertex
		// colour path below. Every input is already 8-bit; only the
		// arithmetic was float.
		int ambR8 = clamp255i((int)(ambR*255.0f + 0.5f));
		int ambG8 = clamp255i((int)(ambG*255.0f + 0.5f));
		int ambB8 = clamp255i((int)(ambB*255.0f + 0.5f));
#if GX_PROBE_SKIN
		// Peds are the only geometry that reaches the GX hardware-lighting
		// path: world geometry has no normals, so it takes the CPU ambient
		// branch and its prelight carries the shading. That makes "peds are
		// black" a question about exactly four inputs — material colour, the
		// ambient register, the light colours, and the texel itself. Dump
		// them once per skinned mesh, bounded, instead of A/B-ing four builds.
		// Fires for any atomic carrying a Skin, not just successfully skinned
		// ones: an empty log then means "no ped geometry reached the renderer
		// at all", while skinned=0 lines mean the CPU skin path was rejected
		// (hierarchy/bone mismatch). Without that split an empty log is
		// ambiguous and costs a whole boot to disambiguate.
		// Sample every mesh of one atomic per second, not one mesh per second:
		// a ped is many meshes and only some render black, so a single
		// arbitrary mesh per sample can't show which material is at fault.
		// Latching the decision per atomic (m == 0) logs the whole breakdown
		// of one character. Spacing by time makes the log span the intro
		// cutscene *and* gameplay — the first N meshes are all cutscene, whose
		// night lighting is legitimately near-zero.
		static unsigned long long tProbe;
		static bool32 probeThisAtomic;
		if(m == 0)
			probeThisAtomic = tProbe == 0 ||
			    ticks_to_millisecs(gettime() - tProbe) >= 1000;
		if(skin && gxProbeLeft > 0 && probeThisAtomic){
			if(m == 0)
				tProbe = gettime();
			gxProbeLeft--;
			uint32 gxFmt = 0xFF, firstWord = 0;
			if(texture)
				gxRasterProbe(texture->raster, &gxFmt, &firstWord);
			char line[320];
			snprintf(line, sizeof(line),
			    "SKIN m=%d tx=%s skinned=%d geo=%p mat=%02x%02x%02x%02x amb=%.2f dif=%.2f "
			    "nDir=%d mask=%02x ambL=%.2f,%.2f,%.2f dir0=%.2f,%.2f,%.2f "
			    "hw=%d prelit=%d tex=%d fmt=%02x w0=%08x nv=%d nb=%d hier=%d "
			    "lit=%d lit2=%d send=%d gflags=%08x world=%d",
			    (int)m, texture ? texture->name : "-",
			    (int)skinnedAtomic,
			    (void*)geo, matcol.red, matcol.green, matcol.blue,
			    matcol.alpha, surfAmb, surfDiff, (int)numDir, lightMask,
			    lights.ambient.red, lights.ambient.green, lights.ambient.blue,
			    numDir > 0 ? dirCol[0].red : 0.0f,
			    numDir > 0 ? dirCol[0].green : 0.0f,
			    numDir > 0 ? dirCol[0].blue : 0.0f,
			    (int)hwLights, (int)prelitMesh, tex != nil,
			    (unsigned)gxFmt, (unsigned)firstWord, (int)geo->numVertices,
			    (int)skin->numBones, hier ? (int)hier->numNodes : -1,
			    (int)lit, (int)lit2, (int)sendColor,
			    (unsigned)geo->flags, rwworld != nil);
			// SD file, not GeckoLog: Dolphin's emulated gecko truncates
			// anything longer than a short line (every heartbeat in the
			// capture is cut mid-string), which would silently shred exactly
			// the fields being probed. libfat only commits on fclose, so
			// open/append/close per line — bounded, so the cost is bounded.
			DVD_FS_GUARD;
			FILE *f = fopen("dvd:/skin.log", "a");
			if(f){
				fprintf(f, "%s\n", line);
				fclose(f);
			}
			GeckoLog("SKIN probe written");
		}
#endif
		// Material color, ambient and lights are all folded into the vertex
		// color above, so none of them reach the GP any more. Passing white
		// and no lights keeps the setup identical across every material in a
		// run, which is what lets the memo skip the ~30 register writes.
		// Build the colour cache even when staying immediate. The expression
		// is the same one the per-vertex code computes, and it only changes
		// with the ambient or the material — so computing it once per mesh and
		// reading it per vertex removes six integer ops and four clamps from
		// every vertex of every frame, with no change to what reaches the GP.
		// This is the safe half of the indexed work: same descriptor, same
		// bytes, just not recomputed.
		const RGBA *cachedCol = nil;
		if(!skinnedAtomic &&
		   gxBuildColorCache(geo, gpk, prelit, numDir,
		       ambR8, ambG8, ambB8, matcol))
			cachedCol = gpk->colors;

#if GX_USE_INDEXED
		// All three attributes have to be arrays for this to be legal: packed
		// positions and texcoords from gxPackGeometry, colours from the cache.
		// Any one missing and the vertex stream would disagree with the
		// descriptor, which desyncs the FIFO rather than drawing wrong.
		if(!skinnedAtomic && packPos && (tex == nil || packUV) &&
		   gxBuildColorCache(geo, gpk, prelit, numDir,
		       ambR8, ambG8, ambB8, matcol))
			useIdx = 1;
#endif
		// Arm the env/rim stage for this mesh. Only when the geometry still
		// owns float normals: packed world geometry freed its float arrays
		// and never had normals to begin with, and the descriptor/emission
		// pair below must agree with setup3DDraw about GX_VA_NRM.
		// The blend trigger the other backends compute per mesh
		// (inst->vertexAlpha || m->color.alpha != 255). The material colour
		// is baked into the vertex colours on this path, so the flag is the
		// only surviving record that this mesh is translucent — the pink
		// mission marker (zonecylb.dff, RpMaterialSetColor alpha) is the
		// visible case.
		stMaterialAlpha = mat != nil && mat->color.alpha != 255;
		gxEnvTex = nil;
		gxEnvUV2 = 0;
		MatFX *mfx = mat ? MatFX::get(mat) : nil;
		if(normals && !packPos){
			if(mfx && (mfx->type == MatFX::ENVMAP ||
			           mfx->type == MatFX::BUMPENVMAP)){
				Texture *envt = mfx->getEnvTexture();
				float ec = mfx->getEnvCoefficient();
				if(envt && envt->raster && ec > 0.02f){
					GXTexObj *eo = gxGetTexture(envt->raster);
					if(eo){
						u8 e8 = ec >= 1.0f ? 255 : (u8)(ec*255.0f);
						gxEnvTex = eo;
						gxEnvK = (GXColor){ e8, e8, e8, 255 };
					}
				}
			}
			// ROAD GLOSS: the neo gloss texture for this material, spheremapped
			// by the vertex normal — view-dependent shine, additive. Same
			// constraint as the PC pipe: no normals, no gloss.
			if(gxEnvTex == nil && gxGlossEnable && gxGlossLookup && mat && mat->texture){
				Texture *gt = gxGlossLookup(mat);
				if(gt && gt->raster){
					GXTexObj *go = gxGetTexture(gt->raster);
					if(go){
						float gm = gxGlossMult;
						if(gm > 1.0f) gm = 1.0f;
						if(gm < 0.0f) gm = 0.0f;
						u8 g8 = (u8)(gm*255.0f);
						gxEnvTex = go;
						gxEnvK = (GXColor){ g8, g8, g8, 255 };
					}
				}
			}
			if(gxEnvTex == nil && gxRimEnable && skinnedAtomic){
				// Bind-pose normals on an animated ped make the rim swim a
				// little; a soft cool rim reads right regardless.
				GXTexObj *ro = gxRimRamp();
				if(ro){
					gxEnvTex = ro;
					gxEnvK = (GXColor){ 90, 110, 140, 255 };
				}
			}
		}
		// WORLD LIGHTMAPS: dual-pass matfx texture through UV set 1, scaled
		// by the time-of-day blend the game interpolates from the neo table.
		if(gxEnvTex == nil && gxLightmapEnable && uv2 && mfx &&
		   mfx->type == MatFX::DUAL){
			Texture *dt = mfx->getDualTexture();
			if(dt && dt->raster){
				GXTexObj *dob = gxGetTexture(dt->raster);
				float lb = gxLightmapBlend;
				if(lb > 1.0f) lb = 1.0f;
				if(lb < 0.0f) lb = 0.0f;
				u8 l8 = (u8)(lb*255.0f);
				if(dob && l8 > 2){
					gxEnvTex = dob;
					gxEnvUV2 = 1;
					gxEnvK = (GXColor){ l8, l8, l8, 255 };
				}
			}
		}
		bool32 sendNrm = gxEnvTex != nil && !gxEnvUV2;
		bool32 sendUV2 = gxEnvTex != nil && gxEnvUV2;
		setup3DDraw(tex != nil, 0, 1, 0,
		    makeRGBA(255, 255, 255, 255), 1.0f, nil, GX_LIGHTNULL,
		    sendColor, useIdx, ambK, posFrac, uvFrac);
		if(useIdx){
			GX_SetArray(GX_VA_POS, (void*)packPos, 3*sizeof(int16));
			GX_SetArray(GX_VA_CLR0, gpk->colors, sizeof(RGBA));
			if(tex)
				GX_SetArray(GX_VA_TEX0, (void*)packUV, 2*sizeof(int16));
			// The GP's vertex cache is keyed by INDEX. Rebinding the arrays
			// does not clear it, so without this the next mesh's index 5 can
			// fetch the PREVIOUS mesh's vertex 5 straight from the cache —
			// chunks of one model drawn with another's positions, on screen
			// as geometry and people teleporting. Reported by the user within
			// hours of GX_USE_INDEXED going on.
			GX_InvVtxCache();
		}
		if(skinnedAtomic){
			// Character rigs mirror their left-side bones, so those bone
			// matrices carry a negative determinant and flip the winding
			// of every triangle they move. Backface culling then eats the
			// visible side of exactly those limbs — the model reads as
			// half-occluded, and only on animated humanoids. Draw skinned
			// meshes double-sided, which is what the fixed-function era
			// did for characters anyway.
			GX_SetCullMode(GX_CULL_NONE);
			gx3DMemoInvalidate();
		}

		// GX_Begin takes a u16 count; big merged world meshes exceed it and
		// a wrapped count desyncs the FIFO (geometry teleports). Chunk:
		// strips overlap 2 indices at an even boundary to keep winding,
		// lists split at triangle boundaries.
		uint32 total = mesh->numIndices;
		uint32 maxChunk = prim == GX_TRIANGLESTRIP ? 65534 : 65535 - 65535%3;

		// Replay a recorded list when we have one. Only static geometry
		// qualifies: prelit (its vertex colors never change — the
		// time-of-day ambient rides the TEV konst instead) and unskinned
		// (skinned vertices are rebuilt every frame by definition).
		GxGeoExt *gext = PLUGINOFFSET(GxGeoExt, geo, gxGeoOffset);
		// GX_DL_BUDGET 0 disables caching entirely — and it must also skip
		// the per-geometry bookkeeping arrays, or every geometry still
		// callocs one it will never use. That silent drain starved the
		// texture allocator until gxGetTexture returned nil and the whole
		// world drew untextured white.
		bool32 canCache = GX_DL_BUDGET > 0 && prelitMesh &&
		    !skinnedAtomic && !useIdx;
		if(canCache && gext->lists == nil){
			int32 n = geo->meshHeader->numMeshes;
			gext->lists = (void**)calloc(n, sizeof(void*));
			gext->sizes = (uint32*)calloc(n, sizeof(uint32));
			gext->numLists = gext->lists && gext->sizes ? n : 0;
			if(!gext->numLists){
				free(gext->lists); free(gext->sizes);
				gext->lists = nil; gext->sizes = nil;
			}
		}
		bool32 recording = 0;
		if(canCache && gext->numLists > (int32)m){
			if(gext->lists[m]){
				gxMeshCount++;
				gxDlMeshCount++;
				gxVertCount += total;
				GX_CallDispList(gext->lists[m], gext->sizes[m]);
				continue;
			}
			// vertex is pos + colour + optional uv/normal, plus GX_Begin
			uint32 vsz = 12 + 4 + (tex ? 8 : 0) + (hwLights ? 12 : 0);
			uint32 cap = total*vsz + 3*(total/maxChunk + 2) + 64;
			cap = (cap + 31) & ~31u;
			if(gxDlBytes + cap <= GX_DL_BUDGET){
				void *buf = memalign(32, cap);
				if(buf){
					DCInvalidateRange(buf, cap); // WGP bypasses the cache
					GX_BeginDispList(buf, cap);
					recording = 1;
					gext->lists[m] = buf;
				}
			}
		}

		gxLastPath = skinnedAtomic ? "3d-skin" : "3d";
		gxLast.path = skinnedAtomic ? "3d-skin" : "3d";
		gxLast.texName = texture ? texture->name : nil;
		gxLast.texRaster = texture ? texture->raster : nil;
		gxLast.texW = texture && texture->raster ? texture->raster->width : 0;
		gxLast.texH = texture && texture->raster ? texture->raster->height : 0;
		gxLast.hasTex = tex != nil;
		gxLast.prim = prim;
		gxLast.vtxfmt = 1;
		gxLast.count = total;
		gxLast.posFrac = posFrac;
		gxLast.uvFrac = uvFrac;

		uint32 start = 0;
		while(start < total){
			uint32 count = total - start;
			if(count > maxChunk)
				count = maxChunk;
			gxMeshCount++;
			gxVertCount += count;
			GX_Begin(prim, GX_VTXFMT1, count);
			if(useIdx){
				for(uint32 i = start; i < start + count; i++){
					uint16 vi = mesh->indices[i];
					GX_Position1x16(vi);
					if(hwLights || sendNrm)
						GX_Normal1x16(vi);
					GX_Color1x16(vi);
					if(tex)
						GX_TexCoord1x16(vi);
					if(sendUV2)
						GX_TexCoord1x16(vi);
				}
				GX_End();
				if(start + count >= total)
					break;
				start += prim == GX_TRIANGLESTRIP ? count - 2 : count;
				continue;
			}
			for(uint32 i = start; i < start + count; i++){
				uint16 vi = mesh->indices[i];
				// Half the position bytes through the write-gather pipe, and
				// the GP applies posFrac itself — no unpacking here.
				if(packPos){
					const int16 *p = packPos + 3*vi;
					GX_Position3s16(p[0], p[1], p[2]);
				}else
					GX_Position3f32(verts[vi].x, verts[vi].y, verts[vi].z);
				// MUST be hwLights, not lit2: the descriptor only declares
				// NRM when hardware lighting is actually on. A lit mesh with
				// normals but zero enumerated directionals (night, or a
				// geometry without the NORMALS flag) would otherwise push an
				// undeclared attribute and desync the FIFO — garbage geometry.
				if(hwLights || sendNrm)
					GX_Normal3f32(normals[vi].x, normals[vi].y,
					    normals[vi].z);
				{
#if !GX_WIREFRAME && !GX_UV_DEBUG
				// With no directional contributing, the whole expression is
				// clamp(prelight + ambient) * material, and every term in it
				// is already 8-bit — only the arithmetic was float. That
				// matters more here than it looks: Gekko has no direct path
				// between the float and integer registers, so each (u8)(float)
				// is a store and a reload through memory, and the float
				// version pays eight of them per vertex. World geometry has no
				// normals, so this is the path essentially the whole city
				// takes. Measured as the difference between work17 (a missed
				// retrace, 30fps) and fitting inside 16.6ms again.
				if(numDir == 0){
					if(cachedCol){
						const RGBA *c = &cachedCol[vi];
						GX_Color4u8(c->red, c->green, c->blue, c->alpha);
					}else{
						int r8 = ambR8, g8 = ambG8, b8 = ambB8, a8 = 255;
						if(prelitMesh){
							r8 += prelit[vi].red;
							g8 += prelit[vi].green;
							b8 += prelit[vi].blue;
							a8 = prelit[vi].alpha;
						}
						GX_Color4u8(
						    (u8)mul255(clamp255i(r8), matcol.red),
						    (u8)mul255(clamp255i(g8), matcol.green),
						    (u8)mul255(clamp255i(b8), matcol.blue),
						    (u8)mul255(a8, matcol.alpha));
					}
					if(tex){
						if(packUV)
							GX_TexCoord2s16(packUV[2*vi], packUV[2*vi+1]);
						else
							GX_TexCoord2f32(uv ? uv[vi].u : 0.0f,
							    uv ? uv[vi].v : 0.0f);
					}
					if(sendUV2)
						GX_TexCoord2f32(uv2[vi].u, uv2[vi].v);
					continue;
				}
#endif
					// librw gl3 default.vert, evaluated on the CPU:
					//   c  = prelight            (else (0,0,0,1))
					//   c += amb*surfAmbient
					//   c += sum(max(0,N.-L))*lightCol*surfDiffuse
					//   c  = clamp(c,0,1) * matColor
					float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
#if GX_WIREFRAME
					GX_Color4u8(255,255,255,255);
					if(tex)
						GX_TexCoord2f32(0.0f, 0.0f);
					continue;
#endif
					if(prelitMesh){
						r = prelit[vi].red/255.0f;
						g = prelit[vi].green/255.0f;
						b = prelit[vi].blue/255.0f;
						a = prelit[vi].alpha/255.0f;
					}
					r += ambR; g += ambG; b += ambB;
					for(int32 k = 0; k < numDir; k++){
						V3d *n = &normals[vi];
						float l = -(n->x*objDir[k].x + n->y*objDir[k].y +
						    n->z*objDir[k].z);
						if(l <= 0.0f)
							continue;
						l *= surfDiff;
						r += l*dirCol[k].red;
						g += l*dirCol[k].green;
						b += l*dirCol[k].blue;
					}
					if(r > 1.0f) r = 1.0f;
					if(g > 1.0f) g = 1.0f;
					if(b > 1.0f) b = 1.0f;
					if(a > 1.0f) a = 1.0f;
#if GX_UV_DEBUG
					// Paint the UV the GP is actually being handed: red = u,
					// green = v. The atlas is known-good and the UVs in the
					// DFF measure u[0.003..0.999] v[0.000..1.000] with none
					// outside range, so if the model comes back as a smooth,
					// body-part-coherent gradient the UVs survive the trip and
					// the fault is sampling; if it is scrambled or flat, the
					// vertex stream is wrong.
					if(uv){
						float du = uv[vi].u, dv = uv[vi].v;
						du = du < 0.0f ? 0.0f : du > 1.0f ? 1.0f : du;
						dv = dv < 0.0f ? 0.0f : dv > 1.0f ? 1.0f : dv;
						GX_Color4u8((u8)(du*255.0f), (u8)(dv*255.0f), 0, 255);
					}else
						GX_Color4u8(0, 0, 255, 255);
#else
					GX_Color4u8((u8)(r*matR*255.0f), (u8)(g*matG*255.0f),
					    (u8)(b*matB*255.0f), (u8)(a*matA*255.0f));
#endif
				}
				if(tex){
					if(packUV)
						GX_TexCoord2s16(packUV[2*vi], packUV[2*vi+1]);
					else
						GX_TexCoord2f32(uv ? uv[vi].u : 0.0f,
						    uv ? uv[vi].v : 0.0f);
					if(sendUV2)
						GX_TexCoord2f32(uv2[vi].u, uv2[vi].v);
				}
			}
			GX_End();
			if(start + count >= total)
				break;
			// strip chunks re-emit the last two indices
			start += prim == GX_TRIANGLESTRIP ? count - 2 : count;
		}
		if(recording){
			uint32 used = GX_EndDispList();
			if(used == 0){
				free(gext->lists[m]);
				gext->lists[m] = nil;
			}else{
				gext->sizes[m] = used;
				gxDlBytes += used;
				GX_CallDispList(gext->lists[m], used);
			}
		}
	}
}

static void
atomicNopCB(ObjPipeline*, Atomic*)
{
}

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->impl.instance = atomicNopCB;
	pipe->impl.uninstance = atomicNopCB;
	pipe->impl.render = atomicRenderCB;
	return pipe;
}

static int
deviceSystem(DeviceReq req, void *arg0, int32 n)
{
	switch(req){
	case DEVICEOPEN:
		startGX();
		return 1;
	case DEVICECLOSE:
		stopGX();
		return 1;
	case DEVICEINIT:
	case DEVICETERM:
	case DEVICEFINALIZE:
		return 1;
	// The ANTI ALIASING option. Real EFB multisampling is a different pixel
	// format (RGB565_Z16, field-rendered) and would cost the dest-alpha the
	// screen effects use, so the togglable level drives the copy-out
	// smoothing filter instead — the hardware's own edge filter, applied for
	// free during the EFB->XFB copy.
	case DEVICEGETMAXMULTISAMPLINGLEVELS:
		return 2;
	case DEVICEGETMULTISAMPLINGLEVELS:
		return gxCopyFilterLevel ? 2 : 1;
	case DEVICESETMULTISAMPLINGLEVELS:
		gxCopyFilterLevel = n > 1 ? 1 : 0;
		return 1;

	case DEVICEGETNUMSUBSYSTEMS:
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
		return 0;
	case DEVICESETSUBSYSTEM:
		return 1;
	case DEVICEGETSUBSSYSTEMINFO:
		if(arg0){
			SubSystemInfo *info = (SubSystemInfo*)arg0;
			strncpy(info->name, "GameCube GX", sizeof(info->name)-1);
			info->name[sizeof(info->name)-1] = '\0';
		}
		return 1;

	case DEVICEGETNUMVIDEOMODES:
		return 1;
	case DEVICEGETCURRENTVIDEOMODE:
		return 0;
	case DEVICESETVIDEOMODE:
		return 1;
	case DEVICEGETVIDEOMODEINFO:
		if(arg0){
			VideoMode *mode = (VideoMode*)arg0;
			startGX();
			mode->width = rmode->fbWidth;
			mode->height = rmode->efbHeight;
			mode->depth = 32;
			mode->flags = VIDEOMODEEXCLUSIVE;
		}
		return 1;

	default:
		break;
	}
	return 1;
}

Device renderdevice = {
	0.0f, 1.0f,
	gx::beginUpdate,
	gx::endUpdate,
	gx::clearCamera,
	gx::showRaster,
	gx::rasterRenderFast,
	gx::setRenderState,
	gx::getRenderState,
	gx::im2DRenderLine,
	gx::im2DRenderTriangle,
	gx::im2DRenderPrimitive,
	gx::im2DRenderIndexedPrimitive,
	gx::im3DTransform,
	gx::im3DRenderPrimitive,
	gx::im3DRenderIndexedPrimitive,
	gx::im3DEnd,
	gx::deviceSystem
};

}
}

#endif
