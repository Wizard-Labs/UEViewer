#include "Wrappers/TextureNVTT.h"

#include "Core.h"
#include "UnCore.h"
#include "UnObject.h"
#include "UnrealMaterial/UnMaterial.h"
#include "UnrealMaterial/UnMaterial3.h"

#include "Exporters.h"

#include "Wrappers/TexturePNG.h"

#define TGA_SAVE_BOTTOMLEFT	1


#define TGA_ORIGIN_MASK		0x30
#define TGA_BOTLEFT			0x00
#define TGA_BOTRIGHT		0x10					// unused
#define TGA_TOPLEFT			0x20
#define TGA_TOPRIGHT		0x30					// unused

#if _MSC_VER
#pragma pack(push,1)
#endif

struct GCC_PACK tgaHdr_t
{
	byte 	id_length, colormap_type, image_type;
	uint16	colormap_index, colormap_length;
	byte	colormap_size;
	uint16	x_origin, y_origin;				// unused
	uint16	width, height;
	byte	pixel_size, attributes;
};

#if _MSC_VER
#pragma pack(pop)
#endif


bool GNoTgaCompress = false;
bool GExportPNG = false;
bool GExportDDS = false;

//?? place this function outside (cannot place to Core - using FArchive)

void WriteTGA(FArchive &Ar, int width, int height, byte *pic)
{
	guard(WriteTGA);

	int		i;

	byte *src;
	int size = width * height;
	// convert RGB to BGR (inplace!)
	for (i = 0, src = pic; i < size; i++, src += 4)
		Exchange(src[0], src[2]);

	// check for 24 bit image possibility
	int colorBytes = 3;
	for (i = 0, src = pic + 3; i < size; i++, src += 4)
		if (src[0] != 255)									// src initialized with offset 3
		{
			colorBytes = 4;
			break;
		}

	byte *packed = (byte*)appMalloc(width * height * colorBytes + 4); // +4 for being able to put uint32 even when 3 bytes needed
	byte *threshold = packed + width * height * colorBytes - 16; // threshold for "dst"

	src = pic;
	byte *dst = packed;
	int column = 0;
	byte *flag = NULL;
	bool rle = false;

	bool useCompression = true;
	if (GNoTgaCompress)
		threshold = dst;									// will break compression loop immediately

	for (i = 0; i < size; i++)
	{
		if (dst >= threshold)								// when compressed is too large, same uncompressed
		{
			useCompression = false;
			break;
		}

		uint32 pix = *(uint32*)src;
		src += 4;

		if (column < width - 1 &&							// not on screen edge; NOTE: when i == size-1, col==width-1
			pix == *(uint32*)src &&							// next pixel will be the same
			!(rle && flag && *flag == 254))					// flag overflow
		{
			if (!rle || !flag)
			{
				// starting new RLE sequence
				flag = dst++;
				*flag = 128 - 1;							// will be incremented below
				*(uint32*)dst = pix;						// store RGBA
				dst += colorBytes;							// increment dst by 3 or 4 bytes
			}
			(*flag)++;										// enqueue one more texel
			rle = true;
		}
		else
		{
			if (rle)
			{
				// previous block was RLE, and next (now: current) byte was
				// the same - enqueue it to previous block and close block
				(*flag)++;
				flag = NULL;
			}
			else
			{
				if (column == 0)							// check for screen edge
					flag = NULL;
				if (!flag)
				{
					// start new copy sequence
					flag = dst++;
					*flag = 255;
				}
				*(uint32*)dst = pix;						// store RGBA
				dst += colorBytes;							// increment dst by 3 or 4 bytes
				(*flag)++;
				if (*flag == 127) flag = NULL;				// check for overflow
			}
			rle = false;
		}

		if (++column == width) column = 0;
	}

	// write header
	tgaHdr_t header;
	memset(&header, 0, sizeof(header));
	header.width  = width;
	header.height = height;
#if 0
	// debug: write black/white image
	header.pixel_size = 8;
	header.image_type = 3;
	fwrite(&header, 1, sizeof(header), f);
	for (i = 0; i < width * height; i++, pic += 4)
	{
		int c = (pic[0]+pic[1]+pic[2]) / 3;
		fwrite(&c, 1, 1, f);
	}
#else
	header.pixel_size = colorBytes * 8;
#if TGA_SAVE_BOTTOMLEFT
	header.attributes = TGA_BOTLEFT;
#else
	header.attributes = TGA_TOPLEFT;
#endif
	if (useCompression)
	{
		header.image_type = 10;		// RLE
		// write data
		Ar.Serialize(&header, sizeof(header));
		Ar.Serialize(packed, dst - packed);
	}
	else
	{
		header.image_type = 2;		// uncompressed
		// convert to 24 bits image, when needed
		if (colorBytes == 3)
		{
			// Use uint32 for faster data moving
			dst = pic;
			uint32* src = (uint32*)pic;
			for (i = 0; i < size; i++)
			{
				uint32 pix = *src++;
				*((uint32*)dst) = pix;
				dst += 3;	// increment 'dst' only by 3
			}
		}
		// write data
		Ar.Serialize(&header, sizeof(header));
		Ar.Serialize(pic, size * colorBytes);
	}
#endif

	appFree(packed);

	unguard;
}

