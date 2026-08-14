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

#ifdef RW_GAMECUBE

#include <gccore.h>
#include <malloc.h>

namespace rw {
namespace gx {

#define FIFO_SIZE (256*1024)

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

	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
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
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,
	    ((rmode->viHeight == 2*rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

	GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
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
static void
setupIm2DVtxDesc(bool32 textured)
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	if(textured){
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
		GX_SetNumTexGens(1);
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	}else{
		GX_SetNumTexGens(0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	}
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
	    GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	// 2D always blends by vertex/texture alpha and ignores depth.
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
}

// Set an orthographic projection matching the 2D screen space librw hands us.
static void
setIm2DMatrices(void)
{
	Mtx44 proj;
	Mtx mv;

	guOrtho(proj, 0, rmode->efbHeight, 0, rmode->fbWidth, -1.0f, 1.0f);
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
loadWorldMtx(Matrix *world)
{
	Mtx w, mv;
	if(world){
		rwToGxMtx(w, world);
		guMtxConcat(gxView, w, mv);
	}else
		guMtxCopy(gxView, mv);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
}

static void
beginUpdate(Camera *cam)
{
	startGX();
	// covers textures (re)built since last frame; per-texture invalidation
	// at stream time would fill the FIFO with nothing draining it
	GX_InvalidateTexAll();
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);

	// View: inverse of the camera frame, transposed into GX layout, Z negated.
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	gxView[0][0] = inv.right.x; gxView[0][1] = inv.up.x; gxView[0][2] = inv.at.x; gxView[0][3] = inv.pos.x;
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
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	startGX();
	if(mode & Camera::CLEARIMAGE){
		clearColor.r = col->red;
		clearColor.g = col->green;
		clearColor.b = col->blue;
		clearColor.a = col->alpha;
		GX_SetCopyClear(clearColor, GX_MAX_Z24);
	}
}

static void
showRaster(Raster *raster, uint32 flags)
{
	startGX();

	GX_DrawDone();
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);

	currentXfb ^= 1;
	GX_CopyDisp(xfb[currentXfb], GX_TRUE);
	GX_Flush();

	VIDEO_SetNextFramebuffer(xfb[currentXfb]);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

static bool32
rasterRenderFast(Raster*, int32, int32)
{
	return 0;
}

// im2D texture, set by the game through rwRENDERSTATETEXTURERASTER.
static Raster *currentTexRaster;
GXTexObj *gxGetTexture(Raster *raster);

static void
setRenderState(int32 state, void *pvalue)
{
	if(!gxStarted)
		return;
	uint32 value = (uint32)(uintptr)pvalue;

	switch(state){
	case TEXTURERASTER:
		currentTexRaster = (Raster*)pvalue;
		break;
	case ZTESTENABLE:
		GX_SetZMode(value ? GX_TRUE : GX_FALSE, GX_LEQUAL, GX_TRUE);
		break;
	case ZWRITEENABLE:
		GX_SetZMode(GX_TRUE, GX_LEQUAL, value ? GX_TRUE : GX_FALSE);
		break;
	case CULLMODE:
		GX_SetCullMode(value == CULLNONE ? GX_CULL_NONE :
		    value == CULLBACK ? GX_CULL_BACK : GX_CULL_FRONT);
		break;
	case VERTEXALPHA:
	case SRCBLEND:
	case DESTBLEND:
		GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA,
		    GX_LO_CLEAR);
		break;
	default:
		break;
	}
}

static void*
getRenderState(int32 state)
{
	if(state == TEXTURERASTER)
		return currentTexRaster;
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

	u8 prim;
	switch(primType){
	case PRIMTYPETRILIST:   prim = GX_TRIANGLES; break;
	case PRIMTYPETRISTRIP:  prim = GX_TRIANGLESTRIP; break;
	case PRIMTYPETRIFAN:    prim = GX_TRIANGLEFAN; break;
	case PRIMTYPELINELIST:  prim = GX_LINES; break;
	case PRIMTYPEPOLYLINE:  prim = GX_LINESTRIP; break;
	default: return;
	}

	GXTexObj *tex = gxGetTexture(currentTexRaster);
	if(tex)
		GX_LoadTexObj(tex, GX_TEXMAP0);

	setupIm2DVtxDesc(tex != nil);
	setIm2DMatrices();

	GX_Begin(prim, GX_VTXFMT0, count);
	for(int32 i = 0; i < count; i++){
		Im2DVertex *v = &verts[indices ? idx[i] : i];
		GX_Position3f32(v->x, v->y, 0.0f);
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

// 3D vertex setup: position, colour, optional texcoord, depth on.
static void
setup3DVtxDesc(bool32 textured)
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	if(textured){
		GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
		GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
		GX_SetNumTexGens(1);
		GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
		GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	}else{
		GX_SetNumTexGens(0);
		GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	}
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
	    GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_LoadProjectionMtx(gxProj, gxProjType);
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

// Default atomic pipeline: immediate-mode walk of the generic geometry.
// ponytail: CPU-fed vertices every frame; the upgrade path is cached GX
// display lists per mesh when this becomes the bottleneck.
static void
atomicRenderCB(ObjPipeline *pipe, Atomic *atomic)
{
	(void)pipe;
	Geometry *geo = atomic->geometry;
	if(geo == nil || geo->flags & Geometry::NATIVE ||
	   geo->meshHeader == nil || geo->morphTargets == nil ||
	   !gxStarted || !gxHaveCamera)
		return;

	V3d *verts = geo->morphTargets[0].vertices;
	if(verts == nil)
		return;
	TexCoords *uv = geo->numTexCoordSets > 0 ? geo->texCoords[0] : nil;
	RGBA *prelit = (geo->flags & Geometry::PRELIT) ? geo->colors : nil;

	Matrix *world = atomic->getFrame()->getLTM();
	u8 prim = (geo->meshHeader->flags & MeshHeader::TRISTRIP) ?
	    GX_TRIANGLESTRIP : GX_TRIANGLES;

	Mesh *mesh = geo->meshHeader->getMeshes();
	for(uint16 m = 0; m < geo->meshHeader->numMeshes; m++, mesh++){
		if(mesh->numIndices == 0)
			continue;

		Texture *texture = mesh->material ? mesh->material->texture : nil;
		GXTexObj *tex = texture ? gxGetTexture(texture->raster) : nil;
		if(tex)
			GX_LoadTexObj(tex, GX_TEXMAP0);

		RGBA matcol = mesh->material ? mesh->material->color :
		    makeRGBA(255, 255, 255, 255);

		setup3DVtxDesc(tex != nil);
		loadWorldMtx(world);

		GX_Begin(prim, GX_VTXFMT1, mesh->numIndices);
		for(uint32 i = 0; i < mesh->numIndices; i++){
			uint16 vi = mesh->indices[i];
			GX_Position3f32(verts[vi].x, verts[vi].y, verts[vi].z);
			if(prelit)
				GX_Color4u8(prelit[vi].red, prelit[vi].green,
				    prelit[vi].blue, prelit[vi].alpha);
			else
				GX_Color4u8(matcol.red, matcol.green, matcol.blue,
				    matcol.alpha);
			if(tex)
				GX_TexCoord2f32(uv ? uv[vi].u : 0.0f,
				    uv ? uv[vi].v : 0.0f);
		}
		GX_End();
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
