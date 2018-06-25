PWM = pwm
CC = gcc
OBJ_DIR = obj

CFLAGS = -std=gnu99 -Wall -Wextra -Werror -DDEBUG
LDFLAGS = -lm

SRC = pwm.c

OBJS  = $(addsuffix .o,$(addprefix $(OBJ_DIR)/,$(basename $(SRC))))

all: $(PWM)

$(PWM): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

MKDIR_OBJDIR = @mkdir -p $(dir $@)

$(OBJ_DIR)/%.o: %.c
	$(MKDIR_OBJDIR)
	@$(CC) -c -o $@ $(CFLAGS) $<


test_sbus: $(PWM)
	./$(PWM) -s 2000 -b -d values.csv <test_data_sbus >r
