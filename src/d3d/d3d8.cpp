#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwd3d.h"
#include "rwd3d8.h"

#include "rwd3dimpl.h"

#define PLUGIN_ID 2

namespace rw {
namespace d3d8 {
using namespace d3d;

static void*
driverOpen(void *o, int32, int32)
{
	engine->driver[PLATFORM_D3D8]->defaultPipeline = makeDefaultPipeline();

	engine->driver[PLATFORM_D3D8]->rasterNativeOffset = nativeRasterOffset;
	engine->driver[PLATFORM_D3D8]->rasterCreate       = rasterCreate;
	engine->driver[PLATFORM_D3D8]->rasterLock         = rasterLock;
	engine->driver[PLATFORM_D3D8]->rasterUnlock       = rasterUnlock;
	engine->driver[PLATFORM_D3D8]->rasterNumLevels    = rasterNumLevels;
	engine->driver[PLATFORM_D3D8]->imageFindRasterFormat = imageFindRasterFormat;
	engine->driver[PLATFORM_D3D8]->rasterFromImage    = rasterFromImage;
	engine->driver[PLATFORM_D3D8]->rasterToImage      = rasterToImage;
	return o;
}

static void*
driverClose(void *o, int32, int32)
{
	return o;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_D3D8, 0, PLATFORM_D3D8,
	                       driverOpen, driverClose);
	// shared between D3D8 and 9
	if(nativeRasterOffset == 0)
		registerNativeRaster();
}

uint32
makeFVFDeclaration(uint32 flags, int32 numTex)
{
	uint32 fvf = 0x2;
	if(flags & Geometry::NORMALS)
		fvf |= 0x10;
	if(flags & Geometry::PRELIT)
		fvf |= 0x40;
	fvf |= numTex << 8;
	return fvf;
}

int32
getStride(uint32 flags, int32 numTex)
{
	int32 stride = 12;
	if(flags & Geometry::NORMALS)
		stride += 12;;
	if(flags & Geometry::PRELIT)
		stride += 4;
	stride += numTex*8;
	return stride;
}

void*
destroyNativeData(void *object, int32, int32)
{
	Geometry *geometry = (Geometry*)object;
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D8)
		return object;
	InstanceDataHeader *header =
		(InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		if(inst->indexBuffer)
			destroyIndexBuffer(inst->indexBuffer);
		if(inst->vertexBuffer)
			destroyVertexBuffer(inst->vertexBuffer);
		inst++;
	}
	rwFree(header->inst);
	rwFree(header);
	return object;
}

static void
convertVertexData(uint8 *vertices, int32 numVertices, int32 stride,
                  uint32 flags, int32 numTexCoords, bool toLittleEndian)
{
	for(int32 i = 0; i < numVertices; i++){
		uint8 *vertex = &vertices[i*stride];
		if(toLittleEndian)
			memLittle32(vertex, 12);
		else
			memNative32(vertex, 12);
		vertex += 12;
		if(flags & Geometry::NORMALS){
			if(toLittleEndian)
				memLittle32(vertex, 12);
			else
				memNative32(vertex, 12);
			vertex += 12;
		}
		if(flags & Geometry::PRELIT)
			vertex += 4;
		if(toLittleEndian)
			memLittle32(vertex, numTexCoords*8);
		else
			memNative32(vertex, numTexCoords*8);
	}
}

