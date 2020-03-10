//#define LOG_NDEBUG 0
#define LOG_TAG "MpiJpegEncoder"
#include <utils/Log.h>

#include <stdlib.h>
#include <string.h>

#include "mpp_err.h"
#include "Utils.h"
#include "MpiDebug.h"
#include "version.h"
#include "RKEncoderWraper.h"
#include "QList.h"
#include "MpiJpegEncoder.h"

uint32_t mpi_enc_debug = 0;

/* APP0 header length of encoded picture default */
static const int APP0_DEFAULT_LEN = 20;

#define _ALIGN(x, a)         (((x)+(a)-1)&~((a)-1))

typedef struct {
    struct timeval start;
    struct timeval end;
} DebugTimeInfo;

static DebugTimeInfo time_info;

static void time_start_record()
{
    if (mpi_enc_debug & DEBUG_TIMING) {
        gettimeofday(&time_info.start, NULL);
    }
}

static void time_end_record(const char *task)
{
    if (mpi_enc_debug & DEBUG_TIMING) {
        gettimeofday(&time_info.end, NULL);
        ALOGD("%s consumes %ld ms", task,
              (time_info.end.tv_sec  - time_info.start.tv_sec)  * 1000 +
              (time_info.end.tv_usec - time_info.start.tv_usec) / 1000);
    }
}

MpiJpegEncoder::MpiJpegEncoder() :
    mMppCtx(NULL),
    mMpi(NULL),
    mInitOK(0),
    mInputWidth(0),
    mInputHeight(0),
    mEncodeQuality(-1),
    mMemGroup(NULL),
    mPackets(NULL),
    mInputFile(NULL),
    mOutputFile(NULL)
{
    ALOGI("version - %s", GIT_INFO);

    /* input format set to YUV420SP default */
    mInputFmt = INPUT_FMT_YUV420SP;

    get_env_u32("mpi_enc_debug", &mpi_enc_debug, 0);

    if (mpi_enc_debug & DEBUG_RECORD_IN) {
        mInputFile = fopen("/data/enc_input.yuv", "wb+");
        if (mInputFile) {
            ALOGD("start dump input yuv to /data/enc_input.yuv");
        }
    }

    if (mpi_enc_debug & DEBUG_RECORD_OUT) {
        mOutputFile = fopen("/data/enc_output.jpg", "wb+");
        if (mOutputFile) {
            ALOGD("start dump output jpeg to /data/enc_output.jpg");
        }
    }
}

MpiJpegEncoder::~MpiJpegEncoder()
{
    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = NULL;
    }
    if (mMemGroup) {
        mpp_buffer_group_put(mMemGroup);
        mMemGroup = NULL;
    }

    if (mPackets) {
        delete mPackets;
        mPackets = NULL;
    }

    if (mInputFile != NULL) {
        fclose(mInputFile);
    }
    if (mOutputFile != NULL) {
        fclose(mOutputFile);
    }
}

bool MpiJpegEncoder::prepareEncoder()
{
    MPP_RET ret = MPP_OK;

    if (mInitOK)
        return true;

    ret = mpp_create(&mMppCtx, &mMpi);
    if (MPP_OK != ret) {
        ALOGE("failed to create mpp context");
        goto FAIL;
    }

    ret = mpp_init(mMppCtx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
    if (MPP_OK != ret) {
        ALOGE("failed to init mpp");
        goto FAIL;
    }

    mPackets = new QList((node_destructor)mpp_frame_deinit);

    /* mpp memery buffer group */
    mpp_buffer_group_get_internal(&mMemGroup, MPP_BUFFER_TYPE_ION);

    mInitOK = 1;

    return true;

FAIL:
    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = NULL;
    }
    return false;
}

void MpiJpegEncoder::flushBuffer()
{
    if (mInitOK) {
        mPackets->lock();
        mPackets->flush();
        mPackets->unlock();

        mMpi->reset(mMppCtx);
    }
}

