#include <string>
#include <ctime>
#include <sstream>
#include <map>
#include <iomanip>
#include <vector>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavutil/time.h>
    #include <libavutil/timestamp.h>
    #include <inttypes.h>
    #include <stdlib.h>
    #include <sys/time.h>
    #include <stdio.h>
    #include <inttypes.h>
}

#define release(p) do { \
    if (p) { \
        delete p; \
        p = nullptr; \
    } \
} while(0);

static int decode_packet(AVCodecContext *ctx, AVPacket *pkt, AVFrame *f) {
    int ret = 0;
    ret = avcodec_send_packet(ctx, pkt);
    if (ret != 0) {
#ifdef DEBUG
        switch (ret) {
            case AVERROR(EAGAIN):
                fprintf(stderr, "send packet:AVERROR(EAGAIN)\n");
                break;
            case AVERROR_EOF:
                fprintf(stderr, "send packet:AVERROR_EOF\n");
                break;
            case AVERROR(EINVAL):
                fprintf(stderr, "send packet:AVERROR(EINVAL)\n");
                break;
            case AVERROR(ENOMEM):
                fprintf(stderr, "send packet:AVERROR(ENOMEM)\n");
                break;
            default:
                fprintf(stderr, "send packet:Unkown\n");
        }
#endif
        return ret;
    }

    ret = avcodec_receive_frame(ctx, f);
    if (ret != 0) {
#ifdef DEBUG
        switch (ret) {
            case AVERROR(EAGAIN): {
                fprintf(stderr, "receive packet:AVERROR(EAGAIN)\n");
                break;
            }
            case AVERROR_EOF: {
                fprintf(stderr, "receive packet:AVERROR_EOF\n");
                break;
            }
            case AVERROR(EINVAL): {
                fprintf(stderr, "receive packet:AVERROR(EINVAL)\n");
                break;
            }
            default:
                fprintf(stderr, "receive packet:Unkown\n");
        }
#endif
        return ret;
    }
    
    return 0;
}

class Logger {
    public:
        Logger () {
            log_file_ = stdout;
        }

        Logger(std::string log_filename) {
            log_filename_ = log_filename;
            log_file_ = fopen(log_filename_.c_str(), "w+");
        }

        ~Logger() {
            if (log_file_ != stdout && log_file_ != stderr &&
                log_file_ != stdin) {
                fclose(log_file_);
            }
        }

        template<typename ...Args> int Write(const char *fmt, Args ... args) {
            std::unique_lock<std::mutex> locker(mutex_);
            /* make time preffix */
            struct timeval tp;
            gettimeofday(&tp, NULL);
            time_t now = time(0);
            struct tm *ltm = localtime(&now);
            std::string month;
            if (ltm->tm_mon + 1 < 10) {
                month = "0" + std::to_string(ltm->tm_mon + 1);
            } else {
                month = std::to_string(ltm->tm_mon + 1);
            }
            std::string day;
            if (ltm->tm_mday < 10) {
                day = "0" + std::to_string(ltm->tm_mday);
            } else {
                day = std::to_string(ltm->tm_mday);
            }
            std::stringstream ss;
            ss << ltm->tm_year + 1900 << month << day << " " << ltm->tm_hour << ":" << ltm->tm_min << ":" << ltm->tm_sec << "." << tp.tv_usec/1000;
            std::string log_time = ss.str();

            std::string log_fmt = "[" + log_time + "] " + std::string(fmt) + "\n";
            fprintf(log_file_, log_fmt.c_str(), args...);
            fflush(log_file_);
            return 0;
        }

    private:
        std::string log_filename_;
        FILE *log_file_;
        std::mutex mutex_;
};

class Publisher {
    public:
        Publisher(const std::string input, const std::string output) {
            input_ = input;
            output_ = output;

            stream_mapping_ = NULL;

            ifmt_ctx_ = NULL;
            
            ofmt_ctx_ = NULL;

            packet_logger_ = new Logger();
            codec_info_logger_ = new Logger();
            metadata_logger_ = new Logger();
            stream_state_logger_ = new Logger();
        }