Stream*
readNativeData(Stream *stream, int32 len, void *object, int32, int32)
{
	Geometry *geometry = (Geometry*)object;
	uint32 platform;
	if(len < 24)
		return nil;
	uint32 start = stream->tell();
	uint32 structSize;
	if(start == UINT32_MAX || !findChunk(stream, ID_STRUCT, &structSize, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	if(stream->tell() != start + 12 || structSize != (uint32)len - 12)
		return nil;
	uint8 nativeHeader[8];
	if(stream->read8(nativeHeader, sizeof(nativeHeader)) != sizeof(nativeHeader))
		return nil;
	platform = readLE32(nativeHeader);
	if(platform != PLATFORM_D3D8){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	int32 size = (int32)readLE32(nativeHeader + 4);
	if(size < 4 || (uint32)size > structSize - 8)
		return nil;
	uint8 *data = rwMallocT(uint8, size, MEMDUR_FUNCTION | ID_GEOMETRY);
	if(data == nil)
		return nil;
	if(stream->read8(data, size) != (uint32)size){
		rwFree(data);
		return nil;
	}
	uint8 *p = data;
	uint16 serialNumber = readLE16(p); p += 2;
	uint16 numMeshes = readLE16(p); p += 2;
	if(numMeshes == 0 || (uint32)size != 4 + (uint32)numMeshes*0x2C ||
	   geometry->meshHeader == nil || numMeshes != geometry->meshHeader->numMeshes){
		rwFree(data);
		return nil;
	}

	uint32 expectedStride = getStride(geometry->flags, geometry->numTexCoordSets);
	uint32 expectedVertexShader = makeFVFDeclaration(geometry->flags, geometry->numTexCoordSets);
	uint64 totalSize = 20 + (uint32)size;
	p = data + 4;
	Mesh *mesh = geometry->meshHeader->getMeshes();
	for(uint32 i = 0; i < numMeshes; i++){
		uint32 minVert = readLE32(p); p += 4;
		int32 stride = (int32)readLE32(p); p += 4;
		int32 numVertices = (int32)readLE32(p); p += 4;
		int32 numIndices = (int32)readLE32(p); p += 4;
		uint32 matid = readLE32(p); p += 4;
		uint32 vertexShader = readLE32(p); p += 4;
		uint32 primType = readLE32(p); p += 4;
		p += 16;
		uint64 vertexBytes = (uint64)(uint32)stride*(uint32)numVertices;
		uint64 indexBytes = (uint64)(uint32)numIndices*2;
		if(stride != (int32)expectedStride || vertexShader != expectedVertexShader ||
		   numVertices < 0 || numIndices < 0 || geometry->matList.numMaterials < 0 ||
		   matid >= (uint32)geometry->matList.numMaterials ||
		   (uint32)numIndices != mesh[i].numIndices ||
		   geometry->matList.materials[matid] != mesh[i].material ||
		   primType != (geometry->meshHeader->flags == MeshHeader::TRISTRIP ?
		               D3DPT_TRIANGLESTRIP : D3DPT_TRIANGLELIST) ||
		   geometry->numVertices < 0 ||
		   (uint64)minVert + (uint32)numVertices > (uint32)geometry->numVertices ||
		   vertexBytes > 0xFFFFFFFF || totalSize + indexBytes + vertexBytes > (uint32)len){
			rwFree(data);
			return nil;
		}
		totalSize += indexBytes + vertexBytes;
	}
	if(totalSize != (uint32)len){
		rwFree(data);
		return nil;
	}

	InstanceDataHeader *header = rwMallocT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	if(header == nil){
		rwFree(data);
		return nil;
	}
	header->platform = PLATFORM_D3D8;
	header->serialNumber = serialNumber;
	header->numMeshes = numMeshes;
	header->inst = rwMallocT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);
	if(header->inst == nil){
		rwFree(header);
		rwFree(data);
		return nil;
	}
	memset(header->inst, 0, header->numMeshes*sizeof(InstanceData));
	geometry->instData = header;

	InstanceData *inst = header->inst;
	p = data + 4;
	for(uint32 i = 0; i < header->numMeshes; i++){
		inst->minVert = readLE32(p); p += 4;
		inst->stride = (int32)readLE32(p); p += 4;
		inst->numVertices = (int32)readLE32(p); p += 4;
		inst->numIndices = (int32)readLE32(p); p += 4;
		uint32 matid = readLE32(p); p += 4;
		inst->material = geometry->matList.materials[matid];
		inst->vertexShader = readLE32(p); p += 4;
		inst->primType = readLE32(p); p += 4;
		inst->indexBuffer = nil; p += 4;
		inst->vertexBuffer = nil; p += 4;
		inst->baseIndex = 0; p += 4;
		inst->vertexAlpha = *p++;
		inst->managed = 0; p++;
		inst->remapped = 0; p++;	// TODO: really unused? and what's that anyway?
		p++;
		inst++;
	}
	rwFree(data);

	inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		uint32 indexBytes = (uint32)inst->numIndices*2;
		uint32 vertexBytes = (uint32)inst->stride*(uint32)inst->numVertices;
		assert(inst->indexBuffer == nil);
		inst->indexBuffer = createIndexBuffer(indexBytes, false);
		if(inst->indexBuffer == nil){
			destroyNativeData(geometry, 0, 0);
			return nil;
		}
		uint16 *indices = lockIndices(inst->indexBuffer, 0, 0, 0);
		if(indices == nil){
			destroyNativeData(geometry, 0, 0);
			return nil;
		}
		if(stream->read16(indices, indexBytes) != indexBytes){
			unlockIndices(inst->indexBuffer);
			destroyNativeData(geometry, 0, 0);
			return nil;
		}
		for(int32 j = 0; j < inst->numIndices; j++)
			if(indices[j] >= (uint32)inst->numVertices){
				unlockIndices(inst->indexBuffer);
				destroyNativeData(geometry, 0, 0);
				return nil;
			}
		unlockIndices(inst->indexBuffer);

		inst->managed = 1;
		assert(inst->vertexBuffer == nil);
		inst->vertexBuffer = createVertexBuffer(vertexBytes, 0, false);
		if(inst->vertexBuffer == nil){
			destroyNativeData(geometry, 0, 0);
			return nil;
		}
		uint8 *verts = lockVertices(inst->vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);
		if(verts == nil){
			destroyNativeData(geometry, 0, 0);
			return nil;
		}
		if(stream->read8(verts, vertexBytes) != vertexBytes){
			unlockVertices(inst->vertexBuffer);
			destroyNativeData(geometry, 0, 0);
			return nil;
		}
		convertVertexData(verts, inst->numVertices, inst->stride,
		                  geometry->flags, geometry->numTexCoordSets, false);
		unlockVertices(inst->vertexBuffer);

		inst++;
	}
	return stream;
}

Stream*
writeNativeData(Stream *stream, int32 len, void *object, int32, int32)
{
	Geometry *geometry = (Geometry*)object;
	writeChunkHeader(stream, ID_STRUCT, len-12);
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D8)
		return stream;
	stream->writeU32(PLATFORM_D3D8);
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;

	int32 size = 4 + geometry->meshHeader->numMeshes*0x2C;
	uint8 *data = rwMallocT(uint8, size, MEMDUR_FUNCTION | ID_GEOMETRY);
	if(data == nil)
		return nil;
	stream->writeI32(size);
	uint8 *p = data;
	writeLE16(p, header->serialNumber); p += 2;
	writeLE16(p, header->numMeshes); p += 2;

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		writeLE32(p, inst->minVert); p += 4;
		writeLE32(p, inst->stride); p += 4;
		writeLE32(p, inst->numVertices); p += 4;
		writeLE32(p, inst->numIndices); p += 4;
		int32 matid = geometry->matList.findIndex(inst->material);
		writeLE32(p, matid); p += 4;
		writeLE32(p, inst->vertexShader); p += 4;
		writeLE32(p, inst->primType); p += 4;
		writeLE32(p, 0); p += 4;
		writeLE32(p, 0); p += 4;
		writeLE32(p, inst->baseIndex); p += 4;
		*p++ = inst->vertexAlpha;
		*p++ = inst->managed;
		*p++ = inst->remapped;
		*p++ = 0;
		inst++;
	}
	uint32 writtenMetadata = stream->write8(data, size);
	rwFree(data);
	if(writtenMetadata != (uint32)size)
		return nil;

	inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		uint32 indexBytes = (uint32)inst->numIndices*2;
		uint32 vertexBytes = (uint32)inst->stride*(uint32)inst->numVertices;
		uint16 *indices = lockIndices(inst->indexBuffer, 0, 0, 0);
		if(indices == nil || stream->write16(indices, indexBytes) != indexBytes){
			if(indices)
				unlockIndices(inst->indexBuffer);
			return nil;
		}
		unlockIndices(inst->indexBuffer);

		uint8 *verts = lockVertices(inst->vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);
		if(verts == nil)
			return nil;
		uint8 *littleEndianVertices = rwMallocT(uint8, vertexBytes, MEMDUR_FUNCTION | ID_GEOMETRY);
		if(littleEndianVertices == nil){
			unlockVertices(inst->vertexBuffer);
			return nil;
		}
		memcpy(littleEndianVertices, verts, vertexBytes);
		convertVertexData(littleEndianVertices, inst->numVertices, inst->stride,
		                  geometry->flags, geometry->numTexCoordSets, true);
		uint32 written = stream->write8(littleEndianVertices, vertexBytes);
		rwFree(littleEndianVertices);
		unlockVertices(inst->vertexBuffer);
		if(written != vertexBytes)
			return nil;
		inst++;
	}
	return stream;
}

