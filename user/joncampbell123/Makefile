all: ffmpeg_dump_cc

ffmpeg_dump_cc: ffmpeg_dump_cc.o
	g++ -pthread -o $@ $< ../../libavutil/libavutil.a ../../libavcodec/libavcodec.a ../../libavformat/libavformat.a ../../libavutil/libavutil.a ../../libswresample/libswresample.a ../../libavutil/libavutil.a ../../libavcodec/libavcodec.a ../../libavformat/libavformat.a -lvorbis -ltheora -lrt -lz -lbz2 -llzma -logg -lvorbis -lx264 -lx265 -lvpx -lxvidcore -lvorbisenc -ltheoraenc -ltheoradec -lspeex -lgsm -lmp3lame -lschroedinger-1.0 -lopenjpeg -lopencore-amrnb -lopencore-amrwb

ffmpeg_dump_cc.o: ffmpeg_dump_cc.cpp
	g++ -pthread -I../../ -c -o $@ $<

clean:
	rm -f *.o
	rm -f ffmpeg_dump_cc

