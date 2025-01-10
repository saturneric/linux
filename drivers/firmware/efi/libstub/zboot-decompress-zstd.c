// SPDX-License-Identifier: GPL-2.0

#include <linux/efi.h>
#include <linux/zstd.h>

#include <asm/efi.h>

#include "decompress_sources.h"
#include "efistub.h"

extern unsigned char _gzdata_start[], _gzdata_end[];
extern u32 __aligned(1) payload_size;

static ZSTD_inBuffer zstd_buf;
static ZSTD_DStream *dstream;
static size_t wksp_size;
static void *wksp;

efi_status_t efi_zboot_decompress_init(unsigned long *alloc_size)
{
	zstd_frame_header header;
	efi_status_t status;
	size_t ret;

	zstd_buf.src = _gzdata_start;
	zstd_buf.pos = 0;
	zstd_buf.size = _gzdata_end - _gzdata_start;

	ret = zstd_get_frame_header(&header, zstd_buf.src, zstd_buf.size);
	if (ret != 0) {
		efi_err("ZSTD-compressed data has an incomplete frame header\n");
		status = EFI_LOAD_ERROR;
		goto out;
	}

	if (header.windowSize > (1 << ZSTD_WINDOWLOG_MAX)) {
		efi_err("ZSTD-compressed data has too large a window size\n");
		status = EFI_LOAD_ERROR;
		goto out;
	}

	wksp_size = zstd_dstream_workspace_bound(header.windowSize);
	status = efi_allocate_pages(wksp_size, (unsigned long *)&wksp, ULONG_MAX);
	if (status != EFI_SUCCESS)
		goto out;

	dstream = zstd_init_dstream(header.windowSize, wksp, wksp_size);
	if (!dstream) {
		efi_err("Can't initialize ZSTD stream\n");
		status = EFI_OUT_OF_RESOURCES;
		goto out;
	}

	*alloc_size = payload_size;
	return EFI_SUCCESS;
out:
	efi_free(wksp_size, (unsigned long)wksp);
	return status;
}

efi_status_t efi_zboot_decompress(u8 *out, unsigned long outlen)
{
	ZSTD_outBuffer zstd_dec;
	size_t ret;
	int retval;

	zstd_dec.dst = out;
	zstd_dec.pos = 0;
	zstd_dec.size = outlen;

	ret = zstd_decompress_stream(dstream, &zstd_dec, &zstd_buf);
	efi_free(wksp_size, (unsigned long)wksp);

	retval = zstd_get_error_code(ret);
	if (retval) {
		efi_err("ZSTD-decompression failed with status %d\n", retval);
		return EFI_LOAD_ERROR;
	}

	efi_cache_sync_image((unsigned long)out, outlen);

	return EFI_SUCCESS;
}
