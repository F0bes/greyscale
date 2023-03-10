#include <stdio.h>
#include <stdlib.h>
#include <graph.h>
#include <dma.h>
#include <dma_tags.h>
#include <gs_psm.h>
#include <gs_gp.h>
#include <gs_privileged.h>
#include <gif_registers.h>
#include <draw.h>
#include <kernel.h>

u32 FBA; // Framebuffer address
const u32 FB_Width = 640;
const u32 FB_Height = 448;
const u32 FBW = FB_Width / 64;

const u32 temp_vram_buffer = 2048 * 150; // Page aligned VRAM area for a page of PSMCT16
u32 palette_vram_buffer = 0;			 // Block aligned VRAM area for a CLUT of 16x16 PSMCT32

void setup_gs_environment()
{
	framebuffer_t fb;
	fb.address = FBA = graph_vram_allocate(FB_Width, FB_Height, GS_PSM_16, GRAPH_ALIGN_PAGE);
	fb.width = FB_Width;
	fb.height = FB_Height;
	fb.psm = GS_PSM_16;
	fb.mask = 0;

	graph_initialize(fb.address, fb.width, fb.height, fb.psm, 0, 0);

	zbuffer_t zb;
	zb.address = 0;
	zb.enable = 0;
	zb.mask = 0;
	zb.method = 0;
	zb.zsm = 0;

	qword_t *gif_packet = aligned_alloc(16, sizeof(qword_t) * 50);
	qword_t *q = gif_packet;

	// Clear the framebuffer as 32ct
	q = draw_setup_environment(q, 0, &fb, &zb);
	q = draw_clear(q, 0, 0x00, 0, 640, 448, 0,0,0);
	q = draw_primitive_xyoffset(q, 0, 0, 0);

	dma_channel_send_normal(DMA_CHANNEL_GIF, gif_packet, q - gif_packet, 0, 0);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);

	free(gif_packet);
}

extern unsigned int size_texture;
extern unsigned char texture[];

// Uploads the texture to the framebuffer
// Not required if you have a pre-existing frame, only for testing purposes
void upload_texture()
{
	qword_t *gif_packet = aligned_alloc(16, sizeof(qword_t) * 100);
	qword_t *q = gif_packet;

	q = draw_texture_transfer(q, texture, 256, 256, GS_PSM_16, FBA, FBW * 64);
	q = draw_texture_flush(q);

	dma_channel_send_chain(DMA_CHANNEL_GIF, gif_packet, q - gif_packet, 0, 0);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);

	free(gif_packet);
}

typedef enum
{
	CHANNEL_RED = 0x000000FF,
	CHANNEL_GREEN = 0x0000FF00,
	CHANNEL_BLUE = 0x00FF0000,
	CHANNEL_ALPHA = 0xFF000000,
} ColourChannels;

void uploadPalette()
{
	if (palette_vram_buffer == 0)
		palette_vram_buffer = graph_vram_allocate(16, 16, GS_PSM_32, GRAPH_ALIGN_BLOCK);

	u32 *palette = aligned_alloc(16, sizeof(u32) * 256);
	int row = 1;
	int index = 0;

	// Generates a 0 -> 0xFF CLUT table in CSM1 format
	for (u32 i = 0; index < 256; i++)
	{
		if (index % 16 == 0 && index > 1)
		{
			if (row == 1 || row % 2 != 0)
				i -= 0x10;
			row++;
		}
		else if (index % 8 == 0 && index > 1)
		{
			i += 0x8;
		}

		palette[index] = (i << 24) | (i << 16) | (i << 8) | (i << 0);
		index++;
	}
	FlushCache(0);

	qword_t transfer_chain[30] ALIGNED(64);
	qword_t *q = transfer_chain;

	q = draw_texture_transfer(q, palette, 16, 16, GS_PSM_32, palette_vram_buffer, 256);
	q = draw_texture_flush(q);

	dma_channel_send_chain(DMA_CHANNEL_GIF, transfer_chain, q - transfer_chain, 0, 0);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);
	free(palette);
}