int32
getSizeNativeData(void *object, int32, int32)
{
	Geometry *geometry = (Geometry*)object;
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_D3D8)
		return 0;

	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	InstanceData *inst = header->inst;
	int32 size = 12 + 4 + 4 + 4 + header->numMeshes*0x2C;
	for(int32 i = 0; i < header->numMeshes; i++){
		size += inst->numIndices*2 + inst->numVertices*inst->stride;
		inst++;
	}
	return size;
}

void
registerNativeDataPlugin(void)
{
	Geometry::registerPlugin(0, ID_NATIVEDATA,
	                         nil, destroyNativeData, nil);
	Geometry::registerPluginStream(ID_NATIVEDATA,
	                               readNativeData,
	                               writeNativeData,
	                               getSizeNativeData);
}


static void
instance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	// TODO: allow for REINSTANCE
	if(geo->instData)
		return;
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	MeshHeader *meshh = geo->meshHeader;
	geo->instData = header;
	header->platform = PLATFORM_D3D8;

	header->serialNumber = meshh->serialNum;
	header->numMeshes = meshh->numMeshes;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

	InstanceData *inst = header->inst;
	Mesh *mesh = meshh->getMeshes();
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, &inst->numVertices);
		inst->numIndices = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexShader = 0;
		inst->primType = meshh->flags == 1 ? D3DPT_TRIANGLESTRIP : D3DPT_TRIANGLELIST;
		inst->vertexBuffer = nil;
		inst->baseIndex = 0;	// (maybe) not used by us
		inst->vertexAlpha = 0;
		inst->managed = 0;
		inst->remapped = 0;

		inst->indexBuffer = createIndexBuffer(inst->numIndices*2, false);
		uint16 *indices = lockIndices(inst->indexBuffer, 0, 0, 0);
		if(inst->minVert == 0)
			memcpy(indices, mesh->indices, inst->numIndices*2);
		else
			for(int32 j = 0; j < inst->numIndices; j++)
				indices[j] = mesh->indices[j] - inst->minVert;
		unlockIndices(inst->indexBuffer);

		pipe->instanceCB(geo, inst);
		mesh++;
		inst++;
	}
}