// Radiance file format

static void float2rgbe(float red, float green, float blue, byte* rgbe)
{
	float v;
	int e;
	v = red;
	if (green > v) v = green;
	if (blue > v) v = blue;
	if (v < 1e-32)
	{
		rgbe[0] = rgbe[1] = rgbe[2] = rgbe[3] = 0;
	}
	else
	{
		v = frexp(v, &e) * 256.0f / v; //?? TODO: check if frexp is slow and could be optimized
		rgbe[0] = byte(red * v);
		rgbe[1] = byte(green * v);
		rgbe[2] = byte(blue * v);
		rgbe[3] = byte(e + 128);
	}
}

static void WriteHDR(FArchive &Ar, int width, int height, byte *pic)
{
	guard(WriteHDR);

	char hdr[64];
	appSprintf(ARRAY_ARG(hdr), "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", height, width);

	//!! TODO: compress HDR file (seems have RLE support)
	// Convert float[w*h*4] to rgbe[w*h] inplace
	const float* floatSrc = (float*)pic;
	byte* byteDst = pic;
	for (int p = 0; p < width * height; p++, floatSrc += 4, byteDst += 4)
	{
		float2rgbe(floatSrc[0], floatSrc[1], floatSrc[2], byteDst);
	}

	Ar.Serialize(hdr, strlen(hdr));
	Ar.Serialize(pic, width * height * 4);

	unguard;
}


static void WriteDDS(FArchive& Ar, const CTextureData& TexData)
{
	guard(WriteDDS);

	if (!TexData.Mips.Num()) return;
	const CMipMap& Mip = TexData.Mips[0];

	unsigned fourCC = TexData.GetFourCC();

	// code from CTextureData::Decompress()
	if (!fourCC)
		appError("unknown texture format %d \n", TexData.Format);	// should not happen - IsDXT() should not pass execution here

	nv::DDSHeader header;
	header.setFourCC(fourCC & 0xFF, (fourCC >> 8) & 0xFF, (fourCC >> 16) & 0xFF, (fourCC >> 24) & 0xFF);
//	header.setPixelFormat(32, 0xFF, 0xFF << 8, 0xFF << 16, 0xFF << 24);	// bit count and per-channel masks
	//!! Note: should use setFourCC for compressed formats, and setPixelFormat for uncompressed - these functions are
	//!! incompatible. When fourcc is used, color masks are zero, and vice versa.
	header.setWidth(Mip.USize);
	header.setHeight(Mip.VSize);
//	header.setNormalFlag(TexData.Format == TPF_DXT5N || TexData.Format == TPF_3DC); -- required for decompression only
	header.setLinearSize(Mip.DataSize);

	byte headerBuffer[128];							// DDS header is 128 bytes long
	memset(headerBuffer, 0, 128);
	WriteDDSHeader(headerBuffer, header);
	Ar.Serialize(headerBuffer, 128);
	Ar.Serialize(const_cast<byte*>(Mip.CompressedData), Mip.DataSize);

	unguard;
}

static void ExportDDS_Worker(FArchive& Ar, CTextureData& TexData, byte* /*pic*/)
{
	WriteDDS(Ar, TexData);
}