void MpiJpegEncoder::updateEncodeQuality(int quant)
{
    MPP_RET ret = MPP_OK;
    MppEncCodecCfg codec_cfg;

    if (mEncodeQuality == quant)
        return;

    if (quant < 0 || quant > 10) {
        ALOGW("invalid quality level %d and set to default 8 default", quant);
        quant = 8;
    }

    ALOGV("update encode quality - %d", quant);

    codec_cfg.coding = MPP_VIDEO_CodingMJPEG;
    codec_cfg.jpeg.change = MPP_ENC_JPEG_CFG_CHANGE_QP;
    /* range from 1~10 */
    codec_cfg.jpeg.quant = quant;

    ret = mMpi->control(mMppCtx, MPP_ENC_SET_CODEC_CFG, &codec_cfg);
    if (MPP_OK != ret)
        ALOGE("failed to set encode quality - %d", quant);
    else
        mEncodeQuality = quant;
}

bool MpiJpegEncoder::updateEncodeCfg(int width, int height,
                                     InputFormat fmt, int qLvl)
{
    MPP_RET ret = MPP_OK;

    MppEncPrepCfg prep_cfg;

    int hor_stride, ver_stride;

    if (!mInitOK) {
        ALOGW("Please prepare encoder first before updateEncodeCfg");
        return false;
    }

    if (mInputWidth == width && mInputHeight == height && mInputFmt == fmt)
        return true;

    ALOGV("updateEncodeCfg - %dx%d - inputFmt: %d", width, height, fmt);

    if (width < 16 || width > 8192) {
        ALOGE("invalid width %d is not in range [16..8192]", width);
        return false;
    }

    if (height < 16 || height > 8192) {
        ALOGE("invalid height %d is not in range [16..8192]", height);
        return false;
    }

    hor_stride = _ALIGN(width, 16);
    ver_stride = _ALIGN(height, 8);

    prep_cfg.change        = MPP_ENC_PREP_CFG_CHANGE_INPUT |
                             MPP_ENC_PREP_CFG_CHANGE_ROTATION |
                             MPP_ENC_PREP_CFG_CHANGE_FORMAT;
    prep_cfg.width         = width;
    prep_cfg.height        = height;
    prep_cfg.hor_stride    = hor_stride;
    prep_cfg.ver_stride    = ver_stride;
    prep_cfg.format        = (MppFrameFormat)fmt;
    prep_cfg.rotation      = MPP_ENC_ROT_0;
    ret = mMpi->control(mMppCtx, MPP_ENC_SET_PREP_CFG, &prep_cfg);
    if (MPP_OK != ret) {
        ALOGE("mpi control enc set prep cfg failed ret %d", ret);
        return false;
    }

    updateEncodeQuality(qLvl);

    mInputWidth = width;
    mInputHeight = height;
    mInputFmt = fmt;

    return true;
}

void MpiJpegEncoder::deinitOutputPacket(OutputPacket_t *aPktOut)
{
    MppPacket packet = NULL;

    if (NULL == aPktOut || NULL == aPktOut->packetHandler)
        return;

    mPackets->lock();
    mPackets->del_at_tail(&packet, sizeof(packet));
    if (packet == aPktOut->packetHandler) {
        mpp_packet_deinit(&packet);
    } else {
        ALOGW("deinit found invaild output packet");
        mpp_packet_deinit(&aPktOut->packetHandler);
    }

    mPackets->unlock();
    memset(aPktOut, 0, sizeof(OutputPacket_t));
}

static int getMPPFrameSize(MpiJpegEncoder::InputFormat fmt, int width, int height)
{
    int frame_size  = 0;
    int h_stride    = _ALIGN(width, 16);
    int v_stride    = _ALIGN(height, 16);

    if ((MppFrameFormat)fmt <= MPP_FMT_YUV420SP_VU)
        frame_size = h_stride * v_stride * 3 / 2;
    else if ((MppFrameFormat)fmt <= MPP_FMT_YUV422_UYVY) {
        // NOTE: yuyv and uyvy need to double stride
        h_stride *= 2;
        frame_size = h_stride * v_stride;
    } else
        frame_size = h_stride * v_stride * 4;

    return frame_size;
}

