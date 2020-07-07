
all:
	g++ build.cpp -march=native -O3 -ffast-math -fno-associative-math -o build.x -std=c++11
	g++ query.cpp -static -march=native -O3 -ffast-math -fno-associative-math -o query.x -std=c++11
	g++ warm.cpp -march=native -O3 -ffast-math -fno-associative-math -o warm.x -std=c++11

clean:
	rm *.x