        ~Publisher() {
            avformat_close_input(&ifmt_ctx_);

            /* close output */
            if (ofmt_ctx_ && !(ofmt->flags & AVFMT_NOFILE))
                avio_closep(&ofmt_ctx_->pb);
            
            avformat_free_context(ofmt_ctx_);

            av_freep(&stream_mapping_);
            // std::map<int, AVCodecContext*>::iterator itr;
            for (auto it : codec_ctx_mapping_) {
                avcodec_free_context(&it.second);
            }

            release(packet_logger_);
        }
        int SetPacketLogPath(const std::string path);
        int SetCodecInfoLogPath(const std::string path);
        int SetMetadataLogPath(const std::string path);
        int SetStreamStateLogPath(const std::string path);
        int Start();
    private:
        int logPacket(const AVFormatContext *fmt_ctx, const AVCodecParameters *codec_par, const AVPacket *pkt, const AVFrame *frame);
        int logCodecInfo(const AVCodecParameters *codec_par);
        int logMetadata(const AVDictionary *metadata);
        int logStreamState(const int fps, const int bitrate, const uint64_t duration, uint64_t bytes);
    private:
        std::string input_;
        std::string output_;

        AVFrame *current_frame_;

        /* logger */
        Logger *packet_logger_;
        Logger *codec_info_logger_;
        Logger *metadata_logger_;
        Logger *stream_state_logger_;

        /* input */
        AVFormatContext *ifmt_ctx_;

        /* output */
        AVFormatContext *ofmt_ctx_;
        AVOutputFormat *ofmt;
        int *stream_mapping_;
        std::map<int, AVCodecContext*> codec_ctx_mapping_;
};

int Publisher::logPacket(const AVFormatContext *fmt_ctx, const AVCodecParameters *codec_par, const AVPacket *pkt, const AVFrame *frame) {
    std::string media_type = av_get_media_type_string(codec_par->codec_type);
    char pic_type = '-';
    if (media_type == "video") {
        pic_type = av_get_picture_type_char(frame->pict_type);
    }
    packet_logger_->Write("<%s> pic_type:%c pts:%s dts:%s duration:%s size:%d",
                        media_type.c_str(), pic_type,
                        av_ts2str(pkt->pts), av_ts2str(pkt->dts),
                        av_ts2str(pkt->duration), pkt->size);
    return 0;
}

int Publisher::logCodecInfo(const AVCodecParameters *codec_par) {
    if (codec_par == NULL) return -1;

    const AVCodecDescriptor *des = avcodec_descriptor_get(codec_par->codec_id);
    switch (codec_par->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            codec_info_logger_->Write("Video: codec=%s height=%d width=%d", des->name, codec_par->height, codec_par->width);
            break;
        case AVMEDIA_TYPE_AUDIO:
            codec_info_logger_->Write("Audio: codec=%s sample_rate=%d", des->name, codec_par->sample_rate);
            break;
        default:
            break;
    }
    return 0;
}

int Publisher::logMetadata(const AVDictionary *metadata) {
    if (metadata == NULL) return -1;
    std::string meta_str;
    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
        meta_str += meta_str + std::string(tag->key) + ":" + std::string(tag->value) + " ";

    std::string log_str = "metadata[ " + meta_str + "]";
    metadata_logger_->Write("%s", log_str.c_str());
    return 0;
}

/**
 * Print stream state log
 * @param fps, frames in one second
 * @param bitrate in bps
 * @param duration in millisecond
 * @param bytes of current total stream bytes
 */
int Publisher::logStreamState(const int fps, const int bitrate, const uint64_t duration, uint64_t bytes) {
    stream_state_logger_->Write("fps=%d bitrate=%.3fkbps duration=%.3fs bytes=%" PRIu64, fps, (float)bitrate/1000, (float)duration/1000, bytes);
    return 0;
}

