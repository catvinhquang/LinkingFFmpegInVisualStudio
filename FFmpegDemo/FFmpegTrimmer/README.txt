Scale:
#include <libswscale/swscale.h>
sws_ctx
sws_scale
đầu vào và đầu ra là mảng uint8_t (dữ liệu ảnh - frame)

Trim:
Đọc từng packet trong vùng cần cắt và chuyển sang video đích


Demuxing - av_read_frame : đọc packet
Muxing - av_interleaved_write_frame : ghi packet
Decoding - avcodec_decode_video2/avcodec_decode_audio4 : chuyển packet sang frame
Encoding - avcodec_encode_video2/avcodec_encode_audio2 : chuyển frame sang packet