static void
uninstance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	if((geo->flags & Geometry::NATIVE) == 0)
		return;
	assert(geo->instData != nil);
	assert(geo->instData->platform == PLATFORM_D3D8);
	geo->numTriangles = geo->meshHeader->guessNumTriangles();
	geo->allocateData();
	geo->allocateMeshes(geo->meshHeader->numMeshes, geo->meshHeader->totalIndices, 0);

	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	InstanceData *inst = header->inst;
	Mesh *mesh = geo->meshHeader->getMeshes();
	for(uint32 i = 0; i < header->numMeshes; i++){
		uint16 *indices = lockIndices(inst->indexBuffer, 0, 0, 0);
		if(inst->minVert == 0)
			memcpy(mesh->indices, indices, inst->numIndices*2);
		else
			for(int32 j = 0; j < inst->numIndices; j++)
				mesh->indices[j] = indices[j] + inst->minVert;
		unlockIndices(inst->indexBuffer);

		pipe->uninstanceCB(geo, inst);
		mesh++;
		inst++;
	}
	geo->generateTriangles();
	geo->flags &= ~Geometry::NATIVE;
	destroyNativeData(geo, 0, 0);
}

static void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	// TODO: allow for REINSTANCE
	if(geo->instData == nil)
		pipe->instance(atomic);
	assert(geo->instData != nil);
	assert(geo->instData->platform == PLATFORM_D3D8);
	if(pipe->renderCB)
		pipe->renderCB(atomic, (InstanceDataHeader*)geo->instData);
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_D3D8);
	this->impl.instance = d3d8::instance;
	this->impl.uninstance = d3d8::uninstance;
	this->impl.render = d3d8::render;
	this->instanceCB = nil;
	this->uninstanceCB = nil;
	this->renderCB = nil;
}

