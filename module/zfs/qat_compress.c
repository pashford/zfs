/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#if defined(_KERNEL) && defined(HAVE_QAT)
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/completion.h>
#include <sys/zfs_context.h>
#include "qat.h"

/*
 * Max instances in QAT device, each instance is a channel to submit
 * jobs to QAT hardware, this is only for pre-allocating instance,
 * and session arrays, the actual number of instances are defined in
 * the QAT driver's configure file.
 */
#define	QAT_DC_MAX_INSTANCES	48

/*
 * ZLIB head and foot size
 */
#define	ZLIB_HEAD_SZ		2
#define	ZLIB_FOOT_SZ		4

static CpaInstanceHandle dc_inst_handles[QAT_DC_MAX_INSTANCES];
static CpaDcSessionHandle session_handles[QAT_DC_MAX_INSTANCES];
static CpaBufferList **buffer_array[QAT_DC_MAX_INSTANCES];
static Cpa16U num_inst = 0;
static Cpa32U inst_num = 0;
static boolean_t qat_dc_init_done = B_FALSE;
int zfs_qat_compress_disable = 0;

boolean_t
qat_dc_use_accel(size_t s_len)
{
	return (!zfs_qat_compress_disable &&
	    qat_dc_init_done &&
	    s_len >= QAT_MIN_BUF_SIZE &&
	    s_len <= QAT_MAX_BUF_SIZE);
}

static void
qat_dc_callback(void *p_callback, CpaStatus status)
{
	if (p_callback != NULL)
		complete((struct completion *)p_callback);
}

static void
qat_dc_clean(void)
{
	Cpa16U buff_num = 0;
	Cpa16U num_inter_buff_lists = 0;
	Cpa16U i = 0;

	for (i = 0; i < num_inst; i++) {
		cpaDcStopInstance(dc_inst_handles[i]);
		QAT_PHYS_CONTIG_FREE(session_handles[i]);
		/* free intermediate buffers  */
		if (buffer_array[i] != NULL) {
			cpaDcGetNumIntermediateBuffers(
			    dc_inst_handles[i], &num_inter_buff_lists);
			for (buff_num = 0; buff_num < num_inter_buff_lists;
			    buff_num++) {
				CpaBufferList *buffer_inter =
				    buffer_array[i][buff_num];
				if (buffer_inter->pBuffers) {
					QAT_PHYS_CONTIG_FREE(
					    buffer_inter->pBuffers->pData);
					QAT_PHYS_CONTIG_FREE(
					    buffer_inter->pBuffers);
				}
				QAT_PHYS_CONTIG_FREE(
				    buffer_inter->pPrivateMetaData);
				QAT_PHYS_CONTIG_FREE(buffer_inter);
			}
		}
	}

	num_inst = 0;
	qat_dc_init_done = B_FALSE;
}

