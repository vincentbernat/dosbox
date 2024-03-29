/*
 *  Copyright (C) 2002-2023  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2023-2023  The DOSBox Staging Team
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "dosbox.h"

#include <cassert>
#include <optional>

#include "capture.h"
#include "render.h"
#include "rgb24.h"
#include "sdlmain.h"

#if (C_SSHOT)
#include <png.h>
#include <zlib.h>

void capture_image(const uint16_t width, const uint16_t height,
                   const uint8_t bits_per_pixel, const uint16_t pitch,
                   const uint8_t capture_flags, const uint8_t* image_data,
                   const uint8_t* palette_data)
{
/*
	LOG_MSG("CAPTURE: Capturing image, width: %d, height: %d, "
	        "bitsPerPixel: %d, pitch: %d, doubleWidth: %d, doubleHeight: %d",
	        width,
	        height,
	        bits_per_pixel,
	        pitch,
	        (capture_flags & CaptureFlagsDoubleWidth),
	        (capture_flags & CaptureFlagsDoubleHeight));
*/
	png_structp png_ptr    = {};
	png_infop info_ptr     = {};
	png_color palette[256] = {};

	FILE* fp = CAPTURE_CreateFile("raw image", ".png");
	if (!fp) {
		return;
	}

	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr) {
		fclose(fp);
		return;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
		fclose(fp);
		return;
	}

	png_init_io(png_ptr, fp);
	png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);

	png_set_compression_mem_level(png_ptr, 8);
	png_set_compression_strategy(png_ptr, Z_DEFAULT_STRATEGY);
	png_set_compression_window_bits(png_ptr, 15);
	png_set_compression_method(png_ptr, 8);
	png_set_compression_buffer_size(png_ptr, 8192);

	if (bits_per_pixel == 8) {
		png_set_IHDR(png_ptr,
		             info_ptr,
		             width,
		             height,
		             8,
		             PNG_COLOR_TYPE_PALETTE,
		             PNG_INTERLACE_NONE,
		             PNG_COMPRESSION_TYPE_DEFAULT,
		             PNG_FILTER_TYPE_DEFAULT);

		for (auto i = 0; i < 256; ++i) {
			palette[i].red   = palette_data[i * 4 + 0];
			palette[i].green = palette_data[i * 4 + 1];
			palette[i].blue  = palette_data[i * 4 + 2];
		}
		png_set_PLTE(png_ptr, info_ptr, palette, 256);

	} else {
		png_set_bgr(png_ptr);
		png_set_IHDR(png_ptr,
		             info_ptr,
		             width,
		             height,
		             8,
		             PNG_COLOR_TYPE_RGB,
		             PNG_INTERLACE_NONE,
		             PNG_COMPRESSION_TYPE_DEFAULT,
		             PNG_FILTER_TYPE_DEFAULT);
	}

#ifdef PNG_TEXT_SUPPORTED
	char keyword[] = "Software";
	static_assert(sizeof(keyword) < 80, "libpng limit");

	char value[] = CANONICAL_PROJECT_NAME " " VERSION;
	constexpr int num_text = 1;

	png_text texts[num_text] = {};

	texts[0].compression = PNG_TEXT_COMPRESSION_NONE;
	texts[0].key         = static_cast<png_charp>(keyword);
	texts[0].text        = static_cast<png_charp>(value);
	texts[0].text_length = sizeof(value);

	png_set_text(png_ptr, info_ptr, texts, num_text);
