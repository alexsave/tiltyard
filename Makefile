OBJDIR := out
OUTOBJS := $(addprefix $(OBJDIR)/, rand.o pq.o fl.o)

out/%.o: src/%.c
	gcc -c -Iinclude $< -o $@

$(OUTOBJS): | $(OBJDIR)

$(OBJDIR):
	mkdir $(OBJDIR)

clean:
	rm -rf out

# test relies on out
test: $(OUTOBJS)
	gcc -I include/ tests/main.c $(OUTOBJS) -o out/test && ./out/test

main: $(OUTOBJS)
	gcc -I include/ src/main.c $(OUTOBJS) -o ./tiltyard && ./tiltyard