bool MpiJpegEncoder::encodeFrame(char *data, OutputPacket_t *aPktOut)
{
    MPP_RET ret         = MPP_OK;
    /* input frame and output packet */
    MppFrame frame      = NULL;
    MppPacket packet    = NULL;
    MppBuffer frm_buf   = NULL;
    void *frm_ptr       = NULL;

    if (!mInitOK) {
        ALOGW("Please prepare encoder first before encodeFrame");
        return false;
    }

    time_start_record();

    int hor_stride = _ALIGN(mInputWidth, 16);
    int ver_stride = _ALIGN(mInputHeight, 16);
    int frame_size = getMPPFrameSize(mInputFmt, mInputWidth, mInputHeight);

    ret = mpp_buffer_get(mMemGroup, &frm_buf, frame_size);
    if (MPP_OK != ret) {
        ALOGE("failed to get buffer for input frame ret %d", ret);
        goto ENCODE_OUT;
    }

    frm_ptr = mpp_buffer_get_ptr(frm_buf);

    // NOTE: The hardware vpu only process buffer aligned, so we translate
    // input frame to aligned before encode
    ret = read_yuv_image((uint8_t*)frm_ptr, (uint8_t*)data,
                         mInputWidth, mInputHeight, hor_stride,
                         ver_stride, (MppFrameFormat)mInputFmt);
    if (MPP_OK != ret)
        goto ENCODE_OUT;

    ret = mpp_frame_init(&frame);
    if (MPP_OK != ret) {
        ALOGE("failed to init input frame");
        goto ENCODE_OUT;
    }

    mpp_frame_set_width(frame, mInputWidth);
    mpp_frame_set_height(frame, mInputWidth);
    mpp_frame_set_hor_stride(frame, hor_stride);
    mpp_frame_set_ver_stride(frame, ver_stride);
    mpp_frame_set_fmt(frame, (MppFrameFormat)mInputFmt);
    mpp_frame_set_buffer(frame, frm_buf);

    /* dump input frame at fp_input if neccessary */
    dump_mpp_frame_to_file(frame, mInputFile);

    ret = mMpi->encode_put_frame(mMppCtx, frame);
    if (MPP_OK != ret) {
        ALOGE("failed to put input frame");
        goto ENCODE_OUT;
    }

    ret = mMpi->encode_get_packet(mMppCtx, &packet);
    if (MPP_OK != ret) {
        ALOGE("failed to get output packet");
        goto ENCODE_OUT;
    }

    if (packet) {
        memset(aPktOut, 0, sizeof(OutputPacket_t));

        aPktOut->data = (uint8_t*)mpp_packet_get_pos(packet);
        aPktOut->size = mpp_packet_get_length(packet);

        /* dump output packet at mOutputFile if neccessary */
        dump_mpp_packet_to_file(packet, mOutputFile);

        mPackets->lock();
        mPackets->add_at_tail(&packet, sizeof(packet));
        mPackets->unlock();

        ALOGV("encoded one frame get output size %d", aPktOut->size);
    }

ENCODE_OUT:
    if (frm_buf) {
        mpp_buffer_put(frm_buf);
        frm_buf = NULL;
    }

    time_end_record("encode frame");

    return ret == MPP_OK ? true : false;
}

