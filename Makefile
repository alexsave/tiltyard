OBJDIR := out

OUTSRCS := $(filter-out src/main.c, $(shell find src -name '*.c'))
OUTOBJS := $(addprefix $(OBJDIR)/, $(notdir $(OUTSRCS:.c=.o)))

FLAGS := -g -O3 -MMD -MP -Wunused-function -Wunused-label -Wunused-value -Wunused-variable -Wunused-parameter -Wunused-but-set-parameter -ferror-limit=100

VPATH := $(sort $(dir $(OUTSRCS)))

out/%.o: %.c
	gcc $(FLAGS) -c -Iinclude $< -o $@

$(OUTOBJS): | $(OBJDIR)

# Pull in the generated header dependencies (see -MMD above).
-include $(OUTOBJS:.o=.d)

$(OBJDIR):
	mkdir $(OBJDIR)

clean:
	rm -rf out


test: $(OUTOBJS)
	gcc $(FLAGS) -I include/ tests/main.c $(OUTOBJS) -o out/test && ./out/test

main: $(OUTOBJS)
	gcc $(FLAGS) -I include/ src/main.c $(OUTOBJS) -o ./tiltyard && ./tiltyard

debug: $(OUTOBJS)
	gcc $(FLAGS) -I include/ src/main.c $(OUTOBJS) -o ./tiltyard && ./tiltyard

