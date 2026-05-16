NAME    = ircserv
CXX     = c++
CXXFLAGS = -Wall -Wextra -Werror -std=c++98

SRC_DIR = src
SRCS    = $(SRC_DIR)/main.cpp \
          $(SRC_DIR)/Server.cpp \
          $(SRC_DIR)/Client.cpp \
          $(SRC_DIR)/Channel.cpp

OBJS    = $(SRCS:.cpp=.o)

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(NAME) $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re