int Publisher::SetPacketLogPath(const std::string path) {
    release(packet_logger_);
    packet_logger_ = new Logger(path);
    return 0;
}

int Publisher::SetCodecInfoLogPath(const std::string path) {
    release(codec_info_logger_);
    codec_info_logger_ = new Logger(path);
    return 0;
}

int Publisher::SetMetadataLogPath(const std::string path) {
    release(metadata_logger_);
    metadata_logger_ = new Logger(path);
    return 0;
}

int Publisher::SetStreamStateLogPath(const std::string path) {
    release(stream_state_logger_);
    stream_state_logger_ = new Logger(path);
    return 0;
}

int Publisher::Start() {
    int ret = 0;
    
    if(avformat_open_input(&ifmt_ctx_, input_.c_str(), NULL, NULL) < 0) {
        fprintf(stderr, "Open input failed\n");
        return -1;
    }

    if (avformat_find_stream_info(ifmt_ctx_, NULL) < 0) {
        fprintf(stderr, "Find stream info failed\n");
        return -1;
    }

    // av_dump_format(ifmt_ctx_, 0, input_.c_str(), 0);

    avformat_alloc_output_context2(&ofmt_ctx_, NULL, "flv", output_.c_str());
    if (!ofmt_ctx_) {
        fprintf(stderr, "Could not create ofmt_ctx\n");
        return -1;
    }

    int stream_mapping_size = ifmt_ctx_->nb_streams;
    stream_mapping_ = (int *)av_mallocz_array(stream_mapping_size, sizeof(*stream_mapping_));
    if (!stream_mapping_) {
        fprintf(stderr, "Map stream failed");
        return -1;
    }

    AVOutputFormat *ofmt = ofmt_ctx_->oformat;

    /* create output streams and map index to stream_mapping */
    int stream_index = 0;
    for (int i = 0; i < ifmt_ctx_->nb_streams; i++) {
        AVStream *out_stream = NULL;
        AVStream *in_stream = ifmt_ctx_->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;

        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping_[i] = -1;
            continue;
        }
        logCodecInfo(in_codecpar);
        stream_mapping_[i] = stream_index;
        
        out_stream = avformat_new_stream(ofmt_ctx_, NULL);

        if (avcodec_parameters_copy(out_stream->codecpar, in_codecpar) < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            return -1;
        }
        out_stream->codecpar->codec_tag = 0;

        AVCodec *codec = avcodec_find_decoder(out_stream->codecpar->codec_id);
        if (codec != NULL) {
            AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
            if (codec_ctx != NULL) {
                codec_ctx_mapping_[stream_index] = codec_ctx;
                /* Copy codec parameters from input stream to output codec context */
                avcodec_parameters_to_context(codec_ctx, out_stream->codecpar);
                if (avcodec_open2(codec_ctx, codec, NULL) != 0) {
                    printf("open codec failed");
                    return -1;
                }
            }
        }
        stream_index++;
    }
    
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_ctx_->pb, output_.c_str(), AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "Could not open output file '%s'", output_.c_str());
            return -1;
        }
    }

    ret = avformat_write_header(ofmt_ctx_, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        return -1;
    }

    logMetadata(ifmt_ctx_->metadata);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int64_t start_time = av_gettime();
    int fps = 0;
    int64_t last_fps_count_time = start_time;
    uint64_t bytes = 0;
    uint64_t last_count_bytes = 0;
    while (av_read_frame(ifmt_ctx_, pkt) >= 0) {
        AVStream *in_stream, *out_stream;

        in_stream = ifmt_ctx_->streams[pkt->stream_index];
        if (pkt->stream_index >= stream_mapping_size ||
            stream_mapping_[pkt->stream_index] < 0) {
            av_packet_unref(pkt);
            continue;
        }
        // logPacket(ifmt_ctx_, in_stream->codecpar, pkt, frame);
        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            fps++;
        }
        bytes += pkt->size;
        uint64_t now = av_gettime();
        if ((now - last_fps_count_time) >= 1000*1000) {
            logStreamState(fps, (bytes - last_count_bytes)*8, (now - start_time)/(1000), bytes);
            fps = 0;
            last_fps_count_time = now;
            last_count_bytes = bytes;
        }

        pkt->stream_index = stream_mapping_[pkt->stream_index];
        out_stream = ofmt_ctx_->streams[pkt->stream_index];

        /* rescale packet to fit output stream */
        pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base,
                                    (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base,
                                    (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
        pkt->pos = -1;

        int64_t delay_usec = 0;
        if (out_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            int64_t dts_usec = av_rescale_q(pkt->dts, out_stream->time_base, AV_TIME_BASE_Q);
            delay_usec = dts_usec - (now - start_time);
        }

        if (codec_ctx_mapping_.find(pkt->stream_index) != codec_ctx_mapping_.end()) {
            if (decode_packet(codec_ctx_mapping_[pkt->stream_index], pkt, frame) != 0) {
                AVStream *stream = ofmt_ctx_->streams[pkt->stream_index];
#ifdef DEBUG
                fprintf(stderr, "Decode packet failed, stream_index:%d type:%s size:%d\n",
                        pkt->stream_index, av_get_media_type_string(stream->codecpar->codec_type),
                        pkt->size);
#endif
                continue;
            }
        }
        
        logPacket(ofmt_ctx_, out_stream->codecpar, pkt, frame);

        if (av_interleaved_write_frame(ofmt_ctx_, pkt) < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }

        av_frame_unref(frame);
        av_packet_unref(pkt);

        /* control data speed */
        if (delay_usec > 0) {
            av_usleep(delay_usec);
        }
    }
    av_write_trailer(ofmt_ctx_);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    return 0;
}

