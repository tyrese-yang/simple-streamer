## simple-streamer
一个基于FFmpeg简单的推拉流程序，可以将推拉流的不同类型日志记录不同的文件中

### 编译
- g++ -o sstreamer simple-streamer.cc -lavformat -lavcodec -std=c++11

### 用法
只有**-i**参数时为拉流，**-i**和**-o**同时存在时为推流，没有指定日志路径时默认为标准输出
`-i`: 指定输入的流  
`-o`: 指定输出的流  
`-packet_log`: 指定帧日志的路径  
`-codec_info_log`: 指定编解码器的信息日志的路径  
`-metadata_log`: 指定metadata信息日志的路径  
`-stream_state_log`: 指定流状态日志的路径  

### Example
推流
- ./sstreamer -i xxx.flv -o rtmp://xxx/app/stream -packet_log packet.log -metadata_log metadata.log -stream_state_log stream_state.log
拉流
- ./sstreamer -i rtmp://xxx/app/stream -packet_log packet.log -metadata_log metadata.log -stream_state_log stream_state.log