bool MpiJpegEncoder::encodeFile(const char *input_file, const char *output_file)
{
    MPP_RET ret = MPP_OK;
    char *buf = NULL;
    size_t buf_size = 0;

    OutputPacket_t pktOut;

    ALOGD("mpi_jpeg_enc encodeFile start with cfg %dx%d inputFmt %d",
          mInputWidth, mInputHeight, mInputFmt);

    ret = get_file_ptr(input_file, &buf, &buf_size);
    if (MPP_OK != ret)
        goto ENCODE_OUT;

    if (!encodeFrame(buf, &pktOut)) {
        ALOGE("failed to encode input frame");
        goto ENCODE_OUT;
    }

    // Write output packet to destination.
    ret = dump_ptr_to_file((char*)pktOut.data, pktOut.size, output_file);
    if (MPP_OK != ret)
        ALOGE("failed to dump packet to file %s", output_file);

    flushBuffer();

    ALOGD("JPEG encode success get output file %s with size %d",
          output_file, pktOut.size);

ENCODE_OUT:
    if (buf)
        free(buf);

    return ret == MPP_OK ? true : false;
}

MPP_RET MpiJpegEncoder::runFrameEnc(MppFrame in_frame, MppPacket out_packet)
{
    MPP_RET ret         = MPP_OK;
    MppTask task        = NULL;

    if (!in_frame || !out_packet)
        return MPP_NOK;

    /* start queue input task */
    ret = mMpi->poll(mMppCtx, MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (MPP_OK != ret) {
        ALOGE("failed to poll input_task");
        return ret;
    }

    /* dequeue input port */
    ret = mMpi->dequeue(mMppCtx, MPP_PORT_INPUT, &task);
    if (MPP_OK != ret) {
        ALOGE("failed dequeue to input_task ");
        return ret;
    }

    mpp_task_meta_set_frame(task, KEY_INPUT_FRAME, in_frame);
    mpp_task_meta_set_packet(task, KEY_OUTPUT_PACKET, out_packet);

    /* enqueue input port */
    ret = mMpi->enqueue(mMppCtx, MPP_PORT_INPUT, task);
    if (MPP_OK != ret) {
        ALOGE("failed to enqueue input_task");
        return ret;
    }

    task = NULL;

    /* poll and wait here */
    ret = mMpi->poll(mMppCtx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (MPP_OK != ret) {
        ALOGE("failed to poll output_task");
        return ret;
    }

    /* dequeue output port */
    ret = mMpi->dequeue(mMppCtx, MPP_PORT_OUTPUT, &task);
    if (MPP_OK != ret) {
        ALOGE("failed to dequeue output_task");
        return ret;
    }

    if (task) {
        MppPacket packet_out = NULL;
        mpp_task_meta_get_packet(task, KEY_OUTPUT_PACKET, &packet_out);

        /* enqueue output port */
        ret = mMpi->enqueue(mMppCtx, MPP_PORT_OUTPUT, task);
        if (MPP_OK != ret) {
            ALOGE("failed to enqueue output_task");
            return ret;
        }

        return packet_out == out_packet ? MPP_OK : MPP_NOK;
    }

    return MPP_NOK;
}

bool MpiJpegEncoder::encodeImageFD(EncInInfo *aInfoIn,
                                   int dst_offset, OutputPacket_t *aPktOut)
{
    MPP_RET ret         = MPP_OK;

    /* input frame and output packet */
    MppFrame frame      = NULL;
    MppBuffer frm_buf   = NULL;
    MppPacket packet    = NULL;
    MppBuffer pkt_buf   = NULL;

    int width           = aInfoIn->width;
    int height          = aInfoIn->height;
    int h_stride        = _ALIGN(width, 16);
    int v_stride        = _ALIGN(height, 8);
    int pkt_size        = 0;
    int pkt_offset      = 0;

    ALOGV("start encode frame-%dx%d", width, height);

    /* update encode quality and config before encode */
    updateEncodeCfg(width, height, aInfoIn->format, aInfoIn->qLvl);

    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, width);
    mpp_frame_set_height(frame, height);
    mpp_frame_set_hor_stride(frame, h_stride);
    mpp_frame_set_ver_stride(frame, v_stride);
    mpp_frame_set_fmt(frame, (MppFrameFormat)aInfoIn->format);

    /* try import input fd to vpu */
    MppBufferInfo inputCommit;
    memset(&inputCommit, 0, sizeof(MppBufferInfo));
    inputCommit.type = MPP_BUFFER_TYPE_ION;
    inputCommit.size = getMPPFrameSize(aInfoIn->format, width, height);;
    inputCommit.fd = aInfoIn->inputPhyAddr;

    ret = mpp_buffer_import(&frm_buf, &inputCommit);
    if (MPP_OK != ret) {
        ALOGE("failed to import input pictrue buffer");
        goto ENCODE_OUT;
    }
    mpp_frame_set_buffer(frame, frm_buf);

    pkt_size = width * height;
    if (aInfoIn->doThumbNail)
        pkt_size += aInfoIn->thumbWidth * aInfoIn->thumbHeight;

    /*
     * Encoded picture from vpu carried APP0 header information deault, so
     * if wants to carried APP1 header instead, remove APP0 header first.
     */
    pkt_offset = dst_offset - APP0_DEFAULT_LEN;

    /* allocate output packet buffer */
    ret = mpp_buffer_get(mMemGroup, &pkt_buf, pkt_size);
    if (MPP_OK != ret) {
        ALOGE("failed to get buffer for output packet ret %d", ret);
        goto ENCODE_OUT;
    }
    /* TODO: set output buffer offset to mpp */
    mpp_buffer_set_index(pkt_buf, pkt_offset);
    mpp_packet_init_with_buffer(&packet, pkt_buf);

    ret = runFrameEnc(frame, packet);
    if (MPP_OK == ret) {
        memset(aPktOut, 0, sizeof(OutputPacket_t));
        aPktOut->data = (uint8_t*)mpp_packet_get_pos(packet);
        aPktOut->size = mpp_packet_get_length(packet);
        aPktOut->packetHandler = packet;

        mPackets->lock();
        mPackets->add_at_tail(&packet, sizeof(packet));
        mPackets->unlock();

        ALOGV("encod frame get output size %d", aPktOut->size);
    }

ENCODE_OUT:
    if (frm_buf)
        mpp_buffer_put(frm_buf);
    if (pkt_buf)
        mpp_buffer_put(pkt_buf);
    if (frame)
        mpp_frame_deinit(&frame);

    return ret == MPP_OK ? true : false;;
}

bool MpiJpegEncoder::encodeThumb(EncInInfo *aInfoIn, uint8_t **data, int *len)
{
    MPP_RET ret         = MPP_OK;

    /* input frame and output packet */
    MppFrame frame      = NULL;
    MppBuffer frm_buf   = NULL;
    MppPacket packet    = NULL;
    MppBuffer pkt_buf   = NULL;
    void *frm_ptr       = NULL;

    int width           = aInfoIn->thumbWidth;
    int height          = aInfoIn->thumbHeight;
    int h_stride        = _ALIGN(width, 16);
    int v_stride        = _ALIGN(height, 8);
    int frame_size      = 0;

    ALOGV("start encode thumb size-%dx%d", width, height);

    /* update encode quality and config before encode */
    updateEncodeCfg(width, height, aInfoIn->format, aInfoIn->thumbQLvl);

    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, width);
    mpp_frame_set_height(frame, height);
    mpp_frame_set_hor_stride(frame, h_stride);
    mpp_frame_set_ver_stride(frame, v_stride);
    mpp_frame_set_fmt(frame, (MppFrameFormat)aInfoIn->format);

    frame_size = getMPPFrameSize(aInfoIn->format, width, height);
    ret = mpp_buffer_get(mMemGroup, &frm_buf, frame_size);
    if (MPP_OK != ret) {
        ALOGE("failed to get buffer for input frame ret %d", ret);
        goto ENCODE_OUT;
    }

    frm_ptr = mpp_buffer_get_ptr(frm_buf);

    // crop raw buffer to the size of thumbnail first
    ret = crop_yuv_image(aInfoIn->inputVirAddr, (uint8_t*)frm_ptr,
                         aInfoIn->width, aInfoIn->height,
                         aInfoIn->width, aInfoIn->height,
                         aInfoIn->thumbWidth, aInfoIn->thumbHeight);
    if (MPP_OK != ret) {
        ALOGE("failed to crop yuv image before encode thumb.");
        goto ENCODE_OUT;
    }

    mpp_frame_set_buffer(frame, frm_buf);

    /* allocate output packet buffer */
    ret = mpp_buffer_get(mMemGroup, &pkt_buf, width * height);
    if (MPP_OK != ret) {
        ALOGE("failed to get buffer for output packet ret %d", ret);
        goto ENCODE_OUT;
    }
    mpp_packet_init_with_buffer(&packet, pkt_buf);

    ret = runFrameEnc(frame, packet);
    if (MPP_OK == ret) {
        uint8_t *src = (uint8_t*)mpp_packet_get_data(packet);
        int length = mpp_packet_get_length(packet);

        if (length > 0) {
            *data = (uint8_t*)malloc(length);
            if (*data)
                memcpy(*data, src, length);
        }

        *len = length;
        mpp_packet_deinit(&packet);

        ALOGV("encoded thumb get output size %d", length);
    }

ENCODE_OUT:
    if (frm_buf)
        mpp_buffer_put(frm_buf);
    if (pkt_buf)
        mpp_buffer_put(pkt_buf);
    if (frame)
        mpp_frame_deinit(&frame);

    return ret == MPP_OK ? true : false;;
}