class Player {
    public:
        Player(const std::string input):input_(input) {
            ifmt_ctx_ = NULL;
            packet_logger_ = new Logger();
            codec_info_logger_ = new Logger();
            stream_state_logger_ = new Logger();
        };
        
        ~Player() {
            for (auto it : dec_ctx_mapping_) {
                avcodec_free_context(&it.second);
            }
            release(packet_logger_);
        };
        int SetPacketLogPath(const std::string path);
        int SetCodecInfoLogPath(const std::string path);
        int SetMetadataLogPath(const std::string path);
        int SetStreamStateLogPath(const std::string path);
        int Start();
    private:
        int logPacket(const AVFormatContext *fmt_ctx, const AVCodecParameters *codec_par, const AVPacket *pkt, const AVFrame *frame);
        int logCodecInfo(const AVCodecParameters *codec_par);
        int logMetadata(const AVDictionary *metadata);
        int logStreamState(const int fps, const int bitrate, const uint64_t duration, uint64_t bytes);
    private:
        std::string input_;
        AVFormatContext *ifmt_ctx_;
        std::map<int, AVCodecContext*> dec_ctx_mapping_;

        Logger *packet_logger_;
        Logger *codec_info_logger_;
        Logger *metadata_logger_;
        Logger *stream_state_logger_;
};

int Player::logPacket(const AVFormatContext *fmt_ctx, const AVCodecParameters *codec_par, const AVPacket *pkt, const AVFrame *frame) {
    std::string media_type = av_get_media_type_string(codec_par->codec_type);
    char pic_type = '-';
    if (media_type == "video") {
        pic_type = av_get_picture_type_char(frame->pict_type);
    }
    packet_logger_->Write("<%s> pic_type:%c pts:%s dts:%s duration:%s size:%d",
                        media_type.c_str(), pic_type,
                        av_ts2str(pkt->pts), av_ts2str(pkt->dts),
                        av_ts2str(pkt->duration), pkt->size);
    return 0;
}

int Player::logCodecInfo(const AVCodecParameters *codec_par) {
    if (codec_par == NULL) return -1;
    const AVCodecDescriptor *des = avcodec_descriptor_get(codec_par->codec_id);
    switch (codec_par->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            codec_info_logger_->Write("Video: codec=%s height=%d width=%d", des->name, codec_par->height, codec_par->width);
            break;
        case AVMEDIA_TYPE_AUDIO:
            codec_info_logger_->Write("Audio: codec=%s sample_rate=%d", des->name, codec_par->sample_rate);
            break;
        default:
            break;
    }
    return 0;
}