static void ExportHDR_Worker(FArchive& Ar, CTextureData& TexData, byte* pic)
{
	WriteHDR(Ar, TexData.Mips[0].USize, TexData.Mips[0].VSize, pic);
}

static void ExportPNG_Worker(FArchive& Ar, CTextureData& TexData, byte* pic)
{
	TArray<byte> Data;
	CompressPNG(pic, TexData.Mips[0].USize, TexData.Mips[0].VSize, Data);
	Ar.Serialize(Data.GetData(), Data.Num());
}

static void ExportTGA_Worker(FArchive& Ar, CTextureData& TexData, byte* pic)
{
	int width = TexData.Mips[0].USize;
	int height = TexData.Mips[0].VSize;
#if TGA_SAVE_BOTTOMLEFT
	// flip image vertically (UnrealEd for UE2 have a bug with importing TGA_TOPLEFT images,
	// it simply ignores orientation flags)
	for (int i = 0; i < height / 2; i++)
	{
		uint32 *p1 = (uint32*)(pic + width * 4 * i);
		uint32 *p2 = (uint32*)(pic + width * 4 * (height - i - 1));
		for (int j = 0; j < width; j++)
			Exchange(*p1++, *p2++);
	}
#endif // TGA_SAVE_BOTTOMLEFT
	WriteTGA(Ar, width, height, pic);
}



void ExportTexture(const UUnrealMaterial *Tex)
{
	guard(ExportTexture);

	// Optimization: the same texture may be passed to export multiple times with different materials
	// or as separate object, avoid re-export.
	if (IsObjectExported(Tex))
		return;

	//todo: performance warning: when GDontOverwriteFiles is set, and file already exists - code will
	//todo: execute slow GetTextureData() anyway and then drop data.

	ETexturePixelFormat Format = Tex->GetTexturePixelFormat();
	if (Format == TPF_UNKNOWN)
	{
		// Warning is already logged by GetTexturePixelFormat()
		return;
	}

	void (*Worker)(FArchive& Ar, CTextureData& TexData, byte* pic);
	bool bNeedDecompressedData = true;
	const char* Ext = NULL;

	if (GExportDDS && PixelFormatInfo[Format].IsDXT())
	{
		Worker = ExportDDS_Worker;
		bNeedDecompressedData = false;
		Ext = "dds";
	}
	else if (PixelFormatInfo[Format].Float)
	{
		Worker = ExportHDR_Worker;
		Ext = "hdr";
	}
	else if (GExportPNG)
	{
		Worker = ExportPNG_Worker;
		Ext = "png";
	}
	else
	{
		Worker = ExportTGA_Worker;
		Ext = "tga";
	}

	FArchive* Ar = CreateExportArchive(Tex, 0, "%s.%s", Tex->Name, Ext);
	if (Ar == NULL)
	{
		// Failed to create file, or file already exists with enabled "don't overwrite" mode
		return;
	}

	// Get texture data
	CTextureData TexData;
	bool bFail = false;
	if (!Tex->GetTextureData(TexData) || TexData.Mips.Num() == 0)
	{
		appPrintf("WARNING: texture %s has no valid mipmaps\n", Tex->Name);
		bFail = true;
		//?? In this case, texture will be logged as "exported". Example: do the export, the texture will be skipped,
		//?? then export again (with "don't overwrite" option) - log will indicate number of exported objects to be non-zero,
		//?? and number will match these "no mipmaps" textures.
	}

	byte* pic = NULL;
	if (!bFail && bNeedDecompressedData)
	{
		pic = TexData.Decompress();
		if (!pic)
		{
			appPrintf("WARNING: failed to decode texture %s\n", Tex->Name);
			bFail = true;
		}
	}

	if (bFail)
	{
		// Close and delete created file
		if (FFileArchive* FileAr = Ar->CastTo<FFileArchive>())
		{
			FileAr->Close();
			remove(FileAr->GetFileName());
		}
		delete Ar;
		return;
	}

	// Do the export
	Worker(*Ar, TexData, pic);

	// Cleanup
	delete Ar;
	if (pic) delete[] pic;
	Tex->ReleaseTextureData();

	unguard;
}