bool MpiJpegEncoder::encode(EncInInfo *inInfo, OutputPacket_t *outPkt)
{
    bool ret;
    RkHeaderData hData;
    int input_size;

    if (!mInitOK) {
        ALOGW("Please prepare encoder first before encode");
        return false;
    }

    time_start_record();

    /* dump input data if neccessary */
    input_size = getMPPFrameSize(inInfo->format, inInfo->width, inInfo->height);
    dump_data_to_file(inInfo->inputVirAddr, input_size, mInputFile);

    memset(&hData, 0, sizeof(RkHeaderData));
    hData.exifInfo = (RkExifInfo*)inInfo->exifInfo;

    if (inInfo->doThumbNail) {
        ret = encodeThumb(inInfo, &hData.thumb_data, &hData.thumb_size);
        if (!ret || hData.thumb_size <= 0)
            ALOGW("faild to get thumbNail, will remove it.");
    }

    /* Generate JPEG exif app1 header */
    ret = generate_app1_header(&hData, &hData.header_buf, &hData.header_len);
    if (!ret || hData.header_len <= 0) {
        ALOGE("failed to generate APP1 header.");
        goto TASK_OUT;
    }

    memset(outPkt, 0, sizeof(OutputPacket_t));
    /* Encode raw image by commit input fd to the encoder */
    ret = encodeImageFD(inInfo, hData.header_len, outPkt);
    if (!ret) {
        ALOGE("failed to encode task.");
        goto TASK_OUT;
    }

    /*
     * Encoded picture from vpu carried APP0 header information deault, so
     * if wants to carried APP1 header instead, remove APP0 header first.
     */
    memcpy(outPkt->data, hData.header_buf, hData.header_len);
    outPkt->size = outPkt->size + hData.header_len - APP0_DEFAULT_LEN;

    /* dump output buffer if neccessary */
    dump_data_to_file(outPkt->data, outPkt->size, mOutputFile);

    ALOGD("task encode success get outputFileLen - %d", outPkt->size);

TASK_OUT:
    if (hData.thumb_data)
        free(hData.thumb_data);
    if (hData.header_buf)
        free(hData.header_buf);

    time_end_record("encode task");

    return ret;
}

