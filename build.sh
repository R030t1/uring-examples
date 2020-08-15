g++ -std=gnu++2a -Wall -pedantic \
	uring-cp.cc \
	-o uring-cp \
	$(pkg-config liburing --cflags --libs)
