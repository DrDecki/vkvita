/*
 * VkVita: VkFormat -> GXM format table.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vkv_internal.h"

#define TX(f)  SCE_GXM_TEXTURE_FORMAT_##f
#define CF(f)  SCE_GXM_COLOR_FORMAT_##f
#define AF(f)  SCE_GXM_ATTRIBUTE_FORMAT_##f
#define OR(f)  SCE_GXM_OUTPUT_REGISTER_FORMAT_##f
#define XF(f)  SCE_GXM_TRANSFER_FORMAT_##f

/*            vk format                      bpp dep rnd cmp tex                       color                 attrib     outReg   xfer  block srgb */
static const VkvFormatInfo k_formats[] = {
	{ VK_FORMAT_R8G8B8A8_UNORM,               4, 0, 1, 4, TX(U8U8U8U8_ABGR),        CF(U8U8U8U8_ABGR),  AF(U8N),  OR(UCHAR4), XF(RAW32) },
	{ VK_FORMAT_R8G8B8A8_SRGB,                4, 0, 1, 4, TX(U8U8U8U8_ABGR),        CF(U8U8U8U8_ABGR),  AF(U8N),  OR(UCHAR4), XF(RAW32), 0, 1 },
	{ VK_FORMAT_B8G8R8A8_UNORM,               4, 0, 1, 0, TX(U8U8U8U8_ARGB),        CF(U8U8U8U8_ARGB),  AF(U8N),  OR(UCHAR4), XF(RAW32) },
	{ VK_FORMAT_B8G8R8A8_SRGB,                4, 0, 1, 0, TX(U8U8U8U8_ARGB),        CF(U8U8U8U8_ARGB),  AF(U8N),  OR(UCHAR4), XF(RAW32), 0, 1 },
	{ VK_FORMAT_R8G8B8A8_SNORM,               4, 0, 0, 4, 0,                        0,                  AF(S8N),  OR(UCHAR4), XF(RAW32) },
	{ VK_FORMAT_R8G8B8A8_UINT,                4, 0, 0, 4, 0,                        0,                  AF(U8),   OR(UCHAR4), XF(RAW32) },
	{ VK_FORMAT_R8G8B8A8_SINT,                4, 0, 0, 4, 0,                        0,                  AF(S8),   OR(UCHAR4), XF(RAW32) },
	{ VK_FORMAT_R8G8B8_UNORM,                 3, 0, 0, 3, TX(U8U8U8_BGR),           0,                  AF(U8N),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R8G8_UNORM,                   2, 0, 1, 2, TX(U8U8_GR),              CF(U8U8_GR),        AF(U8N),  OR(UCHAR4), XF(RAW16) },
	{ VK_FORMAT_R8_UNORM,                     1, 0, 1, 1, TX(U8_R),                 CF(U8_R),           AF(U8N),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R5G6B5_UNORM_PACK16,          2, 0, 1, 0, TX(U5U6U5_RGB),           CF(U5U6U5_RGB),     0,        OR(UCHAR4), XF(RAW16) },
	{ VK_FORMAT_R4G4B4A4_UNORM_PACK16,        2, 0, 0, 0, TX(U4U4U4U4_RGBA),        0,                  0,        OR(UCHAR4), XF(RAW16) },
	{ VK_FORMAT_R5G5B5A1_UNORM_PACK16,        2, 0, 0, 0, TX(U5U5U5U1_RGBA),        0,                  0,        OR(UCHAR4), XF(RAW16) },
	{ VK_FORMAT_A1R5G5B5_UNORM_PACK16,        2, 0, 0, 0, TX(U1U5U5U5_ARGB),        0,                  0,        OR(UCHAR4), XF(RAW16) },
	{ VK_FORMAT_R16G16B16A16_SFLOAT,          8, 0, 1, 4, TX(F16F16F16F16_ABGR),    CF(F16F16F16F16_ABGR), AF(F16), OR(HALF4), 0 },
	{ VK_FORMAT_R16G16_SFLOAT,                4, 0, 1, 2, TX(F16F16_GR),            CF(F16F16_GR),      AF(F16),  OR(HALF4),  XF(RAW32) },
	{ VK_FORMAT_R16_SFLOAT,                   2, 0, 1, 1, 0,                        CF(F16_R),          AF(F16),  OR(HALF4),  XF(RAW16) },
	{ VK_FORMAT_R32_SFLOAT,                   4, 0, 1, 1, TX(F32_R),                CF(F32_R),          AF(F32),  OR(FLOAT),  XF(RAW32) },
	{ VK_FORMAT_R32G32_SFLOAT,                8, 0, 0, 2, TX(F32F32_GR),            0,                  AF(F32),  OR(FLOAT),  0 },
	{ VK_FORMAT_R32G32B32_SFLOAT,            12, 0, 0, 3, 0,                        0,                  AF(F32),  OR(FLOAT),  0 },
	{ VK_FORMAT_R32G32B32A32_SFLOAT,         16, 0, 0, 4, 0,                        0,                  AF(F32),  OR(FLOAT),  0 },
	{ VK_FORMAT_R16G16_UNORM,                 4, 0, 0, 2, TX(U16U16_GR),            0,                  AF(U16N), OR(UCHAR4), 0 },
	{ VK_FORMAT_R16G16_SNORM,                 4, 0, 0, 2, TX(S16S16_GR),            0,                  AF(S16N), OR(UCHAR4), 0 },
	{ VK_FORMAT_R16G16_UINT,                  4, 0, 0, 2, 0,                        0,                  AF(U16),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R16G16_SINT,                  4, 0, 0, 2, 0,                        0,                  AF(S16),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R16G16B16A16_UNORM,           8, 0, 0, 4, 0,                        0,                  AF(U16N), OR(UCHAR4), 0 },
	{ VK_FORMAT_R16G16B16A16_SNORM,           8, 0, 0, 4, 0,                        0,                  AF(S16N), OR(UCHAR4), 0 },
	{ VK_FORMAT_R16G16B16A16_UINT,            8, 0, 0, 4, 0,                        0,                  AF(U16),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R16G16B16A16_SINT,            8, 0, 0, 4, 0,                        0,                  AF(S16),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R16_UNORM,                    2, 0, 0, 1, 0,                        0,                  AF(U16N), OR(UCHAR4), 0 },
	{ VK_FORMAT_R16_SNORM,                    2, 0, 0, 1, 0,                        0,                  AF(S16N), OR(UCHAR4), 0 },
	{ VK_FORMAT_R16_UINT,                     2, 0, 0, 1, 0,                        0,                  AF(U16),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R16_SINT,                     2, 0, 0, 1, 0,                        0,                  AF(S16),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R8G8_SNORM,                   2, 0, 0, 2, 0,                        0,                  AF(S8N),  OR(UCHAR4), 0 },
	{ VK_FORMAT_R8G8_UINT,                    2, 0, 0, 2, 0,                        0,                  AF(U8),   OR(UCHAR4), 0 },
	{ VK_FORMAT_R8G8_SINT,                    2, 0, 0, 2, 0,                        0,                  AF(S8),   OR(UCHAR4), 0 },
	{ VK_FORMAT_R8_UINT,                      1, 0, 0, 1, 0,                        0,                  AF(U8),   OR(UCHAR4), 0 },
	/* block compressed, stored swizzled; bpp is the block size */
	{ VK_FORMAT_BC1_RGB_UNORM_BLOCK,          8, 0, 0, 0, TX(UBC1_1BGR),            0,                  0,        OR(UCHAR4), 0, 8, 0 },
	{ VK_FORMAT_BC1_RGB_SRGB_BLOCK,           8, 0, 0, 0, TX(UBC1_1BGR),            0,                  0,        OR(UCHAR4), 0, 8, 1 },
	{ VK_FORMAT_BC1_RGBA_UNORM_BLOCK,         8, 0, 0, 0, TX(UBC1_ABGR),            0,                  0,        OR(UCHAR4), 0, 8, 0 },
	{ VK_FORMAT_BC1_RGBA_SRGB_BLOCK,          8, 0, 0, 0, TX(UBC1_ABGR),            0,                  0,        OR(UCHAR4), 0, 8, 1 },
	{ VK_FORMAT_BC2_UNORM_BLOCK,             16, 0, 0, 0, TX(UBC2_ABGR),            0,                  0,        OR(UCHAR4), 0, 16, 0 },
	{ VK_FORMAT_BC2_SRGB_BLOCK,              16, 0, 0, 0, TX(UBC2_ABGR),            0,                  0,        OR(UCHAR4), 0, 16, 1 },
	{ VK_FORMAT_BC3_UNORM_BLOCK,             16, 0, 0, 0, TX(UBC3_ABGR),            0,                  0,        OR(UCHAR4), 0, 16, 0 },
	{ VK_FORMAT_BC3_SRGB_BLOCK,              16, 0, 0, 0, TX(UBC3_ABGR),            0,                  0,        OR(UCHAR4), 0, 16, 1 },
	{ VK_FORMAT_BC4_UNORM_BLOCK,              8, 0, 0, 0, TX(UBC4_R),               0,                  0,        OR(UCHAR4), 0, 8, 0 },
	{ VK_FORMAT_BC5_UNORM_BLOCK,             16, 0, 0, 0, TX(UBC5_GR),              0,                  0,        OR(UCHAR4), 0, 16, 0 },
	/* every depth format maps onto the packed S8D24 tiled surface */
	{ VK_FORMAT_D24_UNORM_S8_UINT,            4, 1, 0, 0, 0,                        0,                  0,        OR(UCHAR4), 0 },
	{ VK_FORMAT_X8_D24_UNORM_PACK32,          4, 1, 0, 0, 0,                        0,                  0,        OR(UCHAR4), 0 },
	{ VK_FORMAT_D32_SFLOAT,                   4, 1, 0, 0, 0,                        0,                  0,        OR(UCHAR4), 0 },
	{ VK_FORMAT_D32_SFLOAT_S8_UINT,           4, 1, 0, 0, 0,                        0,                  0,        OR(UCHAR4), 0 },
	{ VK_FORMAT_D16_UNORM,                    4, 1, 0, 0, 0,                        0,                  0,        OR(UCHAR4), 0 },
	{ VK_FORMAT_D16_UNORM_S8_UINT,            4, 1, 0, 0, 0,                        0,                  0,        OR(UCHAR4), 0 },
	{ VK_FORMAT_S8_UINT,                      4, 1, 0, 0, 0,                        0,                  0,        OR(UCHAR4), 0 },
};

const VkvFormatInfo *vkv_format(VkFormat f) {
	for (size_t i = 0; i < VKV_ARRAY_SIZE(k_formats); i++)
		if (k_formats[i].vk == f) return &k_formats[i];
	return NULL;
}