// Channel copy a page
void performChannelCopy(ColourChannels channelIn, u32 channelOut, u32 blockX, u32 blockY, u32 source)
{
	qword_t *copy_packet = aligned_alloc(16, sizeof(qword_t) * 1000);
	qword_t *q = copy_packet;

	q = copy_packet;

	// For the BLUE and ALPHA channels, we need to offset our 'U's by 8 texels
	const u32 horz_block_offset = (channelIn == CHANNEL_BLUE || channelIn == CHANNEL_ALPHA);
	// For the GREEN and ALPHA channels, we need to offset our 'T's by 2 texels
	const u32 vert_block_offset = (channelIn == CHANNEL_GREEN || channelIn == CHANNEL_ALPHA);

	const u32 clamp_horz = horz_block_offset ? 8 : 0;
	const u32 clamp_vert = vert_block_offset ? 2 : 0;
	PACK_GIFTAG(q, GIF_SET_TAG(4, 1, GIF_PRE_DISABLE, 0, GIF_FLG_PACKED, 1),
				(GIF_REG_AD));
	q++;
	// TEX0
	q->dw[0] = GS_SET_TEX0((source >> 6), 1, GS_PSM_8, 10, 10, 0, 1, palette_vram_buffer / 64, GS_PSM_32, 0, 0, 1);
	q->dw[1] = GS_REG_TEX0;
	q++;
	// CLAMP
	q->dw[0] = GS_SET_CLAMP(3, 3, 0xF7, clamp_horz, 0xFD, clamp_vert);
	q->dw[1] = GS_REG_CLAMP;
	q++;
	// TEXFLUSH
	q->dw[0] = GS_SET_TEXFLUSH(1);
	q->dw[1] = GS_REG_TEXFLUSH;
	q++;
	u32 frame_mask = ~channelOut;

	// FRAME
	q->dw[0] = GS_SET_FRAME(source / 2048, FBW, 0, frame_mask);
	q->dw[1] = GS_REG_FRAME;
	q++;

	PACK_GIFTAG(q, GIF_SET_TAG(96, 1, GIF_PRE_ENABLE, GIF_SET_PRIM(GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0), GIF_FLG_PACKED, 4),
				(GIF_REG_UV) | (GIF_REG_XYZ2 << 4) | (GIF_REG_UV << 8) | (GIF_REG_XYZ2 << 12));
	q++;
	for (int y = 0; y < 32; y += 2)
	{
		if (((y % 4) == 0) ^ (vert_block_offset == 1)) // Even (4 16x2 sprites)
		{
			for (int x = 0; x < 64; x += 16)
			{
				// UV
				q->dw[0] = GS_SET_ST(8 + ((8 + x * 2) << 4), 8 + ((y * 2) << 4));
				q->dw[1] = 0;
				q++;
				// XYZ2
				q->dw[0] = (u64)((x + blockX) << 4) | ((u64)((y + blockY) << 4) << 32);
				q->dw[1] = (u64)(1);
				q++;
				// UV
				q->dw[0] = GS_SET_ST(8 + ((24 + x * 2) << 4), 8 + ((2 + y * 2) << 4));
				q->dw[1] = 0;
				q++;
				// XYZ2
				q->dw[0] = (u64)((x + 16 + blockX) << 4) | ((u64)((y + 2 + blockY) << 4) << 32);
				q->dw[1] = (u64)(1);
				q++;
			}
		}
		else // Odd (Eight 8x2 sprites)
		{
			for (int x = 0; x < 64; x += 8)
			{
				// UV
				q->dw[0] = GS_SET_ST(8 + ((4 + x * 2) << 4), 8 + ((y * 2) << 4));
				q->dw[1] = 0;
				q++;
				// XYZ2
				q->dw[0] = (u64)((x + blockX) << 4) | ((u64)((y + blockY) << 4) << 32);
				q->dw[1] = (u64)(1);
				q++;
				// UV
				q->dw[0] = GS_SET_ST(8 + ((12 + x * 2) << 4), 8 + ((2 + y * 2) << 4));
				q->dw[1] = 0;
				q++;
				// XYZ2
				q->dw[0] = (u64)((x + 8 + blockX) << 4) | ((u64)((y + 2 + blockY) << 4) << 32);
				q->dw[1] = (u64)(1);
				q++;
			}
		}
	}
	FlushCache(0);
	dma_channel_send_normal(DMA_CHANNEL_GIF, copy_packet, q - copy_packet, 0, 0);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);

	q = copy_packet;
	PACK_GIFTAG(q, GIF_SET_TAG(2, 1, GIF_PRE_DISABLE, 0, GIF_FLG_PACKED, 1),
				(GIF_REG_AD));
	q++;
	q->dw[0] = GS_SET_CLAMP(0, 0, 0xff, 0, 0xff, 0);
	q->dw[1] = GS_REG_CLAMP;
	q++;
	// FRAME
	q->dw[0] = GS_SET_FRAME(FBA / 2048, FBW, GS_PSM_16, 0x00);
	q->dw[1] = GS_REG_FRAME;
	q++;

	dma_channel_send_normal(DMA_CHANNEL_GIF, copy_packet, q - copy_packet, 0, 0);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);

	free(copy_packet);
}