int
qat_dc_init(void)
{
	CpaStatus status = CPA_STATUS_SUCCESS;
	Cpa32U sess_size = 0;
	Cpa32U ctx_size = 0;
	Cpa16U num_inter_buff_lists = 0;
	Cpa16U buff_num = 0;
	Cpa32U buff_meta_size = 0;
	CpaDcSessionSetupData sd = {0};
	Cpa16U i;

	status = cpaDcGetNumInstances(&num_inst);
	if (status != CPA_STATUS_SUCCESS)
		return (-1);

	/* if the user has configured no QAT compression units just return */
	if (num_inst == 0)
		return (0);

	if (num_inst > QAT_DC_MAX_INSTANCES)
		num_inst = QAT_DC_MAX_INSTANCES;

	status = cpaDcGetInstances(num_inst, &dc_inst_handles[0]);
	if (status != CPA_STATUS_SUCCESS)
		return (-1);

	for (i = 0; i < num_inst; i++) {
		cpaDcSetAddressTranslation(dc_inst_handles[i],
		    (void*)virt_to_phys);

		status = cpaDcBufferListGetMetaSize(dc_inst_handles[i],
		    1, &buff_meta_size);

		if (status == CPA_STATUS_SUCCESS)
			status = cpaDcGetNumIntermediateBuffers(
			    dc_inst_handles[i], &num_inter_buff_lists);

		if (status == CPA_STATUS_SUCCESS && num_inter_buff_lists != 0)
			status = QAT_PHYS_CONTIG_ALLOC(&buffer_array[i],
			    num_inter_buff_lists *
			    sizeof (CpaBufferList *));

		for (buff_num = 0; buff_num < num_inter_buff_lists;
		    buff_num++) {
			if (status == CPA_STATUS_SUCCESS)
				status = QAT_PHYS_CONTIG_ALLOC(
				    &buffer_array[i][buff_num],
				    sizeof (CpaBufferList));

			if (status == CPA_STATUS_SUCCESS)
				status = QAT_PHYS_CONTIG_ALLOC(
				    &buffer_array[i][buff_num]->
				    pPrivateMetaData,
				    buff_meta_size);

			if (status == CPA_STATUS_SUCCESS)
				status = QAT_PHYS_CONTIG_ALLOC(
				    &buffer_array[i][buff_num]->pBuffers,
				    sizeof (CpaFlatBuffer));

			if (status == CPA_STATUS_SUCCESS) {
				/*
				 *  implementation requires an intermediate
				 *  buffer approximately twice the size of
				 *  output buffer, which is 2x max buffer
				 *  size here.
				 */
				status = QAT_PHYS_CONTIG_ALLOC(
				    &buffer_array[i][buff_num]->pBuffers->
				    pData, 2 * QAT_MAX_BUF_SIZE);
				if (status != CPA_STATUS_SUCCESS)
					goto fail;

				buffer_array[i][buff_num]->numBuffers = 1;
				buffer_array[i][buff_num]->pBuffers->
				    dataLenInBytes = 2 * QAT_MAX_BUF_SIZE;
			}
		}

		status = cpaDcStartInstance(dc_inst_handles[i],
		    num_inter_buff_lists, buffer_array[i]);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;

		sd.compLevel = CPA_DC_L1;
		sd.compType = CPA_DC_DEFLATE;
		sd.huffType = CPA_DC_HT_FULL_DYNAMIC;
		sd.sessDirection = CPA_DC_DIR_COMBINED;
		sd.sessState = CPA_DC_STATELESS;
		sd.deflateWindowSize = 7;
		sd.checksum = CPA_DC_ADLER32;
		status = cpaDcGetSessionSize(dc_inst_handles[i],
		    &sd, &sess_size, &ctx_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;

		QAT_PHYS_CONTIG_ALLOC(&session_handles[i], sess_size);
		if (session_handles[i] == NULL)
			goto fail;

		status = cpaDcInitSession(dc_inst_handles[i],
		    session_handles[i],
		    &sd, NULL, qat_dc_callback);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;
	}

	qat_dc_init_done = B_TRUE;
	return (0);
fail:
	qat_dc_clean();
	return (-1);
}

void
qat_dc_fini(void)
{
	if (!qat_dc_init_done)
		return;

	qat_dc_clean();
}

int
qat_compress(qat_compress_dir_t dir, char *src, int src_len,
    char *dst, int dst_len, size_t *c_len)
{
	CpaInstanceHandle dc_inst_handle;
	CpaDcSessionHandle session_handle;
	CpaBufferList *buf_list_src = NULL;
	CpaBufferList *buf_list_dst = NULL;
	CpaFlatBuffer *flat_buf_src = NULL;
	CpaFlatBuffer *flat_buf_dst = NULL;
	Cpa8U *buffer_meta_src = NULL;
	Cpa8U *buffer_meta_dst = NULL;
	Cpa32U buffer_meta_size = 0;
	CpaDcRqResults dc_results;
	CpaStatus status = CPA_STATUS_SUCCESS;
	Cpa32U hdr_sz = 0;
	Cpa32U compressed_sz;
	Cpa32U num_src_buf = (src_len >> PAGE_SHIFT) + 2;
	Cpa32U num_dst_buf = (dst_len >> PAGE_SHIFT) + 2;
	Cpa32U bytes_left;
	char *data;
	struct page *in_page, *out_page;
	struct page **in_pages = NULL;
	struct page **out_pages = NULL;
	Cpa32U page_off = 0;
	struct completion complete;
	size_t ret = -1;
	Cpa32U page_num = 0;
	Cpa16U i;

	/*
	 * We increment num_src_buf and num_dst_buf by 2 to allow
	 * us to handle non page-aligned buffer addresses and buffers
	 * whose sizes are not divisible by PAGE_SIZE.
	 */
	Cpa32U src_buffer_list_mem_size = sizeof (CpaBufferList) +
	    (num_src_buf * sizeof (CpaFlatBuffer));
	Cpa32U dst_buffer_list_mem_size = sizeof (CpaBufferList) +
	    (num_dst_buf * sizeof (CpaFlatBuffer));

	if (QAT_PHYS_CONTIG_ALLOC(&in_pages,
	    num_src_buf * sizeof (struct page *)) != CPA_STATUS_SUCCESS)
		goto fail;

	if (QAT_PHYS_CONTIG_ALLOC(&out_pages,
	    num_dst_buf * sizeof (struct page *)) != CPA_STATUS_SUCCESS)
		goto fail;

	i = atomic_inc_32_nv(&inst_num) % num_inst;
	dc_inst_handle = dc_inst_handles[i];
	session_handle = session_handles[i];

	cpaDcBufferListGetMetaSize(dc_inst_handle, num_src_buf,
	    &buffer_meta_size);
	if (QAT_PHYS_CONTIG_ALLOC(&buffer_meta_src, buffer_meta_size) !=
	    CPA_STATUS_SUCCESS)
		goto fail;

	cpaDcBufferListGetMetaSize(dc_inst_handle, num_dst_buf,
	    &buffer_meta_size);
	if (QAT_PHYS_CONTIG_ALLOC(&buffer_meta_dst, buffer_meta_size) !=
	    CPA_STATUS_SUCCESS)
		goto fail;

	/* build source buffer list */
	if (QAT_PHYS_CONTIG_ALLOC(&buf_list_src, src_buffer_list_mem_size) !=
	    CPA_STATUS_SUCCESS)
		goto fail;

	flat_buf_src = (CpaFlatBuffer *)(buf_list_src + 1);

	buf_list_src->pBuffers = flat_buf_src; /* always point to first one */

	/* build destination buffer list */
	if (QAT_PHYS_CONTIG_ALLOC(&buf_list_dst, dst_buffer_list_mem_size) !=
	    CPA_STATUS_SUCCESS)
		goto fail;

	flat_buf_dst = (CpaFlatBuffer *)(buf_list_dst + 1);

	buf_list_dst->pBuffers = flat_buf_dst; /* always point to first one */

	buf_list_src->numBuffers = 0;
	buf_list_src->pPrivateMetaData = buffer_meta_src;
	bytes_left = src_len;
	data = src;
	page_num = 0;
	while (bytes_left > 0) {
		page_off = ((long)data & ~PAGE_MASK);
		in_page = qat_mem_to_page(data);
		in_pages[page_num] = in_page;
		flat_buf_src->pData = kmap(in_page) + page_off;
		flat_buf_src->dataLenInBytes =
		    min((long)PAGE_SIZE - page_off, (long)bytes_left);

		bytes_left -= flat_buf_src->dataLenInBytes;
		data += flat_buf_src->dataLenInBytes;
		flat_buf_src++;
		buf_list_src->numBuffers++;
		page_num++;
	}

	buf_list_dst->numBuffers = 0;
	buf_list_dst->pPrivateMetaData = buffer_meta_dst;
	bytes_left = dst_len;
	data = dst;
	page_num = 0;
	while (bytes_left > 0) {
		page_off = ((long)data & ~PAGE_MASK);
		out_page = qat_mem_to_page(data);
		flat_buf_dst->pData = kmap(out_page) + page_off;
		out_pages[page_num] = out_page;
		flat_buf_dst->dataLenInBytes =
		    min((long)PAGE_SIZE - page_off, (long)bytes_left);

		bytes_left -= flat_buf_dst->dataLenInBytes;
		data += flat_buf_dst->dataLenInBytes;
		flat_buf_dst++;
		buf_list_dst->numBuffers++;
		page_num++;
	}

	init_completion(&complete);

	if (dir == QAT_COMPRESS) {
		QAT_STAT_BUMP(comp_requests);
		QAT_STAT_INCR(comp_total_in_bytes, src_len);

		cpaDcGenerateHeader(session_handle,
		    buf_list_dst->pBuffers, &hdr_sz);
		buf_list_dst->pBuffers->pData += hdr_sz;
		buf_list_dst->pBuffers->dataLenInBytes -= hdr_sz;
		status = cpaDcCompressData(
		    dc_inst_handle, session_handle,
		    buf_list_src, buf_list_dst,
		    &dc_results, CPA_DC_FLUSH_FINAL,
		    &complete);
		if (status != CPA_STATUS_SUCCESS) {
			goto fail;
		}

		/* we now wait until the completion of the operation. */
		if (!wait_for_completion_interruptible_timeout(&complete,
		    QAT_TIMEOUT_MS)) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		if (dc_results.status != CPA_STATUS_SUCCESS) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		compressed_sz = dc_results.produced;
		if (compressed_sz + hdr_sz + ZLIB_FOOT_SZ > dst_len) {
			goto fail;
		}

		flat_buf_dst = (CpaFlatBuffer *)(buf_list_dst + 1);
		/* move to the last page */
		flat_buf_dst += (compressed_sz + hdr_sz) >> PAGE_SHIFT;

		/* no space for gzip foot in the last page */
		if (((compressed_sz + hdr_sz) % PAGE_SIZE)
		    + ZLIB_FOOT_SZ > PAGE_SIZE)
			goto fail;

		/* jump to the end of the buffer and append footer */
		flat_buf_dst->pData =
		    (char *)((unsigned long)flat_buf_dst->pData & PAGE_MASK)
		    + ((compressed_sz + hdr_sz) % PAGE_SIZE);
		flat_buf_dst->dataLenInBytes = ZLIB_FOOT_SZ;

		dc_results.produced = 0;
		status = cpaDcGenerateFooter(session_handle,
		    flat_buf_dst, &dc_results);
		if (status != CPA_STATUS_SUCCESS) {
			goto fail;
		}

		*c_len = compressed_sz + dc_results.produced + hdr_sz;

		QAT_STAT_INCR(comp_total_out_bytes, *c_len);

		ret = 0;

	} else {
		ASSERT3U(dir, ==, QAT_DECOMPRESS);
		QAT_STAT_BUMP(decomp_requests);
		QAT_STAT_INCR(decomp_total_in_bytes, src_len);

		buf_list_src->pBuffers->pData += ZLIB_HEAD_SZ;
		buf_list_src->pBuffers->dataLenInBytes -= ZLIB_HEAD_SZ;
		status = cpaDcDecompressData(dc_inst_handle,
		    session_handle,
		    buf_list_src,
		    buf_list_dst,
		    &dc_results,
		    CPA_DC_FLUSH_FINAL,
		    &complete);

		if (CPA_STATUS_SUCCESS != status) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		/* we now wait until the completion of the operation. */
		if (!wait_for_completion_interruptible_timeout(&complete,
		    QAT_TIMEOUT_MS)) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		if (dc_results.status != CPA_STATUS_SUCCESS) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		*c_len = dc_results.produced;

		QAT_STAT_INCR(decomp_total_out_bytes, *c_len);

		ret = 0;
	}

fail:
	if (status != CPA_STATUS_SUCCESS) {
		QAT_STAT_BUMP(dc_fails);
	}

	if (in_pages) {
		for (page_num = 0;
		    page_num < buf_list_src->numBuffers;
		    page_num++) {
			kunmap(in_pages[page_num]);
		}
		QAT_PHYS_CONTIG_FREE(in_pages);
	}

	if (out_pages) {
		for (page_num = 0;
		    page_num < buf_list_dst->numBuffers;
		    page_num++) {
			kunmap(out_pages[page_num]);
		}
		QAT_PHYS_CONTIG_FREE(out_pages);
	}

	QAT_PHYS_CONTIG_FREE(buffer_meta_src);
	QAT_PHYS_CONTIG_FREE(buffer_meta_dst);
	QAT_PHYS_CONTIG_FREE(buf_list_src);
	QAT_PHYS_CONTIG_FREE(buf_list_dst);

	return (ret);
}

module_param(zfs_qat_compress_disable, int, 0644);
MODULE_PARM_DESC(zfs_qat_compress_disable, "Disable QAT compression");

#endif
