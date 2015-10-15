PWM = pwm
CC = gcc
OBJ_DIR = obj

CFLAGS = -std=gnu99 -Wall
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


