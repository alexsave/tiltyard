OBJDIR := out
OUTOBJS := $(addprefix $(OBJDIR)/, rand.o pq.o xpq.o fl.o bs.o sch.o client_zero.o cb.o client_one.o holder.o client.o ob.o mbp.o server.o utils.o)

FLAGS := -g -Wunused-function -Wunused-label -Wunused-value -Wunused-variable -Wunused-parameter -Wunused-but-set-parameter -ferror-limit=100

out/%.o: src/%.c
	gcc $(FLAGS) -c -Iinclude $< -o $@

out/%.o: src/strategy/%.c
	gcc $(FLAGS) -c -Iinclude $< -o $@

$(OUTOBJS): | $(OBJDIR)

$(OBJDIR):
	mkdir $(OBJDIR)

clean:
	rm -rf out


# test relies on out
test: clean $(OUTOBJS)
	gcc $(FLAGS) -I include/ tests/main.c $(OUTOBJS) -o out/test && ./out/test

main: clean $(OUTOBJS)
	gcc $(FLAGS) -I include/ src/main.c $(OUTOBJS) -o ./tiltyard && ./tiltyard

debug: clean $(OUTOBJS)
	gcc $(FLAGS) -I include/ src/main.c $(OUTOBJS) -o ./tiltyard && ./tiltyard

