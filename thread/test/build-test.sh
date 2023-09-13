g++ -std=c++17 -I ../../include/ -I../../third_party/include -DThreadUT test.cpp x.cpp \
	../thread-pool.cpp ../workerpool.cpp ../thread-key.cpp \
	../../common/identity-pool.cpp ../../common/alog.cpp -lgtest -lgflags -O2