#endif
	png_write_info(png_ptr, info_ptr);

	const bool is_double_width = (capture_flags & CaptureFlagDoubleWidth);
	const auto row_divisor = (capture_flags & CaptureFlagDoubleHeight) ? 1 : 0;

	uint8_t row_buffer[SCALER_MAXWIDTH * 4];

	for (auto i = 0; i < height; ++i) {
		auto src_row     = image_data + (i >> row_divisor) * pitch;
		auto row_pointer = src_row;

		switch (bits_per_pixel) {
		case 8:
			if (is_double_width) {
				for (auto x = 0; x < width; ++x) {
					row_buffer[x * 2 + 0] =
					        row_buffer[x * 2 + 1] = src_row[x];
				}
				row_pointer = row_buffer;
			}
			break;

		case 15:
			if (is_double_width) {
				for (auto x = 0; x < width; ++x) {
					const auto pixel = host_to_le(
					        reinterpret_cast<const uint16_t*>(
					                src_row)[x]);

					row_buffer[x * 6 + 0] = row_buffer[x * 6 + 3] =
					        ((pixel & 0x001f) * 0x21) >> 2;
					row_buffer[x * 6 + 1] = row_buffer[x * 6 + 4] =
					        ((pixel & 0x03e0) * 0x21) >> 7;
					row_buffer[x * 6 + 2] = row_buffer[x * 6 + 5] =
					        ((pixel & 0x7c00) * 0x21) >> 12;
				}
			} else {
				for (auto x = 0; x < width; ++x) {
					const auto pixel = host_to_le(
					        reinterpret_cast<const uint16_t*>(
					                src_row)[x]);

					row_buffer[x * 3 + 0] = ((pixel & 0x001f) *
					                         0x21) >>
					                        2;
					row_buffer[x * 3 + 1] = ((pixel & 0x03e0) *
					                         0x21) >>
					                        7;
					row_buffer[x * 3 + 2] = ((pixel & 0x7c00) *
					                         0x21) >>
					                        12;
				}
			}
			row_pointer = row_buffer;
			break;

		case 16:
			if (is_double_width) {
				for (auto x = 0; x < width; ++x) {
					const auto pixel = host_to_le(
					        reinterpret_cast<const uint16_t*>(
					                src_row)[x]);
					row_buffer[x * 6 + 0] = row_buffer[x * 6 + 3] =
					        ((pixel & 0x001f) * 0x21) >> 2;
					row_buffer[x * 6 + 1] = row_buffer[x * 6 + 4] =
					        ((pixel & 0x07e0) * 0x41) >> 9;
					row_buffer[x * 6 + 2] = row_buffer[x * 6 + 5] =
					        ((pixel & 0xf800) * 0x21) >> 13;
				}
			} else {
				for (auto x = 0; x < width; ++x) {
					const auto pixel = host_to_le(
					        reinterpret_cast<const uint16_t*>(
					                src_row)[x]);
					row_buffer[x * 3 + 0] = ((pixel & 0x001f) *
					                         0x21) >>
					                        2;
					row_buffer[x * 3 + 1] = ((pixel & 0x07e0) *
					                         0x41) >>
					                        9;
					row_buffer[x * 3 + 2] = ((pixel & 0xf800) *
					                         0x21) >>
					                        13;
				}
			}
			row_pointer = row_buffer;
			break;

		case 24:
			if (is_double_width) {
				for (auto x = 0; x < width; ++x) {
					const auto pixel = host_to_le(
					        reinterpret_cast<const rgb24*>(
					                src_row)[x]);

					reinterpret_cast<rgb24*>(
					        row_buffer)[x * 2 + 0] = pixel;
					reinterpret_cast<rgb24*>(
					        row_buffer)[x * 2 + 1] = pixel;

					row_pointer = row_buffer;
				}
			}
			// There is no else statement here because
			// row_pointer is already defined as src_row
			// above which is already 24-bit single row
			break;

		case 32:
			if (is_double_width) {
				for (auto x = 0; x < width; ++x) {
					row_buffer[x * 6 + 0] = row_buffer[x * 6 + 3] =
					        src_row[x * 4 + 0];
					row_buffer[x * 6 + 1] = row_buffer[x * 6 + 4] =
					        src_row[x * 4 + 1];
					row_buffer[x * 6 + 2] = row_buffer[x * 6 + 5] =
					        src_row[x * 4 + 2];
				}
			} else {
				for (auto x = 0; x < width; ++x) {
					row_buffer[x * 3 + 0] = src_row[x * 4 + 0];
					row_buffer[x * 3 + 1] = src_row[x * 4 + 1];
					row_buffer[x * 3 + 2] = src_row[x * 4 + 2];
				}
			}
			row_pointer = row_buffer;
			break;
		}

		png_write_row(png_ptr, row_pointer);
	}

	png_write_end(png_ptr, 0);
	png_destroy_write_struct(&png_ptr, &info_ptr);
	fclose(fp);
}

#endif

