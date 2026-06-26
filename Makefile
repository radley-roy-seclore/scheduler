CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread

TARGET = scheduler

SRC = main.cpp \
      Scheduler.cpp \
      Job.cpp \
      ComputeJob.cpp \
      Worker.cpp \
      JobQueue.cpp \
      Logger.cpp \
      Utils.cpp

OBJ = $(SRC:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJ)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(OBJ) $(TARGET)
