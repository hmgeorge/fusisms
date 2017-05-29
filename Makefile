CFLAGS=`pkg-config zlib --cflags`
LIBS=`pkg-config zlib --libs`

pack-reader: git-pack-reader.cc memory-mapped-file.cc utils.cc 
	g++ -std=c++11 git-pack-reader.cc memory-mapped-file.cc utils.cc $(LIBS) -o pack-reader

.PHONY: clean
clean:
	rm pack-reader *~