int Player::logMetadata(const AVDictionary *metadata) {
    if (metadata == NULL) return -1;
    std::string meta_str;
    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if(meta_str.empty()) {
            meta_str = "{" + std::string(tag->key) + ":" + std::string(tag->value) + "}";
        } else {
            meta_str += ", {" + std::string(tag->key) + ":" + std::string(tag->value) + "}";
        }
    }

    std::string log_str = "metadata[" + meta_str + "]";
    metadata_logger_->Write("%s", log_str.c_str());
    return -1;
}

/**
 * Print stream state log
 * @param fps, frames in one second
 * @param bitrate in bps
 * @param duration in millisecond
 * @param bytes of current total stream bytes
 */
int Player::logStreamState(const int fps, const int bitrate, const uint64_t duration, uint64_t bytes) {
    stream_state_logger_->Write("fps=%d bitrate=%.3fkbps duration=%.3fs bytes=%" PRIu64, fps, (float)bitrate/1000, (float)duration/1000, bytes);
    return 0;
}

int Player::SetPacketLogPath(const std::string path) {
    release(packet_logger_);
    packet_logger_ = new Logger(path);
    return 0;
}

int Player::SetCodecInfoLogPath(const std::string path) {
    release(codec_info_logger_);
    codec_info_logger_ = new Logger(path);
    return 0;
}

int Player::SetMetadataLogPath(const std::string path) {
    release(metadata_logger_);
    metadata_logger_ = new Logger(path);
    return 0;
}

int Player::SetStreamStateLogPath(const std::string path) {
    release(stream_state_logger_);
    stream_state_logger_ = new Logger(path);
    return 0;
}

