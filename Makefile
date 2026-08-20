NAME     = ircserv
CXX      = c++
CXXFLAGS = -Wall -Wextra -Werror -std=c++98

SRC_DIR = src
SRCS    = $(SRC_DIR)/main.cpp \
          $(SRC_DIR)/Server.cpp \
          $(SRC_DIR)/Client.cpp \
          $(SRC_DIR)/Channel.cpp

OBJS    = $(SRCS:.cpp=.o)

# Every header the sources include. Listing them as prerequisites of the .o
# rule is what makes "touch a header -> only the affected .o is rebuilt"
# work, while still avoiding a full relink when nothing changed.
HEADERS = $(SRC_DIR)/Server.hpp \
          $(SRC_DIR)/Client.hpp \
          $(SRC_DIR)/Channel.hpp \
          $(SRC_DIR)/Ircmessage.hpp

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(NAME) $(OBJS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re