ObjPipeline*
ObjPipeline::create(void)
{
	ObjPipeline *pipe = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

void
defaultInstanceCB(Geometry *geo, InstanceData *inst)
{
	inst->vertexShader = makeFVFDeclaration(geo->flags, geo->numTexCoordSets);
	inst->stride = getStride(geo->flags, geo->numTexCoordSets);

	assert(inst->vertexBuffer == nil);
	inst->vertexBuffer = createVertexBuffer(inst->numVertices*inst->stride,
	                                              inst->vertexShader, false);
	inst->managed = 1;

	uint8 *dst = lockVertices(inst->vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);
	instV3d(VERT_FLOAT3, dst,
		&geo->morphTargets[0].vertices[inst->minVert],
		inst->numVertices, inst->stride);
	dst += 12;

	if(geo->flags & Geometry::NORMALS){
		instV3d(VERT_FLOAT3, dst,
		        &geo->morphTargets[0].normals[inst->minVert],
		        inst->numVertices, inst->stride);
		dst += 12;
	}

	inst->vertexAlpha = 0;
	if(geo->flags & Geometry::PRELIT){
		inst->vertexAlpha = instColor(VERT_ARGB, dst, &geo->colors[inst->minVert],
		                              inst->numVertices, inst->stride);
		dst += 4;
	}

	for(int32 i = 0; i < geo->numTexCoordSets; i++){
		instTexCoords(VERT_FLOAT2, dst, &geo->texCoords[i][inst->minVert],
		        inst->numVertices, inst->stride);
		dst += 8;
	}
	unlockVertices(inst->vertexBuffer);
}

void
defaultUninstanceCB(Geometry *geo, InstanceData *inst)
{
	uint8 *src = lockVertices(inst->vertexBuffer, 0, 0, D3DLOCK_NOSYSLOCK);
	uninstV3d(VERT_FLOAT3,
		&geo->morphTargets[0].vertices[inst->minVert],
		src, inst->numVertices, inst->stride);
	src += 12;

	if(geo->flags & Geometry::NORMALS){
		uninstV3d(VERT_FLOAT3,
		          &geo->morphTargets[0].normals[inst->minVert],
		          src, inst->numVertices, inst->stride);
		src += 12;
	}

	inst->vertexAlpha = 0;
	if(geo->flags & Geometry::PRELIT){
		uninstColor(VERT_ARGB, &geo->colors[inst->minVert], src,
		            inst->numVertices, inst->stride);
		src += 4;
	}

	for(int32 i = 0; i < geo->numTexCoordSets; i++){
		uninstTexCoords(VERT_FLOAT2, &geo->texCoords[i][inst->minVert], src,
		          inst->numVertices, inst->stride);
		src += 8;
	}
	unlockVertices(inst->vertexBuffer);
}

ObjPipeline*
makeDefaultPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = defaultRenderCB;
	return pipe;
}

// Native Texture and Raster

static bool
readTextureBytes(Stream *stream, void *data, uint32 size, uint64 end)
{
	uint32 position = stream->tell();
	if(position == UINT32_MAX || (uint64)position + size > end ||
	   stream->read8(data, size) != size)
		return false;
	return stream->tell() == position + size;
}

static bool
skipTextureBytes(Stream *stream, uint32 size, uint64 end)
{
	uint32 position = stream->tell();
	if(position == UINT32_MAX || size > INT32_MAX || (uint64)position + size > end)
		return false;
	stream->seek((int32)size);
	return stream->tell() == position + size;
}