int Player::Start() {
    /* open input file, and allocate format context */
    if (avformat_open_input(&ifmt_ctx_, input_.c_str(), NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", input_.c_str());
        return -1;
    }

    /* retrieve stream information */
    if (avformat_find_stream_info(ifmt_ctx_, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        return -1;
    }

    /* open codec context */
    for (int i = 0; i < ifmt_ctx_->nb_streams; i++) {
        AVStream *stream = ifmt_ctx_->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;
        if (codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            continue;
        }
        logCodecInfo(codecpar);
        AVCodec *dec = avcodec_find_decoder(codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Open decoder failed");
            continue;
        }

        AVCodecContext *dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) {
            fprintf(stderr, "Alloc context failed");
            continue;
        }

        if (avcodec_parameters_to_context(dec_ctx, codecpar) < 0) {
            fprintf(stderr, "Failed to copy codec parameters");
            continue;
        }

        if (avcodec_open2(dec_ctx, dec, NULL) < 0) {
            fprintf(stderr, "Faild to open codec");
            continue;
        }
        dec_ctx_mapping_[i] = dec_ctx;
    }
    
    // av_dump_format(ifmt_ctx_, 0, input_.c_str(), 0);
    logMetadata(ifmt_ctx_->metadata);
    
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint64_t start_time = av_gettime();
    int fps = 0;
    int64_t last_fps_count_time = start_time;
    uint64_t bytes = 0;
    uint64_t last_count_bytes = 0;
    while(av_read_frame(ifmt_ctx_, pkt) >= 0) {
        AVStream *stream = ifmt_ctx_->streams[pkt->stream_index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            fps++;
        }
        bytes += pkt->size;
        uint64_t now = av_gettime();
        if ((now - last_fps_count_time) >= 1000*1000) {
            logStreamState(fps, (bytes - last_count_bytes)*8, (now - start_time)/(1000), bytes);
            fps = 0;
            last_fps_count_time = now;
            last_count_bytes = bytes;
        }

        if (dec_ctx_mapping_.find(pkt->stream_index) != dec_ctx_mapping_.end()) {
            if (decode_packet(dec_ctx_mapping_[pkt->stream_index], pkt, frame) != 0) {
#ifdef DEBUG
                fprintf(stderr, "Decode packet failed, stream_index:%d type:%s size:%d\n",
                        pkt->stream_index, av_get_media_type_string(stream->codecpar->codec_type),
                        pkt->size);
#endif
                av_packet_unref(pkt);
                continue;
            }
        }
        logPacket(ifmt_ctx_, stream->codecpar, pkt, frame);
        av_packet_unref(pkt);
        av_frame_unref(frame);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    return 0;
}

/** 
 * A simple class of parsing command line options, from:
 * https://stackoverflow.com/questions/865668/how-to-parse-command-line-arguments-in-c
 */
class CmdParser{
    public:
        CmdParser (int &argc, char **argv){
            for (int i=1; i < argc; ++i)
                this->tokens.push_back(std::string(argv[i]));
        }

        const std::string& GetCmdOption(const std::string &option) const{
            std::vector<std::string>::const_iterator itr;
            itr =  std::find(this->tokens.begin(), this->tokens.end(), option);
            if (itr != this->tokens.end() && ++itr != this->tokens.end()){
                return *itr;
            }
            static const std::string empty_string("");
            return empty_string;
        }

        bool CmdOptionExists(const std::string &option) const{
            return std::find(this->tokens.begin(), this->tokens.end(), option)
                   != this->tokens.end();
        }
    private:
        std::vector <std::string> tokens;
};

int main(int argc, char **argv) {
    av_register_all();
    // avformat_network_init();
    CmdParser cmd_parser(argc, argv);
    if (cmd_parser.CmdOptionExists("-i")) {
        if (cmd_parser.CmdOptionExists("-o")) { // publish
            std::string input = cmd_parser.GetCmdOption("-i");
            std::string output = cmd_parser.GetCmdOption("-o");
            Publisher publisher(input, output);
            std::string packet_log = cmd_parser.GetCmdOption("-packet_log");
            if (!packet_log.empty()) {
                publisher.SetPacketLogPath(packet_log);
            }
            std::string codec_info_log = cmd_parser.GetCmdOption("-codec_info_log");
            if (!codec_info_log.empty()) {
                publisher.SetCodecInfoLogPath(codec_info_log);
            }
            std::string metadata_log = cmd_parser.GetCmdOption("-metadata_log");
            if (!metadata_log.empty()) {
                publisher.SetMetadataLogPath(metadata_log);
            }
            std::string stream_state_log = cmd_parser.GetCmdOption("-stream_state_log");
            if (!stream_state_log.empty()) {
                publisher.SetStreamStateLogPath(stream_state_log);
            }
            publisher.Start();
        } else { // play
            std::string input = cmd_parser.GetCmdOption("-i");
            Player player(input);
            std::string packet_log = cmd_parser.GetCmdOption("-packet_log");
            if (!packet_log.empty()) {
                player.SetPacketLogPath(packet_log);
            }
            std::string codec_info_log = cmd_parser.GetCmdOption("-codec_info_log");
            if (!codec_info_log.empty()) {
                player.SetCodecInfoLogPath(codec_info_log);
            }
            std::string metadata_log = cmd_parser.GetCmdOption("-metadata_log");
            if (!metadata_log.empty()) {
                player.SetMetadataLogPath(metadata_log);
            }
            std::string stream_state_log = cmd_parser.GetCmdOption("-stream_state_log");
            if (!stream_state_log.empty()) {
                player.SetStreamStateLogPath(stream_state_log);
            }
            player.Start();
        }
    } else {
        printf("Usage:\n"
               "\t-i: input filename or url\n"
               "\t-o: publish url\n"
               "\t-packet_log: packet log path\n"
               "\t-codec_info_log: codec infomation log path\n"
               "\t-metadata_log: metadata log path\n"
               "\t-stream_state_log: stream state log path\n");
    }

    return 0;
}