void copy32To16Page(u32 inAddr, u32 x, u32 y)
{
	qword_t *gif_packet = aligned_alloc(16, sizeof(qword_t) * 40);
	qword_t *q = gif_packet;

	PACK_GIFTAG(q, GIF_SET_TAG(9, 1, GIF_PRE_ENABLE, GIF_SET_PRIM(GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 0, 0, 0), GIF_FLG_PACKED, 1),
				GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_RGBAQ(0, 0, 0, 1, 0x3f800000), GS_REG_RGBAQ);
	q++;
	PACK_GIFTAG(q, GS_SET_FRAME(FBA / 2048, FBW, GS_PSM_16, 0), GS_REG_FRAME);
	q++;
	PACK_GIFTAG(q, GS_SET_TEX0_SMALL(inAddr >> 6, 10, GS_PSM_32, 6, 5, 1, 1), GS_REG_TEX0);
	q++;
	PACK_GIFTAG(q, GS_SET_TEXFLUSH(1), GS_REG_TEXFLUSH);
	q++;
	PACK_GIFTAG(q, GS_SET_ST(0, 0), GS_REG_ST);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ((x) << 4, (y) << 4, 0), GS_REG_XYZ2);
	q++;
	PACK_GIFTAG(q, GS_SET_ST(0x3f800000, 0x3f800000), GS_REG_ST);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ((64 + x) << 4, (y + 32) << 4, 0), GS_REG_XYZ2);
	q++;
	PACK_GIFTAG(q, GS_SET_FRAME(FBA / 2048, FBW, GS_PSM_16, 0), GS_REG_NOP);
	q++;

	dma_channel_send_normal(DMA_CHANNEL_GIF, gif_packet, q - gif_packet, 0, 0);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);
	free(gif_packet);
}

// 16 -> 32
void copy16To32Page(u32 inAddr)
{
	qword_t *gif_packet = aligned_alloc(16, sizeof(qword_t) * 40);
	qword_t *q = gif_packet;

	PACK_GIFTAG(q, GIF_SET_TAG(9, 1, GIF_PRE_ENABLE, GIF_SET_PRIM(GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 0, 0, 0), GIF_FLG_PACKED, 1),
				GIF_REG_AD);
	q++;
	PACK_GIFTAG(q, GS_SET_RGBAQ(0, 0, 0, 1, 0x3f800000), GS_REG_RGBAQ);
	q++;
	PACK_GIFTAG(q, GS_SET_FRAME(temp_vram_buffer / 2048, FBW, GS_PSM_32, 0), GS_REG_FRAME);
	q++;
	PACK_GIFTAG(q, GS_SET_TEX0_SMALL(inAddr >> 6, 10, GS_PSM_16, 6, 6, 1, 1), GS_REG_TEX0);
	q++;
	PACK_GIFTAG(q, GS_SET_TEXFLUSH(1), GS_REG_TEXFLUSH);
	q++;
	PACK_GIFTAG(q, GS_SET_ST(0, 0), GS_REG_ST);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ(0 << 4, 0 << 4, 0), GS_REG_XYZ2);
	q++;
	PACK_GIFTAG(q, GS_SET_ST(0x3f800000, 0x3f800000), GS_REG_ST);
	q++;
	PACK_GIFTAG(q, GS_SET_XYZ(64 << 4, 64 << 4, 0), GS_REG_XYZ2);
	q++;
	PACK_GIFTAG(q, GS_SET_FRAME(0, FBW, GS_PSM_16, 0), GS_REG_NOP);
	q++;

	dma_channel_send_normal(DMA_CHANNEL_GIF, gif_packet, q - gif_packet, 0, 0);
	dma_channel_wait(DMA_CHANNEL_GIF, 0);
	free(gif_packet);
}

int main(void)
{
	setup_gs_environment();
	uploadPalette();

	while (1)
	{
		// Upload the PSM16 source texture
		upload_texture();

		for (int i = 0; i < FB_Width / 64; i++)
		{
			for (int j = 0; j < FB_Height / 64; j++)
			{
				// Only need to copy it once, PSMCT16 pages are 64x64 (2 PSMCT32 pages)
				// Copies the page from the framebuffer to the temp buffer
				// converting it from CT16 to CT32
				copy16To32Page(i * 2048 + (j * (2048 * 10)) + FBA);

#if 0
				for(int debug = 0; debug < 50; debug++)
					graph_wait_vsync();
#endif
				// Copy the red channel to all the other channels
				// In-place, in the temp buffer
				// Upper page
				performChannelCopy(CHANNEL_RED, CHANNEL_GREEN | CHANNEL_RED | CHANNEL_BLUE, 0, 0, temp_vram_buffer);
				// Lower page
				performChannelCopy(CHANNEL_RED, CHANNEL_GREEN | CHANNEL_RED | CHANNEL_BLUE, 0, 0, temp_vram_buffer + (2048 * FBW));

				// Copy the data from the temp buffer that was channel copied to the framebuffer
				copy32To16Page(temp_vram_buffer, i * 64, (j * 64));
				copy32To16Page(temp_vram_buffer + (2048 * FBW), i * 64, 32 + (j * 64));
			}
		}
		graph_wait_vsync();
	}
	SleepThread();
}