static bool
readTextureU32(Stream *stream, uint32 &value, uint64 end)
{
	uint8 data[4];
	if(!readTextureBytes(stream, data, sizeof(data), end))
		return false;
	value = readLE32(data);
	return true;
}

Raster*
readAsImage(Stream *stream, int32 width, int32 height, int32 depth, int32 format,
            int32 numLevels, uint64 end)
{
	uint8 palette[256*4];
	int32 pallen = 0;
	uint8 *data = nil;
	uint32 dataCapacity = 0;
	Raster *ras = nil;
	if(width <= 0 || height <= 0 || numLevels <= 0 || numLevels > 32)
		return nil;
	uint32 start = stream->tell();
	uint64 inputSize = 4*256;
	for(int32 i = 0; i < numLevels; i++){
		uint32 mipWidth = width >> i;
		uint32 mipHeight = height >> i;
		if(mipWidth == 0) mipWidth = 1;
		if(mipHeight == 0) mipHeight = 1;
		uint64 levelSize = (uint64)mipWidth*mipHeight;
		if(levelSize > UINT32_MAX || inputSize + 4 + levelSize > UINT32_MAX)
			return nil;
		inputSize += 4 + levelSize;
	}
	uint64 imageStride = (uint64)(uint32)width*4;
	uint64 imageSize = imageStride*(uint32)height;
	if(start == UINT32_MAX || start > end || inputSize != end - start ||
	   imageStride > INT32_MAX ||
	   imageSize > INT32_MAX || imageSize > SIZE_MAX)
		return nil;

	Image *img = Image::create(width, height, 32);
	if(img == nil)
		return nil;
	img->allocate();
	if(img->pixels == nil){
		img->destroy();
		return nil;
	}

	if(format & Raster::PAL8){
		pallen = 256;
		if(!readTextureBytes(stream, palette, 4*pallen, end))
			goto fail;
	}else
		goto fail;
	if(!Raster::formatHasAlpha(format))
		for(int32 i = 0; i < pallen; i++)
			palette[i*4+3] = 0xFF;

	for(int i = 0; i < numLevels; i++){
		uint32 size;
		int32 mipWidth = width >> i;
		int32 mipHeight = height >> i;
		if(mipWidth < 1) mipWidth = 1;
		if(mipHeight < 1) mipHeight = 1;
		uint64 expected = (uint64)(uint32)mipWidth * (uint32)mipHeight;
		if(expected > UINT32_MAX || !readTextureU32(stream, size, end) || size != expected)
			goto fail;

		if(ras && i >= ras->getNumLevels()){
			if(!skipTextureBytes(stream, size, end))
				goto fail;
			continue;
		}

		if(data == nil){
			data = rwNewT(uint8, size, MEMDUR_FUNCTION | ID_IMAGE);
			if(data == nil)
				goto fail;
			dataCapacity = size;
		}
		if(size > dataCapacity || !readTextureBytes(stream, data, size, end))
			goto fail;

		if(ras){
			img->width = mipWidth;
			img->height = mipHeight;
			img->stride = img->width*img->bpp;
		}

		uint8 *idx = data;
		uint8 *pixels = img->pixels;
		for(int y = 0; y < img->height; y++){
			uint8 *line = pixels;
			for(int x = 0; x < img->width; x++){
				if(*idx >= pallen)
					goto fail;
				line[0] = palette[*idx*4+0];
				line[1] = palette[*idx*4+1];
				line[2] = palette[*idx*4+2];
				line[3] = palette[*idx*4+3];
				line += img->bpp;
				idx++;
			}
			pixels += img->stride;
		}

		if(ras == nil){
			int32 newformat;
			if(!Raster::imageFindRasterFormat(img, format&7, &width, &height, &depth, &newformat))
				goto fail;
			newformat |= format & (Raster::MIPMAP | Raster::AUTOMIPMAP);
			ras = Raster::create(width, height, depth, newformat);
			if(ras == nil)
				goto fail;
		}

		if(ras->lock(i, Raster::LOCKWRITE|Raster::LOCKNOFETCH) == nil)
			goto fail;
		bool set = ras->setFromImage(img) != nil;
		ras->unlock(i);
		if(!set)
			goto fail;
	}

	rwFree(data);
	img->destroy();
	return ras;

fail:
	if(ras)
		ras->destroy();
	rwFree(data);
	img->destroy();
	return nil;
}

Texture*
readNativeTexture(Stream *stream)
{
	uint32 structSize;
	uint8 header[88];
	uint32 platform, format, hasAlphaValue;
	int32 width, height, depth, numLevels, type, compression;
	int32 pallength = 0;
	int32 maxLevels;
	uint64 structEnd;
	Texture *tex = nil;
	Raster *raster = nil;
	D3dRaster *ras = nil;

	if(stream == nil || !findChunk(stream, ID_STRUCT, &structSize, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	structEnd = (uint64)stream->tell() + structSize;
	if(structSize < sizeof(header) || structEnd > UINT32_MAX ||
	   !readTextureBytes(stream, header, sizeof(header), structEnd))
		return nil;
	platform = readLE32(&header[0]);
	if(platform != PLATFORM_D3D8){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	format = readLE32(&header[72]);
	hasAlphaValue = readLE32(&header[76]);
	width = readLE16(&header[80]);
	height = readLE16(&header[82]);
	depth = header[84];
	numLevels = header[85];
	type = header[86];
	compression = header[87];
	uint32 baseFormat = format & 0xF00;
	uint32 allowedFormatFlags = 0xF00 | Raster::AUTOMIPMAP | Raster::PAL8 |
	                            Raster::PAL4 | Raster::MIPMAP;
	bool validBaseFormat = baseFormat == Raster::C1555 || baseFormat == Raster::C565 ||
	                       baseFormat == Raster::C4444 || baseFormat == Raster::LUM8 ||
	                       baseFormat == Raster::C8888 || baseFormat == Raster::C888 ||
	                       baseFormat == Raster::C555;
	if(memchr(&header[8], '\0', 32) == nil || memchr(&header[40], '\0', 32) == nil ||
	   width <= 0 || height <= 0 || hasAlphaValue > 1 || type != Raster::TEXTURE ||
	   !validBaseFormat || (format & ~allowedFormatFlags) != 0 ||
	   ((format & Raster::PAL4) && (format & Raster::PAL8)) ||
	   (format & Raster::AUTOMIPMAP && !(format & Raster::MIPMAP)))
		return nil;
	maxLevels = Raster::calculateNumLevels(width, height);
	if(numLevels <= 0 || numLevels > maxLevels ||
	   (numLevels > 1 && !(format & Raster::MIPMAP)) ||
	   (compression != 0 && compression != 1 && compression != 3 && compression != 5))
		return nil;
	if(compression){
		if(format & (Raster::PAL4 | Raster::PAL8) || (depth != 16 && depth != 32))
			return nil;
	}else if(format & Raster::PAL4){
		if(depth != 4)
			return nil;
		pallength = 32;
	}else if(format & Raster::PAL8){
		if(depth != 8)
			return nil;
		pallength = 256;
	}else{
		int32 expectedDepth = baseFormat == Raster::LUM8 ? 8 :
		                      baseFormat == Raster::C8888 ? 32 :
		                      baseFormat == Raster::C888 ? 24 : 16;
		if(depth != expectedDepth)
			return nil;
	}

	tex = Texture::create(nil);
	if(tex == nil)
		return nil;
	tex->filterAddressing = readLE32(&header[4]);
	memcpy(tex->name, &header[8], 32);
	memcpy(tex->mask, &header[40], 32);
	if(pallength != 0 && !d3d::isP8supported){
		if(format & Raster::PAL4)
			goto fail;
		raster = readAsImage(stream, width, height, depth, format|type, numLevels, structEnd);
		if(raster == nil || stream->tell() != structEnd)
			goto fail;
		tex->raster = raster;
		return tex;
	}

	if(compression){
		raster = Raster::create(width, height, depth, format | type | Raster::DONTALLOCATE, PLATFORM_D3D8);
		if(raster == nil)
			goto fail;
		tex->raster = raster;
		ras = GETD3DRASTEREXT(raster);
		allocateDXT(raster, compression, numLevels, hasAlphaValue);
		if(ras->texture == nil)
			goto fail;
		ras->customFormat = 1;
	}else{
		raster = Raster::create(width, height, depth, format | type, PLATFORM_D3D8);
		if(raster == nil)
			goto fail;
		tex->raster = raster;
		ras = GETD3DRASTEREXT(raster);
	}

	if(raster->getNumLevels() < numLevels ||
	   (pallength != 0 && (ras->palette == nil ||
	    !readTextureBytes(stream, ras->palette, 4*pallength, structEnd))))
		goto fail;

	for(int32 i = 0; i < numLevels; i++){
		uint32 size;
		if(!readTextureU32(stream, size, structEnd))
			goto fail;
		uint32 expectedSize;
		if(compression){
			uint32 levelWidth = width >> i;
			uint32 levelHeight = height >> i;
			if(levelWidth == 0) levelWidth = 1;
			if(levelHeight == 0) levelHeight = 1;
			expectedSize = getDXTDataSize(compression, levelWidth, levelHeight);
		}else
			expectedSize = getLevelSize(raster, i);
		if(expectedSize == 0 || size != expectedSize)
			goto fail;
		uint8 *data = raster->lock(i, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
		if(data == nil)
			goto fail;
		bool read = readTextureBytes(stream, data, size, structEnd);
		raster->unlock(i);
		if(!read)
			goto fail;
	}
	if(stream->tell() != structEnd)
		goto fail;
	return tex;

fail:
	if(tex)
		tex->destroy();
	return nil;
}

void
writeNativeTexture(Texture *tex, Stream *stream)
{
	int32 chunksize = getSizeNativeTexture(tex);
	writeChunkHeader(stream, ID_STRUCT, chunksize-12);
	stream->writeU32(PLATFORM_D3D8);

	// Texture
	stream->writeU32(tex->filterAddressing);
	stream->write8(tex->name, 32);
	stream->write8(tex->mask, 32);

	// Raster
	Raster *raster = tex->raster;
	D3dRaster *ras = GETD3DRASTEREXT(raster);
	int32 numLevels = raster->getNumLevels();
	stream->writeI32(raster->format);
	stream->writeI32(ras->hasAlpha);
	stream->writeU16(raster->width);
	stream->writeU16(raster->height);
	stream->writeU8(!ras->customFormat && (raster->format & 0xF00) == Raster::C888 ?
	                24 : raster->depth);
	stream->writeU8(numLevels);
	stream->writeU8(raster->type);
	int32 compression = 0;
	if(ras->format)
		switch(ras->format){
		case 0x31545844:	// DXT1
			compression = 1;
			break;
		case 0x32545844:	// DXT2
			compression = 2;
			break;
		case 0x33545844:	// DXT3
			compression = 3;
			break;
		case 0x34545844:	// DXT4
			compression = 4;
			break;
		case 0x35545844:	// DXT5
			compression = 5;
			break;
		}
	stream->writeU8(compression);

	if(raster->format & Raster::PAL4)
		stream->write8(ras->palette, 4*32);
	else if(raster->format & Raster::PAL8)
		stream->write8(ras->palette, 4*256);

	uint32 size;
	uint8 *data;
	for(int32 i = 0; i < numLevels; i++){
		size = getLevelSize(raster, i);
		stream->writeU32(size);
		data = raster->lock(i, Raster::LOCKREAD);
		stream->write8(data, size);
		raster->unlock(i);
	}
}

uint32
getSizeNativeTexture(Texture *tex)
{
	uint32 size = 12 + 72 + 16;
	int32 levels = tex->raster->getNumLevels();
	if(tex->raster->format & Raster::PAL4)
		size += 4*32;
	else if(tex->raster->format & Raster::PAL8)
		size += 4*256;
	for(int32 i = 0; i < levels; i++)
		size += 4 + getLevelSize(tex->raster, i);
	return size;
}

}
}
